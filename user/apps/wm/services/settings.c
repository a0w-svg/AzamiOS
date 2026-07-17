/**
 * settings.c — AzamiOS Settings & Desktop Environment Switcher (v5.0)
 *
 * This service provides the Settings panel window, which allows the user to:
 *   1. Switch between Desktop Environments (Classic ↔ Modern) in real-time.
 *   2. Choose a color theme (Ocean Dark, Amber Warm, Cyber Purple).
 *   3. View build and version information.
 *
 * DESIGN PRINCIPLES:
 *   - DE switching is done via wm_switch_de(), which atomically swaps the g_de
 *     pointer. No restart is required — the very next rendered frame uses the
 *     new DE's visual callbacks.
 *   - Theme switching similarly swaps g_theme via wm_switch_theme().
 *   - Both actions push a toast notification so the user gets visual feedback.
 *   - The panel is entirely self-contained in on_render; no external UI coupling.
 *
 * INTERACTION MODEL:
 *   The on_key handler processes:
 *     '1' / '2' — Switch to Classic / Modern DE
 *     'q' / 'w' / 'e' — Switch to Ocean / Amber / Cyber theme
 *     (Mouse click dispatch is done via coordinate math in on_render state)
 *
 * NOTE: Mouse clicks within the Settings window body are detected in main.c's
 * general window-body-click handler, which calls on_key with a sentinel code.
 * To keep things simple we use a dedicated coord-tracking approach below.
 */

#include "../wm.h"

/* ── Persistent click region state (updated each render) ────────────────── */
typedef struct {
    int x, y, w, h;
    int action;   /* 0=switch_de, 1=switch_theme */
    int param;    /* de_id or theme_id */
} settings_btn_t;

#define MAX_BTNS 8
static settings_btn_t s_btns[MAX_BTNS];
static int s_num_btns = 0;

/* Register a clickable button region for hit-testing */
static void reg_btn(int x, int y, int w, int h, int action, int param) {
    if (s_num_btns >= MAX_BTNS) return;
    s_btns[s_num_btns++] = (settings_btn_t){x, y, w, h, action, param};
}

/* ── Color helpers (self-contained, no desktop.c dependency) ─────────────── */
static uint32_t blend_col(uint32_t a, uint32_t b, int t) {
    uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint8_t rr = (uint8_t)(ar + (int)(br - ar) * t / 256);
    uint8_t gg = (uint8_t)(ag + (int)(bg - ag) * t / 256);
    uint8_t bb2= (uint8_t)(ab + (int)(bb - ab) * t / 256);
    return ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | bb2;
}

static uint32_t darken(uint32_t c, int f) {
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >>  8) & 0xFF;
    uint8_t b =  c        & 0xFF;
    r = (uint8_t)(r * (16 - f) / 16);
    g = (uint8_t)(g * (16 - f) / 16);
    b = (uint8_t)(b * (16 - f) / 16);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ── Section rendering helpers ───────────────────────────────────────────── */
static void draw_section_header(int x, int y, int w, const char *title, uint32_t accent, uint32_t bg) {
    draw_rect(x, y, w, 18, darken(bg, 3));
    draw_rect(x, y, 3, 18, accent);
    draw_text(x + 8, y + 5, title, accent, darken(bg, 3));
}

/* Draw a toggle button */
static void draw_toggle_btn(int x, int y, int bw, int bh,
                            const char *label, bool selected,
                            uint32_t accent, uint32_t bg,
                            int action, int param) {
    uint32_t btn_bg = selected ? accent : darken(bg, 2);
    uint32_t btn_fg = selected ? COL_TEXT_WHITE : COL_TEXT_GRAY;
    draw_rect(x, y, bw, bh, btn_bg);
    draw_rect(x, y, bw, 1, selected ? blend_col(accent, 0x00FFFFFF, 50) : darken(bg, (int)4));
    if (selected) {
        draw_rect(x + 1, y + 1, bw - 2, bh / 2, blend_col(accent, 0x00FFFFFF, 25));
    }
    int tx = x + (bw - (int)strlen(label) * 8) / 2;
    if (tx < x + 2) tx = x + 2;
    draw_text(tx, y + (bh - 8) / 2, label, btn_fg, btn_bg);
    reg_btn(x, y, bw, bh, action, param);
}

