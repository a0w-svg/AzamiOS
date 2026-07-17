/**
 * desktop.c — AzamiOS Modular Desktop Environment v5.0
 *
 * NEW IN v5.0:
 *   - O(1) DE Lookup: wm_get_de() uses direct table indexing (g_de_map).
 *
 * NEW IN v4.0:
 *   - Two full Desktop Environment implementations:
 *       de_classic: The original AzamiOS look — dark navy taskbar, flat window chrome.
 *       de_modern:  Glassmorphism-inspired — layered gradients, pill-shaped taskbar
 *                   buttons, frosted start panel, top status bar, bold accent lines.
 *   - wm_theme_t definitions: Ocean Dark, Amber Warm, Cyber Purple palettes.
 *   - render_toast_notifications(): Draws up to WM_NOTIF_MAX toast cards bottom-right.
 *   - Systray area: CPU + network indicator glyphs rendered in taskbar.
 *   - All public entry points (draw_wallpaper, render_taskbar, etc.) are thin
 *     dispatchers that call the active g_de->* function pointer. Switching DE
 *     requires only swapping g_de; none of the call sites change.
 *
 * ARCHITECTURE NOTE:
 *   wm_get_de() and wm_get_theme() are defined here because the DE/theme
 *   descriptors are static locals in this translation unit. The core engine
 *   (wm_core.c) and Settings service call these to retrieve the descriptors
 *   by ID without needing to know their internals.
 */

#include "wm.h"

/* ── Global UI state ─────────────────────────────────────────────────────── */
bool ctx_menu_open = false;
int  ctx_menu_x = 0, ctx_menu_y = 0;
bool start_menu  = false;

/* ── Desktop icons array ─────────────────────────────────────────────────── */
const desktop_icon_t icons[NUM_ICONS] = {
    {14,  18, "System",    WIN_SYSMON},
    {14,  73, "Terminal",  WIN_TERMINAL},
    {14, 128, "Notepad",   WIN_NOTEPAD},
    {14, 183, "Files",     WIN_FILES},
    {14, 238, "3D Cube",   WIN_GLCUBE},
    {14, 293, "C IDE",     WIN_IDE},
    {14, 348, "Settings",  WIN_SETTINGS},
    {14, 403, "Snake",     WIN_SNAKE},
    {14, 458, "Mines",     WIN_MINESWEEPER},
    {14, 513, "Tetris",    WIN_TETRIS},
    {14, 568, "Python",    WIN_PYTHON},
    {14, 623, "Debugger",  WIN_DEBUGGER},
    {14, 678, "Browser",   WIN_BROWSER},
    {14, 733, "Pong",      WIN_PONG},
};


/* ────────────────────────────────────────────────────────────────────────── */
/*  THEMES — loaded from *.theme files at startup by wm_config.c.             */
/*  g_themes[], g_theme_filenames[], g_num_themes are defined in wm_config.c.  */
/*  wm_get_theme(idx) is implemented in wm_config.c.                           */
/* ────────────────────────────────────────────────────────────────────────── */


/* ── Color helpers ───────────────────────────────────────────────────────── */
/* Blend two 0x00RRGGBB colors by t/256 (0=a, 256=b). */
static uint32_t blend_col(uint32_t a, uint32_t b, int t) {
    uint8_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint8_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint8_t rr = (uint8_t)(ar + (int)(br - ar) * t / 256);
    uint8_t gg = (uint8_t)(ag + (int)(bg - ag) * t / 256);
    uint8_t bb2= (uint8_t)(ab + (int)(bb - ab) * t / 256);
    return ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | bb2;
}

