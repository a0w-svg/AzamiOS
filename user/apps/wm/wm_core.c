/**
 * wm_core.c — AzamiOS Modular Window Manager Core Engine v5.0
 *
 * NEW IN v5.0:
 *   - O(1) Service Lookup:   wm_get_service() uses direct table indexing (g_service_map)
 *   - O(1) Window Lookup:    find_win_by_type() uses direct table indexing (g_win_map)
 *
 * NEW IN v4.0:
 *   - g_de:                  Active Desktop Environment pointer (swappable at runtime)
 *   - g_theme:               Active runtime color theme
 *   - g_notif_queue:         Toast notification ring buffer
 *   - wm_switch_de():        Atomically swaps g_de, triggers full redraw
 *   - wm_switch_theme():     Swaps g_theme palette
 *   - wm_push_notification():Enqueues a timed toast message
 *   - wm_tick_notifications():Called each dirty frame to age/expire toasts
 *   - Window chrome routing: render_window() now calls g_de->render_window_chrome()
 *
 * SAFETY PHILOSOPHY (unchanged):
 *   1. Safe String Manipulation:  wm_strlcpy prevents buffer overflows.
 *   2. Parameter Clamping:        Coordinates are always clamped to screen bounds.
 *   3. Modularity:                Service Registry decouples app logic from core UI.
 *   4. NULL Guards:               Every pointer is validated before dereference.
 */

#include "wm.h"

/* ── Global state ───────────────────────────────────────────────────────────── */
window_t g_wins[MAX_WINS];
int g_num_wins = 0;
int g_focus    = -1; /* -1 = no focus yet; set in init_wins */

/* ── DE / Theme / Notification globals ──────────────────────────────────────── */
/* g_themes[], g_theme_filenames[], g_num_themes defined in wm_config.c */
wm_de_t          *g_de    = NULL;
wm_theme_t       *g_theme = NULL;
wm_notification_t g_notif_queue[WM_NOTIF_MAX];

/* ── Safe String Helper ──────────────────────────────────────────────────── */
/**
 * wm_strlcpy: Copies up to max_len-1 chars from src to dst, guarantees
 * null-termination. Prevents buffer overflows in title bars and labels.
 */
void wm_strlcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i < max_len - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── Secure Window Mutators (Safe API) ───────────────────────────────────── */
void wm_set_title(window_t *w, const char *title) {
    if (!w || !title) return;
    wm_strlcpy(w->title, title, sizeof(w->title));
}

void wm_resize_window(window_t *w, int new_w, int new_h) {
    if (!w) return;
    if (new_w < 64)      new_w = 64;
    if (new_h < 40)      new_h = 40;
    if (new_w > SCREEN_W) new_w = SCREEN_W;
    if (new_h > DESKTOP_H) new_h = DESKTOP_H;
    w->w = new_w;
    w->h = new_h;
}

void wm_close_window(window_t *w) {
    if (!w || !w->open) return;
    w->open = false;
    const wm_service_t *srv = wm_get_service(w->type);
    if (srv && srv->on_close) {
        srv->on_close(w);
    }
}

/* ── DE / Theme switching ────────────────────────────────────────────────── */
/* wm_switch_de() and wm_switch_theme() are defined in wm_config.c because
 * they depend on wm_get_de() / wm_get_theme() and the internal _wm_apply_*
 * no-save helpers that live in that translation unit. */


/* ── Toast Notification System ───────────────────────────────────────────── */
/**
 * wm_push_notification: Enqueues a timed toast into the ring buffer.
 * If all slots are full, the oldest (lowest ticks) slot is evicted.
 * SAFETY: msg is bounded to WM_NOTIF_MAX chars via wm_strlcpy.
 */
void wm_push_notification(const char *msg, uint32_t color) {
    if (!msg) return;
    /* Find a free slot, or the slot with the fewest ticks left */
    int target = 0;
    int min_ticks = WM_NOTIF_TICKS + 1;
    for (int i = 0; i < WM_NOTIF_MAX; i++) {
        if (!g_notif_queue[i].active) {
            target = i;
            min_ticks = -1;
            break;
        }
        if (g_notif_queue[i].ticks < min_ticks) {
            min_ticks = g_notif_queue[i].ticks;
            target = i;
        }
    }
    wm_strlcpy(g_notif_queue[target].msg, msg, sizeof(g_notif_queue[target].msg));
    g_notif_queue[target].color  = color;
    g_notif_queue[target].ticks  = WM_NOTIF_TICKS;
    g_notif_queue[target].active = true;
}