/* ── Render callback ─────────────────────────────────────────────────────── */
static void settings_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;

    s_num_btns = 0;  /* Reset click regions each frame */

    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    uint32_t accent  = g_theme ? g_theme->accent        : COL_TITLE_ACT;
    uint32_t accent2 = g_theme ? g_theme->accent2       : COL_TEXT_CYAN;
    uint32_t bg      = g_theme ? darken(g_theme->win_body, 1) : COL_WIN_BODY;
    uint32_t txt     = g_theme ? g_theme->text_primary   : COL_TEXT_DARK;
    uint32_t txt2    = g_theme ? g_theme->text_secondary : COL_TEXT_GRAY;

    /* Background */
    draw_rect(bx, by, bw, bh, bg);

    int cy = by + 6;

    /* ── Section 1: Desktop Environment ─────────────────────────────── */
    draw_section_header(bx + 4, cy, bw - 8, "Desktop Environment", accent, bg);
    cy += 22;

    draw_text(bx + 8, cy, "Choose your DE — active instantly, no restart needed.", txt2, bg);
    cy += 14;

    int btn_w = (bw - 28) / 3;
    bool classic_active = (g_de && g_de->id == WM_DE_CLASSIC);
    bool modern_active  = (g_de && g_de->id == WM_DE_MODERN);
    bool win10_active   = (g_de && g_de->id == WM_DE_WIN10);

    draw_toggle_btn(bx + 8,                 cy, btn_w, 22, "Classic",  classic_active, accent, bg, 0, WM_DE_CLASSIC);
    draw_toggle_btn(bx + 12 + btn_w,        cy, btn_w, 22, "Modern",   modern_active,  accent, bg, 0, WM_DE_MODERN);
    draw_toggle_btn(bx + 16 + btn_w * 2,    cy, btn_w, 22, "Win10",    win10_active,   accent, bg, 0, WM_DE_WIN10);
    cy += 28;

    /* DE description */
    if (win10_active) {
        draw_text(bx + 8, cy, "Win10: Left taskbar, search box, live tiles", accent, bg);
        cy += 11;
        draw_text(bx + 8, cy, "in start menu, Aero Snap edge resizing.", txt2, bg);
    } else if (modern_active) {
        draw_text(bx + 8, cy, "Modern: gradient wallpaper, glassmorphism", accent2, bg);
        cy += 11;
        draw_text(bx + 8, cy, "panels, top status bar, pill buttons.", txt2, bg);
    } else {
        draw_text(bx + 8, cy, "Classic: clean flat chrome, solid gradient", txt2, bg);
        cy += 11;
        draw_text(bx + 8, cy, "wallpaper, navy taskbar, square buttons.", txt2, bg);
    }
    cy += 16;

    /* Divider */
    draw_rect(bx + 8, cy, bw - 16, 1, blend_col(accent, 0x00000000, 180));
    cy += 8;

    /* ── Section 2: Color Theme (enumerate from g_themes[] — no hardcoded list) */
    draw_section_header(bx + 4, cy, bw - 8, "Color Theme", accent2, bg);
    cy += 22;

    draw_text(bx + 8, cy, "Themes are loaded from *.theme files at boot.", txt2, bg);
    cy += 14;

    /* Distribute theme buttons evenly across the window width */
    int nt = g_num_themes > 0 ? g_num_themes : 1;
    int tbw = (bw - 16 - (nt - 1) * 4) / nt;  /* per-button width */
    if (tbw < 20) tbw = 20;
    if (tbw > 90) tbw = 90;
    int tx = bx + 8;

    for (int ti = 0; ti < nt && ti < MAX_THEMES; ti++) {
        wm_theme_t *th = &g_themes[ti];
        bool sel = (g_theme == th);
        uint32_t swatch    = th->accent;
        uint32_t swatch_hi = sel ? blend_col(swatch, 0x00FFFFFF, 40) : swatch;
        /* Button background */
        draw_rect(tx, cy, tbw, 24, swatch_hi);
        /* Top highlight line */
        draw_rect(tx, cy, tbw, 1, blend_col(swatch, 0x00FFFFFF, 60));
        /* Active underline */
        if (sel) draw_rect(tx, cy + 22, tbw, 2, 0x00FFFFFF);
        /* Label — up to 10 chars from theme name */
        char label[11]; int nl = 0;
        for (const char *p = th->name; *p && nl < 10; p++) label[nl++] = *p;
        label[nl] = '\0';
        int lx = tx + (tbw - nl * 8) / 2;
        if (lx < tx + 1) lx = tx + 1;
        draw_text(lx, cy + 8, label, COL_TEXT_WHITE, swatch_hi);
        reg_btn(tx, cy, tbw, 24, 1, ti);
        tx += tbw + 4;
    }
    cy += 30;

    /* Active theme: filename on right */
    draw_text(bx + 8, cy, "Active: ", txt2, bg);
    const char *tname = g_theme ? g_theme->name : "(none)";
    draw_text(bx + 8 + 8 * 8, cy, tname, accent, bg);
    /* Font setting */
    draw_text(bx + 8 + 8 * 8 + 8 * 16, cy, "|", txt2, bg);
    const char *tfont = (g_theme && g_theme->font[0]) ? g_theme->font : "default";
    draw_text(bx + 8 + 8 * 8 + 8 * 16 + 8, cy, "font:", txt2, bg);
    draw_text(bx + 8 + 8 * 8 + 8 * 16 + 48, cy, tfont, accent2, bg);
    cy += 16;

    /* Divider */
    draw_rect(bx + 8, cy, bw - 16, 1, blend_col(accent, 0x00000000, 180));
    cy += 8;

    /* ── Section 3: About ───────────────────────────────────────────────── */
    draw_section_header(bx + 4, cy, bw - 8, "About AzamiOS", txt, bg);
    cy += 22;

    draw_text(bx + 8, cy, "AzamiOS v5.0  —  Modular DE Architecture", txt, bg); cy += 13;
    draw_text(bx + 8, cy, "Themes: loaded from *.theme files at boot",  txt2, bg); cy += 13;
    draw_text(bx + 8, cy, "Config: wm.conf (de, theme) — auto-saved",  txt2, bg); cy += 13;
    draw_text(bx + 8, cy, "State:  wm_state saved before exec()",       txt2, bg); cy += 13;

    /* Show how many themes are loaded */
    char tbuf[32]; int tp = 0;
    const char *ts_pfx = "Loaded themes: ";
    while (*ts_pfx) tbuf[tp++] = *ts_pfx++;
    int tcount = g_num_themes;
    if (tcount >= 10) { tbuf[tp++] = (char)('0' + tcount / 10); }
    tbuf[tp++] = (char)('0' + tcount % 10);
    tbuf[tp] = '\0';
    draw_text(bx + 8, cy, tbuf, accent2, bg);

    (void)cy;
}

