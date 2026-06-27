#include "wm.h"

/* ── Global state ────────────────────────────────────────────────────────── */
window_t g_wins[MAX_WINS];
int g_num_wins = 6;
int g_focus = 0;

/* ── Service registry ────────────────────────────────────────────────────── */
#define MAX_SERVICES 16
static const wm_service_t *g_services[MAX_SERVICES];
static int g_num_services = 0;

void wm_register_service(const wm_service_t *service) {
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

/* ── Window management ───────────────────────────────────────────────────── */
void init_wins(void) {
    g_wins[0] = (window_t){0, "Welcome", 140, 60, 360, 260, 140, 60, 360, 260, true, false, false, WIN_WELCOME};
    g_wins[1] = (window_t){1, "Terminal", 180, 40, 410, 254, 180, 40, 410, 254, false, false, false, WIN_TERMINAL};
    g_wins[2] = (window_t){2, "System Monitor", 130, 90, 380, 240, 130, 90, 380, 240, false, false, false, WIN_SYSMON};
    g_wins[3] = (window_t){3, "About AzamiOS", 160, 70, 320, 280, 160, 70, 320, 280, false, false, false, WIN_ABOUT};
    g_wins[4] = (window_t){4, "Notepad", 200, 50, 380, 240, 200, 50, 380, 240, false, false, false, WIN_NOTEPAD};
    g_wins[5] = (window_t){5, "File Manager", 120, 80, 400, 260, 120, 80, 400, 260, false, false, false, WIN_FILES};
    g_num_wins = 6;

    for (int i = 0; i < g_num_services; i++) {
        if (g_services[i]->on_init) {
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
    const wm_service_t *srv = wm_get_service(w->type);
    if (srv && srv->on_render) {
        srv->on_render(w, t, frame_cnt, blink);
    } else {
        draw_rect(w->x + 1, w->y + TITLEBAR_H, w->w - 2, w->h - TITLEBAR_H - 1, COL_WIN_BODY);
    }
}

/* ── Render a single window ──────────────────────────────────────────────── */
void render_window(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    if (!w->open || w->minimized) return;

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
