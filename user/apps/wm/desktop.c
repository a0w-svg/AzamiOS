#include "wm.h"

/* ── Global UI state ─────────────────────────────────────────────────────── */
bool ctx_menu_open = false;
int  ctx_menu_x = 0, ctx_menu_y = 0;
bool start_menu = false;

/* ── Desktop icons array ─────────────────────────────────────────────────── */
const desktop_icon_t icons[NUM_ICONS] = {
    {16,  20, "System",    WIN_SYSMON},
    {16,  85, "Terminal",  WIN_TERMINAL},
    {16, 150, "Notepad",   WIN_NOTEPAD},
    {16, 215, "Files",     WIN_FILES},
    {16, 280, "3D Cube",   WIN_GLCUBE},
};

/* ── Time formatting helpers ─────────────────────────────────────────────── */
void format_time_str(char *buf, rtc_time_t *t) {
    char tmp[16]; int p = 0;
    itoa(t->hour, tmp, 10);
    if (t->hour < 10) buf[p++] = '0';
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->minute, tmp, 10);
    if (t->minute < 10) buf[p++] = '0';
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = ':';
    itoa(t->second, tmp, 10);
    if (t->second < 10) buf[p++] = '0';
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p] = 0;
}

void format_date_str(char *buf, rtc_time_t *t) {
    char tmp[16]; int p = 0;
    itoa(t->day, tmp, 10);
    if (t->day < 10) buf[p++] = '0';
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p++] = '/';
    itoa(t->month, tmp, 10);
    if (t->month < 10) buf[p++] = '0';
    for (int i = 0; tmp[i]; i++) buf[p++] = tmp[i];
    buf[p] = 0;
}

/* ── Pixel art icon drawing ──────────────────────────────────────────────── */
void draw_icon_terminal(int x, int y) {
    /* Dark terminal rectangle with >_ prompt */
    draw_rect(x, y, 24, 24, COL_TERM_BG);
    draw_rect(x, y, 24, 5, COL_ICON_GRAY);
    draw_rect(x + 1, y + 1, 3, 3, COL_BTN_CLOSE);
    draw_rect(x + 5, y + 1, 3, 3, COL_BTN_MIN);
    draw_rect(x + 9, y + 1, 3, 3, COL_BTN_MAX);
    draw_text(x + 3, y + 9, ">_", COL_TEXT_GREEN, COL_TERM_BG);
    draw_rect(x + 3, y + 19, 10, 2, COL_TEXT_GREEN);
}

void draw_icon_system(int x, int y) {
    /* Gear icon approximation using rectangles */
    draw_rect(x, y, 24, 24, COL_ICON_BLUE);
    draw_rect(x + 2, y + 2, 20, 20, COL_WIN_FRAME);
    /* Gear teeth */
    draw_rect(x + 10, y, 4, 4, COL_ICON_BLUE);
    draw_rect(x + 10, y + 20, 4, 4, COL_ICON_BLUE);
    draw_rect(x, y + 10, 4, 4, COL_ICON_BLUE);
    draw_rect(x + 20, y + 10, 4, 4, COL_ICON_BLUE);
    /* Center circle */
    draw_rect(x + 8, y + 8, 8, 8, COL_ICON_BLUE);
    draw_rect(x + 10, y + 10, 4, 4, COL_WIN_FRAME);
}

void draw_icon_folder(int x, int y) {
    /* Folder shape */
    draw_rect(x, y + 4, 24, 20, COL_ICON_AMBER);
    draw_rect(x, y + 2, 10, 4, COL_ICON_AMBER);
    draw_rect(x + 1, y + 7, 22, 15, 0x00FCD34D);
    draw_rect(x + 2, y + 9, 20, 2, COL_ICON_AMBER);
}