/* ── Key / mouse dispatch ───────────────────────────────────────────────────── */
static void settings_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)t; (void)frame_cnt;
    if (!w || !w->open || w->minimized) return;

    /* DE shortcuts: '1'=Classic '2'=Modern '3'=Win10 */
    if (c == '1') {
        wm_switch_de(WM_DE_CLASSIC);
        wm_push_notification("Desktop: Classic", COL_TEXT_BLUE);
    } else if (c == '2') {
        wm_switch_de(WM_DE_MODERN);
        wm_push_notification("Desktop: Modern", COL_TEXT_CYAN);
    } else if (c == '3') {
        wm_switch_de(WM_DE_WIN10);
        wm_push_notification("Desktop: Windows 10", COL_TEXT_WHITE);
    }
    /* Theme shortcuts: 'q'=theme[0]  'w'=theme[1]  'e'=theme[2]  ... */
    else if (c >= 'q' && c <= 'z') {
        int ti = c - 'q';
        if (ti < g_num_themes) {
            wm_switch_theme(ti);
            char msg[40]; int mp = 0;
            const char *pfx = "Theme: ";
            while (*pfx) msg[mp++] = *pfx++;
            const char *tn = g_themes[ti].name;
            while (*tn && mp < 38) msg[mp++] = *tn++;
            msg[mp] = '\0';
            wm_push_notification(msg, g_themes[ti].accent);
        }
    } else if (c >= 'Q' && c <= 'Z') {
        int ti = c - 'Q';
        if (ti < g_num_themes) {
            wm_switch_theme(ti);
            wm_push_notification(g_themes[ti].name, g_themes[ti].accent);
        }
    }
}

/* ── Mouse click handler (called from wm_core via a special sentinel path) ─ */
/**
 * settings_handle_mouse_click: Called when a left-click lands inside the
 * Settings window body. We check each registered button rect to find a hit.
 */
void settings_handle_mouse_click(int mx, int my) {
    for (int i = 0; i < s_num_btns; i++) {
        settings_btn_t *b = &s_btns[i];
        if (mx >= b->x && mx <= b->x + b->w &&
            my >= b->y && my <= b->y + b->h) {
            if (b->action == 0) {
                /* DE switch */
                wm_switch_de(b->param);
                const char *name = (b->param == WM_DE_WIN10) ? "Windows 10" : ((b->param == WM_DE_MODERN) ? "Modern" : "Classic");
                char msg[40];
                int p = 0;
                const char *pfx = "Desktop: ";
                while (*pfx) msg[p++] = *pfx++;
                while (*name) msg[p++] = *name++;
                msg[p] = '\0';
                uint32_t col = (b->param == WM_DE_WIN10) ? COL_TEXT_WHITE : ((b->param == WM_DE_MODERN) ? COL_TEXT_CYAN : COL_TEXT_BLUE);
                wm_push_notification(msg, col);
            } else {
                /* Theme switch */
                wm_switch_theme(b->param);
                wm_theme_t *th = wm_get_theme(b->param);
                char msg[40];
                int p = 0;
                const char *pfx = "Theme: ";
                while (*pfx) msg[p++] = *pfx++;
                const char *tname = th ? th->name : "Unknown";
                while (*tname) msg[p++] = *tname++;
                msg[p] = '\0';
                uint32_t col = th ? th->accent : COL_TEXT_WHITE;
                wm_push_notification(msg, col);
            }
            break;
        }
    }
}

/* ── Mouse state tracking for body-level hit testing ────────────────────── */
static bool s_prev_left = false;

static void settings_on_open(window_t *w) {
    (void)w;
    s_prev_left = false;
    wm_push_notification("Settings opened", COL_TEXT_GRAY);
}

void settings_service_init(void) {
    static const wm_service_t settings_srv = {
        WIN_SETTINGS,
        "Settings",
        WM_SRV_FLAG_NONE,
        NULL,
        settings_on_open,
        NULL,
        settings_render,
        settings_on_key,
    };
    wm_register_service(&settings_srv);
}