/* Darken a color by factor/16 */
static uint32_t darken(uint32_t c, int factor) {
    uint8_t r = (c >> 16) & 0xFF;
    uint8_t g = (c >>  8) & 0xFF;
    uint8_t b =  c        & 0xFF;
    r = (uint8_t)(r * (16 - factor) / 16);
    g = (uint8_t)(g * (16 - factor) / 16);
    b = (uint8_t)(b * (16 - factor) / 16);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  TIME / DATE FORMATTING                                                    */
/* ────────────────────────────────────────────────────────────────────────── */
void format_time_str(char *buf, rtc_time_t *t) {
    if (!buf || !t) return;
    char tmp[16]; int p = 0;
    itoa(t->hour, tmp, 10);
    if (t->hour   < 10) buf[p++] = '0';
    for (int i = 0; tmp[i] && p < 8; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->minute, tmp, 10);
    if (t->minute < 10) buf[p++] = '0';
    for (int i = 0; tmp[i] && p < 8; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->second, tmp, 10);
    if (t->second < 10) buf[p++] = '0';
    for (int i = 0; tmp[i] && p < 8; i++) buf[p++] = tmp[i];
    buf[p] = 0;
}

void format_date_str(char *buf, rtc_time_t *t) {
    if (!buf || !t) return;
    char tmp[16]; int p = 0;
    itoa(t->day, tmp, 10);
    if (t->day   < 10) buf[p++] = '0';
    for (int i = 0; tmp[i] && p < 8; i++) buf[p++] = tmp[i];
    buf[p++] = '/';
    itoa(t->month, tmp, 10);
    if (t->month < 10) buf[p++] = '0';
    for (int i = 0; tmp[i] && p < 8; i++) buf[p++] = tmp[i];
    buf[p] = 0;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  PIXEL-ART ICON DRAWING                                                    */
/* ────────────────────────────────────────────────────────────────────────── */
void draw_icon_terminal(int x, int y) {
    draw_rect(x, y, 24, 24, COL_TERM_BG);
    draw_rect(x, y, 24, 5, COL_ICON_GRAY);
    draw_rect(x + 1, y + 1, 3, 3, COL_BTN_CLOSE);
    draw_rect(x + 5, y + 1, 3, 3, COL_BTN_MIN);
    draw_rect(x + 9, y + 1, 3, 3, COL_BTN_MAX);
    draw_text(x + 2, y + 9, ">_", COL_TEXT_GREEN, COL_TERM_BG);
    draw_rect(x + 3, y + 19, 10, 2, COL_TEXT_GREEN);
}

void draw_icon_system(int x, int y) {
    draw_rect(x, y, 24, 24, COL_ICON_BLUE);
    draw_rect(x + 2, y + 2, 20, 20, COL_WIN_FRAME);
    draw_rect(x + 10, y, 4, 4, COL_ICON_BLUE);
    draw_rect(x + 10, y + 20, 4, 4, COL_ICON_BLUE);
    draw_rect(x, y + 10, 4, 4, COL_ICON_BLUE);
    draw_rect(x + 20, y + 10, 4, 4, COL_ICON_BLUE);
    draw_rect(x + 8, y + 8, 8, 8, COL_ICON_BLUE);
    draw_rect(x + 10, y + 10, 4, 4, COL_WIN_FRAME);
}

void draw_icon_folder(int x, int y) {
    draw_rect(x, y + 4, 24, 20, COL_ICON_AMBER);
    draw_rect(x, y + 2, 10, 4, COL_ICON_AMBER);
    draw_rect(x + 1, y + 7, 22, 15, 0x00FCD34D);
    draw_rect(x + 2, y + 9, 20, 2, COL_ICON_AMBER);
}

void draw_icon_notepad(int x, int y) {
    draw_rect(x + 2, y, 20, 24, COL_WIN_BODY);
    draw_rect(x + 2, y,  20, 1, COL_ICON_GRAY);
    draw_rect(x + 2, y + 23, 20, 1, COL_ICON_GRAY);
    draw_rect(x + 2, y, 1, 24, COL_ICON_GRAY);
    draw_rect(x + 21, y, 1, 24, COL_ICON_GRAY);
    draw_rect(x + 5, y + 5,  14, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 9,  12, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 13, 14, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 17,  8, 1, COL_TEXT_BLUE);
    draw_rect(x + 16, y + 14, 2, 8, COL_ICON_AMBER);
    draw_rect(x + 16, y + 22, 2, 2, COL_TEXT_DARK);
}

void draw_icon_cube(int x, int y) {
    draw_rect(x + 4, y,  16, 24, COL_TEXT_CYAN);
    draw_rect(x + 6, y + 4, 12, 16, COL_TITLE_ACT);
    draw_rect(x + 10, y + 8, 4, 8, COL_TEXT_GOLD);
}

void draw_icon_pong(int x, int y) {
    draw_rect(x + 2, y + 4, 4, 16, COL_TEXT_CYAN);
    draw_rect(x + 18, y + 4, 4, 16, COL_BTN_CLOSE);
    draw_rect(x + 10, y + 10, 4, 4, COL_TEXT_GOLD);
}

void draw_icon_ide(int x, int y) {
    draw_rect(x + 2, y, 20, 24, COL_IDE_BG);
    draw_rect(x + 2, y,  20, 1, COL_ICON_AMBER);
    draw_rect(x + 2, y + 23, 20, 1, COL_ICON_AMBER);
    draw_rect(x + 2, y, 1, 24, COL_ICON_AMBER);
    draw_rect(x + 21, y, 1, 24, COL_ICON_AMBER);
    draw_rect(x + 5, y + 6,  8, 2, COL_TEXT_GREEN);
    draw_rect(x + 5, y + 12, 12, 2, COL_TEXT_CYAN);
    draw_rect(x + 5, y + 18,  6, 2, COL_TEXT_GOLD);
}

/* Gear icon for Settings */
void draw_icon_settings(int x, int y) {
    uint32_t accent = g_theme ? g_theme->accent : COL_ICON_BLUE;
    /* outer ring */
    draw_rect(x + 8, y, 8, 4, accent);
    draw_rect(x + 8, y + 20, 8, 4, accent);
    draw_rect(x, y + 8, 4, 8, accent);
    draw_rect(x + 20, y + 8, 4, 8, accent);
    /* diagonal teeth */
    draw_rect(x + 3, y + 3, 4, 4, accent);
    draw_rect(x + 17, y + 3, 4, 4, accent);
    draw_rect(x + 3, y + 17, 4, 4, accent);
    draw_rect(x + 17, y + 17, 4, 4, accent);
    /* inner circle */
    draw_rect(x + 6, y + 6, 12, 12, accent);
    draw_rect(x + 8, y + 8, 8, 8, 0x00000000);
    draw_rect(x + 9, y + 9, 6, 6, darken(accent, 6));
    draw_rect(x + 10, y + 10, 4, 4, 0x00000000);
}

void draw_icon_game(int x, int y) {
    draw_rect(x + 2, y + 6, 20, 12, COL_ICON_GREEN);
    draw_rect(x + 5, y + 10, 4, 4, COL_TEXT_WHITE);
    draw_rect(x + 15, y + 10, 3, 3, COL_TEXT_RED);
}

void draw_icon_python(int x, int y) {
    draw_rect(x + 2, y + 2, 20, 20, 0x003776AB);
    draw_rect(x + 6, y + 6, 12, 12, 0x00FFD43B);
    draw_rect(x + 8, y + 8, 2, 2, COL_TEXT_DARK);
}

void draw_icon_debug(int x, int y) {
    draw_rect(x + 4, y + 4, 16, 16, COL_TEXT_RED);
    draw_rect(x + 8, y + 8, 8, 8, COL_WIN_BODY);
}

void draw_icon_browser(int x, int y) {
    draw_circle(x + 12, y + 12, 10, COL_TEXT_CYAN);
    draw_line(x + 2, y + 12, x + 22, y + 12, COL_TEXT_WHITE);
    draw_line(x + 12, y + 2, x + 12, y + 22, COL_TEXT_WHITE);
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  DESKTOP ICON RENDERING                                                    */
/* ────────────────────────────────────────────────────────────────────────── */
void render_desktop_icons(void) {
    for (int i = 0; i < NUM_ICONS; i++) {
        int ix = icons[i].x;
        int iy = icons[i].y;
        if (iy + 40 > DESKTOP_H) {
            ix = 74;
            iy = 18 + (i - 13) * 55;
        }
        switch (icons[i].win_type) {
            case WIN_SYSMON:      draw_icon_system(ix, iy);   break;
            case WIN_TERMINAL:    draw_icon_terminal(ix, iy); break;
            case WIN_NOTEPAD:     draw_icon_notepad(ix, iy);  break;
            case WIN_FILES:       draw_icon_folder(ix, iy);   break;
            case WIN_GLCUBE:      draw_icon_cube(ix, iy);     break;
            case WIN_PONG:        draw_icon_pong(ix, iy);     break;
            case WIN_IDE:         draw_icon_ide(ix, iy);      break;
            case WIN_SETTINGS:    draw_icon_settings(ix, iy); break;
            case WIN_SNAKE:
            case WIN_MINESWEEPER:
            case WIN_TETRIS:      draw_icon_game(ix, iy);     break;
            case WIN_PYTHON:      draw_icon_python(ix, iy);   break;
            case WIN_DEBUGGER:    draw_icon_debug(ix, iy);    break;
            case WIN_BROWSER:     draw_icon_browser(ix, iy);  break;
        }
        int lx = ix + 12 - ((int)strlen(icons[i].label) * 4);
        draw_text(lx, iy + 28, icons[i].label, COL_TEXT_WHITE, 0xFFFFFFFF);
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  CLASSIC DE IMPLEMENTATION                                                 */
/* ────────────────────────────────────────────────────────────────────────── */

static void classic_draw_wallpaper(void) {
    uint32_t top = g_theme ? g_theme->wall_top : COL_WALL_TOP;
    uint32_t bot = g_theme ? g_theme->wall_bot : COL_WALL_BOT;
    uint8_t r1 = (top >> 16) & 0xFF, g1 = (top >> 8) & 0xFF, b1 = top & 0xFF;
    uint8_t r2 = (bot >> 16) & 0xFF, g2 = (bot >> 8) & 0xFF, b2 = bot & 0xFF;
    for (int y = 0; y < DESKTOP_H; y += 16) {
        int rr = r1 + (r2 - r1) * y / DESKTOP_H;
        int gg = g1 + (g2 - g1) * y / DESKTOP_H;
        int bb = b1 + (b2 - b1) * y / DESKTOP_H;
        uint32_t col = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
        draw_rect(0, y, SCREEN_W, 16, col);
    }
}

static void classic_render_window_chrome(window_t *w, bool is_active) {
    if (!w) return;
    uint32_t bar_col  = is_active
        ? (g_theme ? g_theme->title_active   : COL_TITLE_ACT)
        : (g_theme ? g_theme->title_inactive  : COL_TITLE_INA);
    uint32_t frame_col = g_theme ? g_theme->win_frame : COL_WIN_FRAME;

    /* Drop shadow */
    draw_rect(w->x + 4, w->y + 4, w->w, w->h, COL_WIN_SHAD);
    /* Window frame */
    draw_rect(w->x, w->y, w->w, w->h, frame_col);
    /* Titlebar */
    draw_rect(w->x, w->y, w->w, TITLEBAR_H, bar_col);
    /* Accent stripe at top of titlebar */
    uint32_t accent = g_theme ? g_theme->accent : COL_TASKBAR_LN;
    draw_rect(w->x, w->y, w->w, 2, accent);
    /* Title text */
    draw_text(w->x + 8, w->y + 7, w->title, COL_TEXT_WHITE, bar_col);
    /* Buttons */
    int bx = w->x + w->w - 20;
    int by = w->y + 4;
    draw_rect(bx, by, 16, 14, COL_BTN_CLOSE);
    draw_text(bx + 4, by + 3, "X", COL_TEXT_WHITE, COL_BTN_CLOSE);
    bx -= 20;
    draw_rect(bx, by, 16, 14, COL_BTN_MAX);
    draw_rect(bx + 4, by + 4, 8, 6, COL_TEXT_WHITE);
    draw_rect(bx + 4, by + 4, 8, 2, COL_BTN_MAX);
    bx -= 20;
    draw_rect(bx, by, 16, 14, COL_BTN_MIN);
    draw_rect(bx + 4, by + 9, 8, 2, COL_TEXT_WHITE);
}

static void classic_render_taskbar(rtc_time_t *t, bool start_open, int notif_count) {
    (void)notif_count;
    uint32_t tb_col  = g_theme ? g_theme->taskbar       : COL_TASKBAR;
    uint32_t ln_col  = g_theme ? g_theme->taskbar_line   : COL_TASKBAR_LN;
    uint32_t sb_col  = g_theme ? (start_open ? COL_START_ACT : g_theme->start_btn)
                               : (start_open ? COL_START_ACT : COL_START_BTN);

    int ty = SCREEN_H - TASKBAR_H;
    draw_rect(0, ty, SCREEN_W, TASKBAR_H, tb_col);
    draw_rect(0, ty, SCREEN_W, 2, ln_col);

    /* START button */
    draw_rect(4, ty + 4, 62, 18, sb_col);
    draw_text(10, ty + 8, "START", COL_TEXT_WHITE, sb_col);

    /* Window buttons */
    int btn_x = 74;
    for (int i = 0; i < g_num_wins; i++) {
        if (!g_wins[i].open) continue;
        if (btn_x + 70 > 490) break;
        uint32_t bcol = (g_focus == i) ? COL_TB_BTN_ACT : COL_TB_BTN;
        draw_rect(btn_x, ty + 4, 68, 18, bcol);
        /* Active window gets accent underline */
        if (g_focus == i) draw_rect(btn_x, ty + 20, 68, 2, ln_col);
        char lbl[10];
        wm_strlcpy(lbl, g_wins[i].title, 9);
        draw_text(btn_x + 4, ty + 8, lbl, COL_TEXT_WHITE, bcol);
        btn_x += 72;
    }

    /* Systray: network + CPU glyphs */
    int tray_x = 496;
    /* Network glyph */
    draw_rect(tray_x, ty + 8, 6, 10, COL_TEXT_CYAN);
    draw_rect(tray_x + 2, ty + 6, 2, 12, COL_TEXT_CYAN);
    draw_rect(tray_x + 8, ty + 10, 6, 8, COL_TEXT_CYAN);
    /* CPU glyph */
    draw_rect(tray_x + 18, ty + 7, 10, 10, ln_col);
    draw_rect(tray_x + 20, ty + 9, 6, 6, tb_col);
    draw_rect(tray_x + 22, ty + 11, 2, 2, ln_col);

    /* Date / time */
    char date_buf[12], time_buf[12];
    format_date_str(date_buf, t);
    format_time_str(time_buf, t);
    draw_text(536, ty + 8, date_buf, COL_TEXT_GRAY, tb_col);
    draw_text(580, ty + 8, time_buf, COL_TEXT_WHITE, tb_col);
}

static void classic_render_start_menu(void) {
    int srv_count     = wm_get_service_count();
    int total_entries = srv_count + 1;
    int menu_h        = START_HEADER_H + total_entries * START_ENTRY_H + 8;
    int mx = 4;
    int my = SCREEN_H - TASKBAR_H - menu_h;

    uint32_t ctx_bg = COL_CTX_BG;
    uint32_t ln_col = g_theme ? g_theme->taskbar_line : COL_TASKBAR_LN;
    uint32_t sb_col = g_theme ? g_theme->start_btn : COL_START_BTN;

    draw_rect(mx, my, START_MENU_W, menu_h, ctx_bg);
    draw_rect(mx, my, START_MENU_W, 2, ln_col);
    draw_rect(mx + 4, my + 4, START_MENU_W - 8, START_HEADER_H - 6, sb_col);
    draw_text(mx + 10, my + 8, "AzamiOS", COL_TEXT_WHITE, sb_col);
    draw_text(mx + 10, my + 20, "v5.0", COL_TEXT_GRAY, sb_col);


    static const uint32_t palette[] = {
        COL_TEXT_WHITE, COL_TEXT_CYAN, COL_TEXT_GOLD, COL_TEXT_BLUE, COL_TEXT_GREEN
    };
    int ey = my + START_HEADER_H + 2;
    for (int i = 0; i < srv_count; i++) {
        const wm_service_t *srv = wm_get_service_by_index(i);
        char lbl[32];
        lbl[0] = '>'; lbl[1] = ' '; lbl[2] = '\0';
        if (srv && srv->name) wm_strlcpy(lbl + 2, srv->name, sizeof(lbl) - 2);
        draw_text(mx + 10, ey + 6, lbl, palette[i % 5], ctx_bg);
        ey += START_ENTRY_H;
    }
    draw_rect(mx + 8, ey, START_MENU_W - 16, 1, COL_ICON_GRAY);
    ey += 3;
    draw_text(mx + 10, ey + 6, "X Shutdown", COL_TEXT_RED, ctx_bg);
}

static void classic_render_context_menu(void) {
    int mx = ctx_menu_x, my = ctx_menu_y;
    if (mx + CTX_MENU_W > SCREEN_W) mx = SCREEN_W - CTX_MENU_W;
    if (my + CTX_MENU_H > DESKTOP_H) my = DESKTOP_H - CTX_MENU_H;

    uint32_t ctx_bg = COL_CTX_BG;
    uint32_t ln_col = g_theme ? g_theme->taskbar_line : COL_TASKBAR_LN;

    draw_rect(mx, my, CTX_MENU_W, CTX_MENU_H, ctx_bg);
    draw_rect(mx, my, CTX_MENU_W, 2, ln_col);

    int ey = my + 6;
    draw_text(mx + 10, ey + 4, "= New Notepad",   COL_TEXT_GREEN, ctx_bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "? About AzamiOS", COL_TEXT_CYAN,  ctx_bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "@ File Manager",  COL_TEXT_GOLD,  ctx_bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "~ Refresh",        COL_TEXT_GRAY,  ctx_bg);
}

static void classic_render_status_bar(rtc_time_t *t) {
    (void)t; /* Classic DE has no top status bar */
}

static wm_de_t de_classic = {
    WM_DE_CLASSIC, "Classic",
    classic_draw_wallpaper,
    classic_render_taskbar,
    classic_render_start_menu,
    classic_render_context_menu,
    classic_render_window_chrome,
    classic_render_status_bar,
};

/* ────────────────────────────────────────────────────────────────────────── */
/*  MODERN DE IMPLEMENTATION                                                  */
/* ────────────────────────────────────────────────────────────────────────── */

static void modern_draw_wallpaper(void) {
    uint32_t top = g_theme ? g_theme->wall_top : COL_WALL_TOP;
    uint32_t bot = g_theme ? g_theme->wall_bot : COL_WALL_BOT;
    /* Multi-stop gradient: top → mid (slightly lighter) → bottom */
    uint32_t mid = blend_col(top, bot, 90);

    for (int y = 0; y < DESKTOP_H; y += 8) {
        uint32_t col;
        if (y < DESKTOP_H / 2) {
            col = blend_col(top, mid, y * 256 / (DESKTOP_H / 2));
        } else {
            col = blend_col(mid, bot, (y - DESKTOP_H / 2) * 256 / (DESKTOP_H / 2));
        }
        draw_rect(0, y, SCREEN_W, 8, col);
    }

    /* Subtle grid overlay (every 32px, very dark lines) */
    uint32_t grid_col = blend_col(top, 0x00FFFFFF, 12);
    for (int y = 0; y < DESKTOP_H; y += 32) {
        draw_rect(0, y, SCREEN_W, 1, grid_col);
    }
    for (int x = 0; x < SCREEN_W; x += 32) {
        draw_rect(x, 0, 1, DESKTOP_H, grid_col);
    }

    /* Radial glow in center — simulate with a soft large rect */
    uint32_t glow = g_theme ? g_theme->accent : COL_TITLE_ACT;
    glow = blend_col(g_theme ? g_theme->wall_top : COL_WALL_TOP, glow, 20);
    draw_rect(200, 80, 240, 160, glow);
    glow = blend_col(g_theme ? g_theme->wall_top : COL_WALL_TOP, glow, 40);
    draw_rect(240, 110, 160, 100, glow);
}

static void modern_render_window_chrome(window_t *w, bool is_active) {
    if (!w) return;
    uint32_t accent  = g_theme ? g_theme->accent        : COL_TITLE_ACT;
    uint32_t bar_col = is_active
        ? (g_theme ? g_theme->title_active   : COL_TITLE_ACT)
        : darken(g_theme ? g_theme->title_inactive : COL_TITLE_INA, 2);
    uint32_t frame_col = g_theme ? g_theme->win_frame : COL_WIN_FRAME;

    /* Larger drop shadow (modern depth) */
    draw_rect(w->x + 6, w->y + 6, w->w, w->h, 0x00020408);
    draw_rect(w->x + 3, w->y + 3, w->w, w->h, blend_col(COL_WIN_SHAD, 0x00000000, 80));

    /* Window frame with slight inner border */
    draw_rect(w->x, w->y, w->w, w->h, frame_col);
    draw_rect(w->x + 1, w->y + 1, w->w - 2, w->h - 2, frame_col);

    /* Titlebar */
    draw_rect(w->x, w->y, w->w, TITLEBAR_H, bar_col);

    /* Gradient on titlebar (active windows get a subtle top-to-bottom fade) */
    if (is_active) {
        uint32_t bar_hi = blend_col(bar_col, 0x00FFFFFF, 18);
        draw_rect(w->x, w->y, w->w, TITLEBAR_H / 2, bar_hi);
        draw_rect(w->x, w->y + TITLEBAR_H / 2, w->w, TITLEBAR_H / 2, bar_col);
    }

    /* Bold accent bar at top */
    draw_rect(w->x, w->y, w->w, 3, accent);

    /* Title text */
    draw_text(w->x + 10, w->y + 7, w->title, COL_TEXT_WHITE, is_active ? bar_col : darken(bar_col, 2));

    /* Pill-style buttons */
    int bx = w->x + w->w - 22;
    int by = w->y + 5;
    /* Close */
    draw_rect(bx, by, 16, 12, COL_BTN_CLOSE);
    draw_rect(bx + 1, by + 1, 14, 10, blend_col(COL_BTN_CLOSE, 0x00FFFFFF, 40));
    draw_text(bx + 4, by + 2, "X", COL_TEXT_WHITE, COL_BTN_CLOSE);
    bx -= 20;
    /* Maximize */
    draw_rect(bx, by, 16, 12, COL_BTN_MAX);
    draw_rect(bx + 1, by + 1, 14, 10, blend_col(COL_BTN_MAX, 0x00FFFFFF, 40));
    draw_rect(bx + 4, by + 3, 8, 6, COL_TEXT_WHITE);
    draw_rect(bx + 4, by + 3, 8, 2, COL_BTN_MAX);
    bx -= 20;
    /* Minimize */
    draw_rect(bx, by, 16, 12, COL_BTN_MIN);
    draw_rect(bx + 1, by + 1, 14, 10, blend_col(COL_BTN_MIN, 0x00FFFFFF, 40));
    draw_rect(bx + 4, by + 8, 8, 2, COL_TEXT_WHITE);
}

static void modern_render_taskbar(rtc_time_t *t, bool start_open, int notif_count) {
    uint32_t tb_col  = g_theme ? darken(g_theme->taskbar, 2) : darken(COL_TASKBAR, 2);
    uint32_t ln_col  = g_theme ? g_theme->taskbar_line        : COL_TASKBAR_LN;
    uint32_t accent  = g_theme ? g_theme->accent               : COL_START_BTN;
    uint32_t sb_col  = start_open ? darken(accent, 3) : accent;

    int ty = SCREEN_H - TASKBAR_H;
    draw_rect(0, ty, SCREEN_W, TASKBAR_H, tb_col);

    /* Frosted glass top edge — two-tone gradient line */
    draw_rect(0, ty, SCREEN_W, 1, blend_col(ln_col, 0x00FFFFFF, 60));
    draw_rect(0, ty + 1, SCREEN_W, 1, ln_col);

    /* START button — pill shape simulation */
    draw_rect(4, ty + 4, 64, 18, sb_col);
    draw_rect(5, ty + 4, 62, 1, blend_col(sb_col, 0x00FFFFFF, 60));  /* top shine */
    draw_text(8, ty + 9, "AZAMI", COL_TEXT_WHITE, sb_col);

    /* Window buttons — pill shape with active indicator */
    int btn_x = 76;
    for (int i = 0; i < g_num_wins; i++) {
        if (!g_wins[i].open) continue;
        if (btn_x + 70 > 480) break;
        bool active = (g_focus == i);
        uint32_t bcol = active ? accent : blend_col(tb_col, 0x00FFFFFF, 25);

        draw_rect(btn_x, ty + 4, 68, 18, bcol);
        /* Shine line */
        draw_rect(btn_x + 1, ty + 4, 66, 1, blend_col(bcol, 0x00FFFFFF, active ? 50 : 25));
        /* Active underline dot */
        if (active) {
            draw_rect(btn_x + 30, ty + 22, 8, 2, ln_col);
        }
        char lbl[10];
        wm_strlcpy(lbl, g_wins[i].title, 9);
        draw_text(btn_x + 4, ty + 9, lbl, COL_TEXT_WHITE, bcol);
        btn_x += 72;
    }

    /* Systray glyphs */
    int tray_x = 486;
    /* Notification badge (if any active) */
    if (notif_count > 0) {
        draw_rect(tray_x, ty + 7, 10, 12, ln_col);
        draw_rect(tray_x + 2, ty + 9, 6, 8, tb_col);
        draw_rect(tray_x + 4, ty + 9, 2, 4, ln_col);
        tray_x += 14;
    }
    /* Network antenna */
    draw_rect(tray_x + 3, ty + 7, 4, 10, COL_TEXT_CYAN);
    draw_rect(tray_x, ty + 14, 10, 3, COL_TEXT_CYAN);
    /* CPU chip */
    draw_rect(tray_x + 14, ty + 8, 10, 10, accent);
    draw_rect(tray_x + 16, ty + 10, 6, 6, tb_col);
    draw_rect(tray_x + 18, ty + 12, 2, 2, accent);

    /* Date / time */
    char date_buf[12], time_buf[12];
    format_date_str(date_buf, t);
    format_time_str(time_buf, t);
    draw_text(530, ty + 4, date_buf, COL_TEXT_GRAY, tb_col);
    draw_text(574, ty + 12, time_buf, COL_TEXT_WHITE, tb_col);
}

static void modern_render_start_menu(void) {
    int srv_count     = wm_get_service_count();
    int total_entries = srv_count + 1;
    int menu_h        = START_HEADER_H + total_entries * START_ENTRY_H + 12;
    int mx = 4;
    int my = SCREEN_H - TASKBAR_H - menu_h;

    uint32_t accent = g_theme ? g_theme->accent  : COL_START_BTN;
    uint32_t tb_col = g_theme ? darken(g_theme->taskbar, 2) : darken(COL_TASKBAR, 2);
    uint32_t ln_col = g_theme ? g_theme->taskbar_line : COL_TASKBAR_LN;
    uint32_t bg     = blend_col(tb_col, 0x00000000, 40);  /* semi-transparent panel */

    /* Frosted panel */
    draw_rect(mx, my, START_MENU_W, menu_h, bg);
    draw_rect(mx, my, 2, menu_h, accent);              /* left accent bar */
    draw_rect(mx, my, START_MENU_W, 1, ln_col);        /* top border */
    draw_rect(mx + START_MENU_W - 1, my, 1, menu_h, ln_col); /* right border */

    /* Header */
    draw_rect(mx + 2, my, START_MENU_W - 2, START_HEADER_H, darken(bg, 2));
    draw_rect(mx + 2, my, START_MENU_W - 2, 1, blend_col(accent, 0x00FFFFFF, 60));
    draw_text(mx + 12, my + 6,  "AzamiOS", COL_TEXT_WHITE, darken(bg, 2));
    draw_text(mx + 12, my + 18, "v5.0 Modern", g_theme ? g_theme->accent2 : COL_TEXT_CYAN, darken(bg, 2));


    static const uint32_t palette[] = {
        COL_TEXT_WHITE, COL_TEXT_CYAN, COL_TEXT_GOLD, COL_TEXT_BLUE, COL_TEXT_GREEN
    };
    int ey = my + START_HEADER_H + 4;
    for (int i = 0; i < srv_count; i++) {
        const wm_service_t *srv = wm_get_service_by_index(i);
        char lbl[32];
        lbl[0] = 0xBB; lbl[1] = ' '; lbl[2] = '\0';  /* » style arrow */
        lbl[0] = '>'; lbl[1] = ' ';
        if (srv && srv->name) wm_strlcpy(lbl + 2, srv->name, sizeof(lbl) - 2);
        uint32_t col = palette[i % 5];
        draw_text(mx + 14, ey + 6, lbl, col, bg);
        ey += START_ENTRY_H;
    }

    draw_rect(mx + 8, ey, START_MENU_W - 16, 1, blend_col(ln_col, 0x00000000, 100));
    ey += 4;
    draw_text(mx + 14, ey + 6, "X Shutdown", COL_TEXT_RED, bg);
}

static void modern_render_context_menu(void) {
    int mx = ctx_menu_x, my = ctx_menu_y;
    if (mx + CTX_MENU_W > SCREEN_W) mx = SCREEN_W - CTX_MENU_W;
    if (my + CTX_MENU_H > DESKTOP_H) my = DESKTOP_H - CTX_MENU_H;

    uint32_t accent = g_theme ? g_theme->accent : COL_TASKBAR_LN;
    uint32_t bg     = blend_col(COL_CTX_BG, 0x00000000, 30);
    uint32_t ln_col = g_theme ? g_theme->taskbar_line : COL_TASKBAR_LN;

    draw_rect(mx, my, CTX_MENU_W, CTX_MENU_H, bg);
    draw_rect(mx, my, 2, CTX_MENU_H, accent);
    draw_rect(mx, my, CTX_MENU_W, 1, ln_col);
    draw_rect(mx + CTX_MENU_W - 1, my, 1, CTX_MENU_H, ln_col);

    int ey = my + 6;
    draw_text(mx + 12, ey + 4, "= New Notepad",   COL_TEXT_GREEN, bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 12, ey + 4, "? About AzamiOS", COL_TEXT_CYAN,  bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 12, ey + 4, "@ File Manager",  COL_TEXT_GOLD,  bg); ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 12, ey + 4, "~ Refresh",        COL_TEXT_GRAY,  bg);
}

static void modern_render_status_bar(rtc_time_t *t) {
    /* Modern DE: thin status bar at TOP of screen showing DE name and active theme */
    uint32_t accent = g_theme ? g_theme->accent   : COL_TASKBAR_LN;
    uint32_t tb_col = g_theme ? g_theme->taskbar   : COL_TASKBAR;
    uint32_t bar_bg = darken(tb_col, 4);

    draw_rect(0, 0, SCREEN_W, STATUS_BAR_H, bar_bg);
    draw_rect(0, STATUS_BAR_H - 1, SCREEN_W, 1, accent);

    draw_text(8, 5, "AzamiOS 5.0", accent, bar_bg);

    const char *theme_name = g_theme ? g_theme->name : "Ocean Dark";
    draw_text(SCREEN_W - 8 - (int)strlen(theme_name) * 8, 5, theme_name, COL_TEXT_GRAY, bar_bg);

    /* Center: active DE label */
    draw_text(SCREEN_W / 2 - 28, 5, "[ Modern ]", COL_TEXT_WHITE, bar_bg);

    (void)t;
}

static wm_de_t de_modern = {
    WM_DE_MODERN, "Modern",
    modern_draw_wallpaper,
    modern_render_taskbar,
    modern_render_start_menu,
    modern_render_context_menu,
    modern_render_window_chrome,
    modern_render_status_bar,
};

/* ────────────────────────────────────────────────────────────────────────── */
/*  WINDOWS 10 DE IMPLEMENTATION                                              */
/* ────────────────────────────────────────────────────────────────────────── */
static void win10_draw_wallpaper(void) {
    uint32_t top = g_theme ? g_theme->wall_top : 0x000F172A;
    uint32_t bot = g_theme ? g_theme->wall_bot : 0x001E3A8A;
    for (int y = 0; y < DESKTOP_H; y += 10) {
        int t = y * 256 / DESKTOP_H;
        uint32_t col = blend_col(top, bot, t);
        draw_rect(0, y, SCREEN_W, 10, col);
    }
    /* Fluent hero polygons in background */
    uint32_t hero = blend_col(bot, 0x003B82F6, 40);
    draw_rect(SCREEN_W - 500, DESKTOP_H - 300, 450, 250, hero);
}

static void win10_render_window_chrome(window_t *w, bool is_active) {
    if (!w) return;
    uint32_t bar_col = is_active ? (g_theme ? g_theme->title_active : 0x001E293B) : 0x00334155;
    uint32_t frame_col = g_theme ? g_theme->win_frame : 0x00F8FAFC;

    /* Drop shadow */
    draw_rect(w->x + 4, w->y + 4, w->w, w->h, 0x00020408);
    /* Frame & Titlebar */
    draw_rect(w->x, w->y, w->w, w->h, frame_col);
    draw_rect(w->x, w->y, w->w, TITLEBAR_H, bar_col);
    if (is_active) {
        draw_rect(w->x, w->y, w->w, 2, g_theme ? g_theme->accent : 0x003B82F6);
    }
    draw_text(w->x + 10, w->y + 7, w->title, COL_TEXT_WHITE, bar_col);

    /* Flat Win10 buttons right-aligned */
    int bx = w->x + w->w - 28;
    int by = w->y + 4;
    draw_rect(bx, by, 24, 18, COL_BTN_CLOSE);
    draw_text(bx + 8, by + 4, "X", COL_TEXT_WHITE, COL_BTN_CLOSE);
    bx -= 26;
    draw_rect(bx, by, 24, 18, bar_col);
    draw_rect(bx + 7, by + 5, 10, 8, COL_TEXT_WHITE);
    draw_rect(bx + 8, by + 6, 8, 6, bar_col);
    bx -= 26;
    draw_rect(bx, by, 24, 18, bar_col);
    draw_rect(bx + 7, by + 11, 10, 2, COL_TEXT_WHITE);
}

static void win10_render_taskbar(rtc_time_t *t, bool start_open, int notif_count) {
    (void)notif_count;
    uint32_t tb_col = 0x000F172A;
    uint32_t accent = g_theme ? g_theme->accent : 0x003B82F6;
    int ty = SCREEN_H - TASKBAR_H;

    draw_rect(0, ty, SCREEN_W, TASKBAR_H, tb_col);
    draw_rect(0, ty, SCREEN_W, 1, 0x00334155);

    /* Start Button (flat colored Windows logo concept) */
    uint32_t sb_col = start_open ? accent : 0x001E293B;
    draw_rect(4, ty + 4, 44, TASKBAR_H - 8, sb_col);
    draw_rect(16, ty + 10, 6, 6, COL_TEXT_WHITE);
    draw_rect(24, ty + 10, 6, 6, COL_TEXT_WHITE);
    draw_rect(16, ty + 18, 6, 6, COL_TEXT_WHITE);
    draw_rect(24, ty + 18, 6, 6, COL_TEXT_WHITE);

    /* Search Box */
    draw_rect(54, ty + 6, 160, TASKBAR_H - 12, 0x001E293B);
    draw_text(62, ty + 11, "Type here to search...", 0x0064748B, 0x001E293B);

    /* Window buttons */
    int btn_x = 224;
    for (int i = 0; i < g_num_wins; i++) {
        if (!g_wins[i].open) continue;
        if (btn_x + 110 > SCREEN_W - 200) break;
        bool active = (g_focus == i);
        uint32_t bcol = active ? 0x001E293B : tb_col;
        draw_rect(btn_x, ty + 4, 106, TASKBAR_H - 8, bcol);
        if (active) draw_rect(btn_x, ty + TASKBAR_H - 3, 106, 3, accent);
        char lbl[14];
        wm_strlcpy(lbl, g_wins[i].title, 13);
        draw_text(btn_x + 8, ty + 11, lbl, COL_TEXT_WHITE, bcol);
        btn_x += 110;
    }

    /* System Tray */
    char date_buf[12], time_buf[12];
    format_date_str(date_buf, t);
    format_time_str(time_buf, t);
    draw_text(SCREEN_W - 130, ty + 6, time_buf, COL_TEXT_WHITE, tb_col);
    draw_text(SCREEN_W - 130, ty + 20, date_buf, COL_TEXT_GRAY, tb_col);
}

static void win10_render_start_menu(void) {
    int menu_w = 360;
    int menu_h = 420;
    int mx = 4;
    int my = SCREEN_H - TASKBAR_H - menu_h;
    uint32_t bg = 0x000F172A;
    uint32_t accent = g_theme ? g_theme->accent : 0x003B82F6;

    draw_rect(mx, my, menu_w, menu_h, bg);
    draw_rect(mx, my, menu_w, 2, accent);
    draw_rect(mx + menu_w - 1, my, 1, menu_h, 0x00334155);

    /* Left pane: Pinned list */
    draw_text(mx + 14, my + 14, "Pinned Apps", COL_TEXT_WHITE, bg);
    int srv_count = wm_get_service_count();
    int ey = my + 40;
    for (int i = 0; i < srv_count && i < 12; i++) {
        const wm_service_t *srv = wm_get_service_by_index(i);
        if (srv && srv->name) {
            draw_text(mx + 18, ey, srv->name, COL_TEXT_CYAN, bg);
            ey += 24;
        }
    }

    /* Right pane: Live Tiles */
    int tile_x = mx + 160;
    draw_text(tile_x, my + 14, "Live Tiles", COL_TEXT_WHITE, bg);
    draw_rect(tile_x, my + 40, 80, 80, 0x002563EB);
    draw_text(tile_x + 8, my + 100, "Snake", COL_TEXT_WHITE, 0x002563EB);
    draw_rect(tile_x + 90, my + 40, 80, 80, 0x0010B981);
    draw_text(tile_x + 98, my + 100, "Python", COL_TEXT_WHITE, 0x0010B981);
    draw_rect(tile_x, my + 130, 80, 80, 0x00D97706);
    draw_text(tile_x + 8, my + 190, "Files", COL_TEXT_WHITE, 0x00D97706);
    draw_rect(tile_x + 90, my + 130, 80, 80, 0x007C3AED);
    draw_text(tile_x + 98, my + 190, "Debug", COL_TEXT_WHITE, 0x007C3AED);

    draw_rect(mx + 14, my + menu_h - 36, menu_w - 28, 1, 0x00334155);
    draw_text(mx + 18, my + menu_h - 26, "Power / Shutdown", COL_TEXT_RED, bg);
}

static void win10_render_context_menu(void) {
    modern_render_context_menu();
}

static void win10_render_status_bar(rtc_time_t *t) {
    (void)t; /* Win10 has no top status bar */
}

static wm_de_t de_win10 = {
    WM_DE_WIN10, "Windows 10",
    win10_draw_wallpaper,
    win10_render_taskbar,
    win10_render_start_menu,
    win10_render_context_menu,
    win10_render_window_chrome,
    win10_render_status_bar,
};

/* ── DE Descriptor Accessor (O(1) Direct Lookup) ─────────────────────────── */
static wm_de_t *g_de_map[3] = { &de_classic, &de_modern, &de_win10 };

wm_de_t *wm_get_de(int de_id) {
    if (de_id >= 0 && de_id <= 2) return g_de_map[de_id];
    return &de_win10;
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  PUBLIC DISPATCH WRAPPERS (call g_de->* with NULL safety)                 */
/* ────────────────────────────────────────────────────────────────────────── */
void draw_wallpaper(void) {
    if (g_de && g_de->draw_wallpaper) g_de->draw_wallpaper();
}

void render_taskbar(rtc_time_t *t, bool start_open) {
    int notif_count = 0;
    for (int i = 0; i < WM_NOTIF_MAX; i++)
        if (g_notif_queue[i].active) notif_count++;
    if (g_de && g_de->render_taskbar) g_de->render_taskbar(t, start_open, notif_count);
}

void render_start_menu(void) {
    if (g_de && g_de->render_start_menu) g_de->render_start_menu();
}

void render_context_menu(void) {
    if (g_de && g_de->render_context_menu) g_de->render_context_menu();
}

/* ────────────────────────────────────────────────────────────────────────── */
/*  TOAST NOTIFICATION RENDERER                                               */
/* ────────────────────────────────────────────────────────────────────────── */
/**
 * render_toast_notifications: Draws active toasts stacked in the bottom-right
 * corner, above the taskbar. Each toast shows a colored accent bar on the left,
 * the message text, and a thin countdown progress bar at the bottom.
 * Toasts gracefully expire via wm_tick_notifications() each dirty frame.
 */
void render_toast_notifications(void) {
    #define TOAST_W  220
    #define TOAST_H  32
    #define TOAST_M  4   /* margin between toasts */

    int ty = SCREEN_H - TASKBAR_H - TOAST_M;
    int slot = 0;

    for (int i = WM_NOTIF_MAX - 1; i >= 0; i--) {
        if (!g_notif_queue[i].active) continue;
        int tx = SCREEN_W - TOAST_W - 8;
        int cy = ty - TOAST_H - (slot * (TOAST_H + TOAST_M));

        /* Background */
        uint32_t bg = blend_col(COL_CTX_BG, 0x00000000, 30);
        draw_rect(tx, cy, TOAST_W, TOAST_H, bg);

        /* Left accent stripe */
        draw_rect(tx, cy, 4, TOAST_H, g_notif_queue[i].color);

        /* Top border */
        draw_rect(tx, cy, TOAST_W, 1, g_notif_queue[i].color);

        /* Message */
        draw_text(tx + 8, cy + 4, g_notif_queue[i].msg, COL_TEXT_WHITE, bg);

        /* Progress bar (shrinks as ticks decrease) */
        int progress_w = (TOAST_W - 4) * g_notif_queue[i].ticks / WM_NOTIF_TICKS;
        draw_rect(tx + 4, cy + TOAST_H - 3, TOAST_W - 4, 2, darken(bg, 4));
        draw_rect(tx + 4, cy + TOAST_H - 3, progress_w,  2, g_notif_queue[i].color);

        slot++;
    }

    wm_tick_notifications();
}