void draw_icon_notepad(int x, int y) {
    /* Paper with lines */
    draw_rect(x + 2, y, 20, 24, COL_WIN_BODY);
    draw_rect(x + 2, y, 20, 1, COL_ICON_GRAY);
    draw_rect(x + 2, y + 23, 20, 1, COL_ICON_GRAY);
    draw_rect(x + 2, y, 1, 24, COL_ICON_GRAY);
    draw_rect(x + 21, y, 1, 24, COL_ICON_GRAY);
    /* Lines of text */
    draw_rect(x + 5, y + 5, 14, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 9, 12, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 13, 14, 1, COL_TEXT_BLUE);
    draw_rect(x + 5, y + 17, 8, 1, COL_TEXT_BLUE);
    /* Pencil */
    draw_rect(x + 16, y + 14, 2, 8, COL_ICON_AMBER);
    draw_rect(x + 16, y + 22, 2, 2, COL_TEXT_DARK);
}

void draw_icon_cube(int x, int y) {
    draw_rect(x + 4, y, 16, 24, COL_TEXT_CYAN);
    draw_rect(x + 6, y + 4, 12, 16, COL_TITLE_ACT);
    draw_rect(x + 10, y + 8, 4, 8, COL_TEXT_GOLD);
}

/* ── Wallpaper rendering (vertical gradient) ─────────────────────────────── */
void draw_wallpaper(void) {
    /* Draw vertical gradient from COL_WALL_TOP to COL_WALL_BOT */
    uint8_t r1 = (COL_WALL_TOP >> 16) & 0xFF, g1 = (COL_WALL_TOP >> 8) & 0xFF, b1 = COL_WALL_TOP & 0xFF;
    uint8_t r2 = (COL_WALL_BOT >> 16) & 0xFF, g2 = (COL_WALL_BOT >> 8) & 0xFF, b2 = COL_WALL_BOT & 0xFF;

    /* Draw in 20-pixel strips for high performance (23 strips for 460 pixels) */
    for (int y = 0; y < DESKTOP_H; y += 20) {
        int rr = r1 + (r2 - r1) * y / DESKTOP_H;
        int gg = g1 + (g2 - g1) * y / DESKTOP_H;
        int bb = b1 + (b2 - b1) * y / DESKTOP_H;
        uint32_t col = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
        draw_rect(0, y, SCREEN_W, 20, col);
    }

}

/* ── Render desktop icons ────────────────────────────────────────────────── */
void render_desktop_icons(void) {
    for (int i = 0; i < NUM_ICONS; i++) {
        int ix = icons[i].x, iy = icons[i].y;
        switch (icons[i].win_type) {
            case WIN_SYSMON:   draw_icon_system(ix, iy);    break;
            case WIN_TERMINAL: draw_icon_terminal(ix, iy);  break;
            case WIN_NOTEPAD:  draw_icon_notepad(ix, iy);   break;
            case WIN_FILES:    draw_icon_folder(ix, iy);    break;
            case WIN_GLCUBE:   draw_icon_cube(ix, iy);      break;
        }
        /* Label centered below icon */
        int lx = ix + 12 - (strlen(icons[i].label) * 4);
        draw_text(lx, iy + 28, icons[i].label, COL_TEXT_WHITE, 0xFFFFFFFF);
    }
}