/**
 * wm_tick_notifications: Decrements toast timers each dirty frame.
 * Expired toasts are marked inactive and their slot is freed.
 */
void wm_tick_notifications(void) {
    for (int i = 0; i < WM_NOTIF_MAX; i++) {
        if (g_notif_queue[i].active) {
            g_notif_queue[i].ticks--;
            if (g_notif_queue[i].ticks <= 0) {
                g_notif_queue[i].active = false;
            }
        }
    }
}

/* ── Service Registry (Modularity Engine) ────────────────────────────────── */
#define MAX_SERVICES 32
static const wm_service_t *g_services[MAX_SERVICES];
static const wm_service_t *g_service_map[MAX_WINS]; /* O(1) direct service lookup */
static int g_win_map[MAX_WINS];                     /* O(1) direct window lookup */
static int g_num_services = 0;

void wm_register_service(const wm_service_t *service) {
    if (!service) return;
    if (g_num_services < MAX_SERVICES) {
        g_services[g_num_services++] = service;
        if (service->type >= 0 && service->type < MAX_WINS) {
            g_service_map[service->type] = service;
        }
    }
}

const wm_service_t *wm_get_service(int type) {
    if (type >= 0 && type < MAX_WINS) {
        return g_service_map[type];
    }
    return NULL;
}

int wm_get_service_count(void) { return g_num_services; }

const wm_service_t *wm_get_service_by_index(int idx) {
    if (idx < 0 || idx >= g_num_services) return NULL;
    return g_services[idx];
}

/* ── Window Management & Lifecycle ───────────────────────────────────────── */
void init_wins(void) {
    /* Pre-defined window slots for 1280×800 resolution */
    g_wins[0] = (window_t){0, "", 200, 100, 560, 420, 200, 100, 560, 420, true,  false, false, 0, WIN_WELCOME};
    wm_set_title(&g_wins[0], "Welcome");
    g_wins[1] = (window_t){1, "", 240, 60,  680, 480, 240, 60,  680, 480, false, false, false, 0, WIN_TERMINAL};
    wm_set_title(&g_wins[1], "Terminal");
    g_wins[2] = (window_t){2, "", 180, 140, 500, 360, 180, 140, 500, 360, false, false, false, 0, WIN_SYSMON};
    wm_set_title(&g_wins[2], "System Monitor");
    g_wins[3] = (window_t){3, "", 300, 180, 420, 320, 300, 180, 420, 320, false, false, false, 0, WIN_ABOUT};
    wm_set_title(&g_wins[3], "About AzamiOS");
    g_wins[4] = (window_t){4, "", 260, 80,  600, 440, 260, 80,  600, 440, false, false, false, 0, WIN_NOTEPAD};
    wm_set_title(&g_wins[4], "Notepad");
    g_wins[5] = (window_t){5, "", 180, 120, 540, 400, 180, 120, 540, 400, false, false, false, 0, WIN_FILES};
    wm_set_title(&g_wins[5], "File Manager");
    g_wins[6] = (window_t){6, "", 220, 80,  500, 420, 220, 80,  500, 420, false, false, false, 0, WIN_GLCUBE};
    wm_set_title(&g_wins[6], "3D OpenGL Demo");
    g_wins[7] = (window_t){7, "", 160, 60,  520, 400, 160, 60,  520, 400, false, false, false, 0, WIN_PONG};
    wm_set_title(&g_wins[7], "Arcade Pong");
    g_wins[8] = (window_t){8, "", 160, 80,  640, 480, 160, 80,  640, 480, false, false, false, 0, WIN_IDE};
    wm_set_title(&g_wins[8], "AzamiCC IDE");
    g_wins[9] = (window_t){9, "", 300, 140, 520, 440, 300, 140, 520, 440, false, false, false, 0, WIN_SETTINGS};
    wm_set_title(&g_wins[9], "Settings");
    g_wins[10] = (window_t){10, "", 280, 120, 440, 380, 280, 120, 440, 380, false, false, false, 0, WIN_SNAKE};
    wm_set_title(&g_wins[10], "Snake");
    g_wins[11] = (window_t){11, "", 320, 100, 420, 460, 320, 100, 420, 460, false, false, false, 0, WIN_MINESWEEPER};
    wm_set_title(&g_wins[11], "Minesweeper");
    g_wins[12] = (window_t){12, "", 340, 80,  320, 520, 340, 80,  320, 520, false, false, false, 0, WIN_TETRIS};
    wm_set_title(&g_wins[12], "Tetris");
    g_wins[13] = (window_t){13, "", 200, 160, 680, 480, 200, 160, 680, 480, false, false, false, 0, WIN_PYTHON};
    wm_set_title(&g_wins[13], "Python REPL");
    g_wins[14] = (window_t){14, "", 160, 100, 720, 520, 160, 100, 720, 520, false, false, false, 0, WIN_DEBUGGER};
    wm_set_title(&g_wins[14], "Kernel Debugger");
    g_wins[15] = (window_t){15, "", 140, 80,  760, 540, 140, 80,  760, 540, false, false, false, 0, WIN_BROWSER};
    wm_set_title(&g_wins[15], "Lynx Browser");
    g_wins[16] = (window_t){16, "", 220, 120, 580, 420, 220, 120, 580, 420, false, false, false, 0, WIN_NETMON};
    wm_set_title(&g_wins[16], "Network Monitor");
    g_num_wins = 17;

    /* Populate O(1) window lookup table */
    for (int i = 0; i < MAX_WINS; i++) g_win_map[i] = -1;
    for (int i = 0; i < g_num_wins; i++) {
        if (g_wins[i].type >= 0 && g_wins[i].type < MAX_WINS) {
            g_win_map[g_wins[i].type] = i;
        }
    }

    /* Ensure a sane initial focus: first open window, or -1 if none. */
    g_focus = -1;
    for (int i = 0; i < g_num_wins; i++) {
        if (g_wins[i].open) { g_focus = i; break; }
    }

    /* Initialize modular services */
    for (int i = 0; i < g_num_services; i++) {
        if (g_services[i] && g_services[i]->on_init) {
            int idx = find_win_by_type(g_services[i]->type);
            if (idx >= 0) g_services[i]->on_init(&g_wins[idx]);
        }
    }

    /* Initialize notification queue */
    for (int i = 0; i < WM_NOTIF_MAX; i++) {
        g_notif_queue[i].active = false;
        g_notif_queue[i].ticks  = 0;
    }
}

