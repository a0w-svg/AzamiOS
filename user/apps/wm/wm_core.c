/**
 * wm_core.c — AzamiOS Secure & Modular Window Manager Core Engine
 *
 * EDUCATIONAL EXPLANATIONS & SAFETY PHILOSOPHY:
 * 1. Safe String Manipulations:
 *    Legacy C functions like `strcpy` or `sprintf` do not verify buffer limits,
 *    leading to stack-based buffer overflow exploits. Here, `wm_strlcpy` strictly
 *    bounds all copies to the allocated destination size.
 * 2. Parameter Clamping & Bounds Verification:
 *    Window rendering algorithms interact directly with low-level graphical memory buffers.
 *    If a window's dimensions (x, y, w, h) exceed screen bounds or become negative,
 *    subsequent pixel writes can corrupt unallocated framebuffer regions or kernel memory.
 *    `render_window` and `wm_resize_window` enforce rigid coordinate clamps.
 * 3. Modularity & Decoupling:
 *    The Service Registry (`g_services`) isolates application dependencies from core UI routing.
 */

#include "wm.h"

/* ── Global state ────────────────────────────────────────────────────────── */
window_t g_wins[MAX_WINS];
int g_num_wins = 7;
int g_focus = 0;

/* ── Safe String Helper ──────────────────────────────────────────────────── */
/**
 * wm_strlcpy copies up to max_len - 1 characters from src to dst, guaranteeing
 * null-termination. This prevents buffer overflows across UI title bars.
 */