/* ── Taskbar ─────────────────────────────────────────────────────────────── */
void render_taskbar(rtc_time_t *t, bool start_open) {
    int ty = SCREEN_H - TASKBAR_H;
    /* Background */
    draw_rect(0, ty, SCREEN_W, TASKBAR_H, COL_TASKBAR);
    /* Top accent line */
    draw_rect(0, ty, SCREEN_W, 2, COL_TASKBAR_LN);
    /* START button */
    draw_rect(4, ty + 4, 60, 16, start_open ? COL_START_ACT : COL_START_BTN);
    draw_text(12, ty + 8, "START", COL_TEXT_WHITE, start_open ? COL_START_ACT : COL_START_BTN);

    /* Window buttons */
    int btn_x = 72;
    for (int i = 0; i < g_num_wins; i++) {
        if (!g_wins[i].open) continue;
        if (btn_x + 72 > 500) break; /* don't overflow into clock area */
        uint32_t bcol = (g_focus == i) ? COL_TB_BTN_ACT : COL_TB_BTN;
        draw_rect(btn_x, ty + 4, 68, 16, bcol);
        /* Truncate title to fit */
        char lbl[10];
        strncpy(lbl, g_wins[i].title, 8);
        lbl[8] = 0;
        draw_text(btn_x + 4, ty + 8, lbl, COL_TEXT_WHITE, bcol);
        btn_x += 72;
    }

    /* Date + time */
    char date_buf[12], time_buf[12];
    format_date_str(date_buf, t);
    format_time_str(time_buf, t);
    draw_text(536, ty + 8, date_buf, COL_TEXT_GRAY, COL_TASKBAR);
    draw_text(580, ty + 8, time_buf, COL_TEXT_WHITE, COL_TASKBAR);
}

/* ── Start menu ──────────────────────────────────────────────────────────── */
void render_start_menu(void) {
    int mx = 4;
    int my = SCREEN_H - TASKBAR_H - START_MENU_H;
    /* Background */
    draw_rect(mx, my, START_MENU_W, START_MENU_H, COL_CTX_BG);
    /* Top accent */
    draw_rect(mx, my, START_MENU_W, 2, COL_TASKBAR_LN);
    /* Header */
    draw_rect(mx + 4, my + 4, START_MENU_W - 8, START_HEADER_H - 4, COL_START_BTN);
    draw_text(mx + 12, my + 10, "AzamiOS v3.0", COL_TEXT_WHITE, COL_START_BTN);

    int ey = my + START_HEADER_H + 2;
    /* Menu entries */
    static const char *labels[] = {
        "> Welcome",
        "> Terminal",
        "# System Monitor",
        "? About AzamiOS",
        "= Notepad",
        "@ File Manager",
        "* 3D OpenGL Demo",
        "X Shutdown"
    };
    static const uint32_t colors[] = {
        COL_TEXT_WHITE,
        COL_TEXT_CYAN,
        COL_TEXT_GOLD,
        COL_TEXT_BLUE,
        COL_TEXT_GREEN,
        COL_TEXT_GOLD,
        COL_TEXT_CYAN,
        COL_TEXT_RED,
    };
    for (int i = 0; i < START_MENU_ENTRIES; i++) {
        if (i == START_MENU_ENTRIES - 1) {
            /* Separator before Shutdown */
            draw_rect(mx + 8, ey, START_MENU_W - 16, 1, COL_ICON_GRAY);
            ey += 3;
        }
        draw_text(mx + 12, ey + 6, labels[i], colors[i], COL_CTX_BG);
        ey += START_ENTRY_H;
    }
}

/* ── Context menu ────────────────────────────────────────────────────────── */
void render_context_menu(void) {
    int mx = ctx_menu_x, my = ctx_menu_y;
    /* Clamp to screen */
    if (mx + CTX_MENU_W > SCREEN_W) mx = SCREEN_W - CTX_MENU_W;
    if (my + CTX_MENU_H > DESKTOP_H) my = DESKTOP_H - CTX_MENU_H;

    draw_rect(mx, my, CTX_MENU_W, CTX_MENU_H, COL_CTX_BG);
    draw_rect(mx, my, CTX_MENU_W, 2, COL_TASKBAR_LN);

    int ey = my + 6;
    draw_text(mx + 10, ey + 4, "= New Notepad", COL_TEXT_GREEN, COL_CTX_BG);
    ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "? About AzamiOS", COL_TEXT_CYAN, COL_CTX_BG);
    ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "@ File Manager", COL_TEXT_GOLD, COL_CTX_BG);
    ey += CTX_MENU_ENTRY_H;
    draw_text(mx + 10, ey + 4, "~ Refresh", COL_TEXT_GRAY, COL_CTX_BG);
}