int find_win_by_type(int type) {
    if (type >= 0 && type < MAX_WINS) {
        return g_win_map[type];
    }
    return -1;
}

void open_win_type(int type) {
    int idx = find_win_by_type(type);
    if (idx >= 0) {
        g_wins[idx].open      = true;
        g_wins[idx].minimized = false;
        g_focus               = idx;
        const wm_service_t *srv = wm_get_service(type);
        if (srv && srv->on_open) srv->on_open(&g_wins[idx]);
    }
}

/* ── Window Chrome (routed through g_de) ─────────────────────────────────── */
/**
 * render_window: Renders a complete window including chrome + body.
 * The chrome (frame, titlebar, buttons) is delegated to g_de->render_window_chrome()
 * so each DE can express its own visual style without any code change here.
 */
void render_window(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    if (!w || !w->open || w->minimized) return;

    /* SAFETY: Clamp boundaries to prevent out-of-bounds framebuffer writes */
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > SCREEN_W) w->w = SCREEN_W - w->x;
    if (w->y + w->h > DESKTOP_H) w->h = DESKTOP_H - w->y;
    if (w->w <= 0 || w->h <= TITLEBAR_H) return;

    bool is_active = (g_focus == w->id);

    /* Route chrome rendering through active DE */
    if (g_de && g_de->render_window_chrome) {
        g_de->render_window_chrome(w, is_active);
    }

    /* Body content is always rendered by the service */
    render_window_body(w, t, frame_cnt, blink);
}

void render_window_body(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    if (!w) return;
    const wm_service_t *srv = wm_get_service(w->type);
    if (srv && srv->on_render) {
        srv->on_render(w, t, frame_cnt, blink);
    } else {
        draw_rect(w->x + 1, w->y + TITLEBAR_H, w->w - 2, w->h - TITLEBAR_H - 1, COL_WIN_BODY);
    }
}