void wm_strlcpy(char *dst, const char *src, size_t max_len) {
    if (!dst || max_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
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
    /* Enforce strict safety limits to avoid memory corruption or invisible windows */
    if (new_w < 64) new_w = 64;
    if (new_h < 40) new_h = 40;
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

/* ── Service Registry (Modularity Engine) ────────────────────────────────── */
#define MAX_SERVICES 16
static const wm_service_t *g_services[MAX_SERVICES];
static int g_num_services = 0;

void wm_register_service(const wm_service_t *service) {
    if (!service) return; /* Security guard against null dereference */
    if (g_num_services < MAX_SERVICES) {
        g_services[g_num_services++] = service;
    }
}

const wm_service_t *wm_get_service(int type) {
    for (int i = 0; i < g_num_services; i++) {
        if (g_services[i] && g_services[i]->type == type) {
            return g_services[i];
        }
    }
    return NULL;
}

int wm_get_service_count(void) {
    return g_num_services;
}

const wm_service_t *wm_get_service_by_index(int idx) {
    if (idx < 0 || idx >= g_num_services) return NULL;
    return g_services[idx];
}

/* ── Window Management & Lifecycle ───────────────────────────────────────── */
void init_wins(void) {
    g_wins[0] = (window_t){0, "", 140, 60, 360, 260, 140, 60, 360, 260, true, false, false, WIN_WELCOME};
    wm_set_title(&g_wins[0], "Welcome");
    g_wins[1] = (window_t){1, "", 180, 40, 410, 254, 180, 40, 410, 254, false, false, false, WIN_TERMINAL};
    wm_set_title(&g_wins[1], "Terminal");
    g_wins[2] = (window_t){2, "", 130, 90, 380, 240, 130, 90, 380, 240, false, false, false, WIN_SYSMON};
    wm_set_title(&g_wins[2], "System Monitor");
    g_wins[3] = (window_t){3, "", 160, 70, 320, 280, 160, 70, 320, 280, false, false, false, WIN_ABOUT};
    wm_set_title(&g_wins[3], "About AzamiOS");
    g_wins[4] = (window_t){4, "", 200, 50, 380, 240, 200, 50, 380, 240, false, false, false, WIN_NOTEPAD};
    wm_set_title(&g_wins[4], "Notepad");
    g_wins[5] = (window_t){5, "", 120, 80, 400, 260, 120, 80, 400, 260, false, false, false, WIN_FILES};
    wm_set_title(&g_wins[5], "File Manager");
    g_wins[6] = (window_t){6, "", 150, 50, 420, 320, 150, 50, 420, 320, false, false, false, WIN_GLCUBE};
    wm_set_title(&g_wins[6], "3D OpenGL Demo");
    g_num_wins = 7;

    /* Initialize registered modular services */
    for (int i = 0; i < g_num_services; i++) {
        if (g_services[i] && g_services[i]->on_init) {
            int idx = find_win_by_type(g_services[i]->type);
            if (idx >= 0) g_services[i]->on_init(&g_wins[idx]);
        }
    }
}

int find_win_by_type(int type) {
    for (int i = 0; i < g_num_wins; i++)
        if (g_wins[i].type == type) return i;
    return -1;
}

void open_win_type(int type) {
    int idx = find_win_by_type(type);
    if (idx >= 0) {
        g_wins[idx].open = true;
        g_wins[idx].minimized = false;
        g_focus = idx;
        const wm_service_t *srv = wm_get_service(type);
        if (srv && srv->on_open) {
            srv->on_open(&g_wins[idx]);
        }
    }
}

/* ── Draw titlebar buttons ───────────────────────────────────────────────── */
static void draw_titlebar_buttons(window_t *w) {
    if (!w) return;
    int bx = w->x + w->w - 20;
    int by = w->y + 4;
    /* Close [X] */
    draw_rect(bx, by, 16, 14, COL_BTN_CLOSE);
    draw_text(bx + 4, by + 3, "X", COL_TEXT_WHITE, COL_BTN_CLOSE);
    /* Maximize [ ] */
    bx -= 20;
    draw_rect(bx, by, 16, 14, COL_BTN_MAX);
    draw_rect(bx + 3, by + 3, 10, 8, COL_BTN_MAX);
    draw_rect(bx + 4, by + 4, 8, 6, COL_TEXT_WHITE);
    draw_rect(bx + 4, by + 4, 8, 2, COL_BTN_MAX);
    /* Minimize [_] */
    bx -= 20;
    draw_rect(bx, by, 16, 14, COL_BTN_MIN);
    draw_rect(bx + 4, by + 9, 8, 2, COL_TEXT_WHITE);
}

/* ── Render window body content ──────────────────────────────────────────── */
void render_window_body(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    if (!w) return;
    const wm_service_t *srv = wm_get_service(w->type);
    if (srv && srv->on_render) {
        srv->on_render(w, t, frame_cnt, blink);
    } else {
        draw_rect(w->x + 1, w->y + TITLEBAR_H, w->w - 2, w->h - TITLEBAR_H - 1, COL_WIN_BODY);
    }
}

/* ── Render a single window with boundary safety checks ──────────────────── */
void render_window(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    if (!w || !w->open || w->minimized) return;

    /* SAFETY ENFORCEMENT: Clamp boundaries to prevent out-of-bounds GUI writes */
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    if (w->x + w->w > SCREEN_W) w->w = SCREEN_W - w->x;
    if (w->y + w->h > DESKTOP_H) w->h = DESKTOP_H - w->y;
    if (w->w <= 0 || w->h <= TITLEBAR_H) return;

    bool is_active = (g_focus == w->id);
    uint32_t bar_col = is_active ? COL_TITLE_ACT : COL_TITLE_INA;

    /* Drop shadow */
    draw_rect(w->x + 4, w->y + 4, w->w, w->h, COL_WIN_SHAD);
    /* Window frame */
    draw_rect(w->x, w->y, w->w, w->h, COL_WIN_FRAME);
    /* Titlebar */
    draw_rect(w->x, w->y, w->w, TITLEBAR_H, bar_col);
    draw_text(w->x + 8, w->y + 7, w->title, COL_TEXT_WHITE, bar_col);
    /* Titlebar buttons */
    draw_titlebar_buttons(w);
    /* Body */
    render_window_body(w, t, frame_cnt, blink);
}
