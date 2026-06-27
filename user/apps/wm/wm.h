#ifndef WM_H
#define WM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gui.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Screen constants ────────────────────────────────────────────────────── */
#define SCREEN_W       640
#define SCREEN_H       480
#define TASKBAR_H      24
#define TITLEBAR_H     22
#define DESKTOP_H      (SCREEN_H - TASKBAR_H)

/* ── Terminal constants ──────────────────────────────────────────────────── */
#define TERM_COLS      48
#define TERM_ROWS      26

/* ── Notepad constants ───────────────────────────────────────────────────── */
#define NOTE_COLS      44
#define NOTE_ROWS      20

/* ── Color palette ───────────────────────────────────────────────────────── */
#define COL_WALL_TOP   0x00070D1A
#define COL_WALL_BOT   0x00162544
#define COL_TASKBAR    0x001A2332
#define COL_TASKBAR_LN 0x002563EB
#define COL_START_BTN  0x002563EB
#define COL_START_ACT  0x00475569
#define COL_WIN_FRAME  0x00F0F4F8
#define COL_WIN_BODY   0x00FFFFFF
#define COL_WIN_SHAD   0x00040810
#define COL_TITLE_ACT  0x001D4ED8
#define COL_TITLE_INA  0x0064748B
#define COL_BTN_CLOSE  0x00EF4444
#define COL_BTN_MAX    0x0010B981
#define COL_BTN_MIN    0x00F59E0B
#define COL_TEXT_WHITE 0x00FFFFFF
#define COL_TEXT_DARK  0x000F172A
#define COL_TEXT_GREEN 0x0010B981
#define COL_TEXT_GOLD  0x00F59E0B
#define COL_TEXT_CYAN  0x0038BDF8
#define COL_TEXT_RED   0x00EF4444
#define COL_TEXT_BLUE  0x002563EB
#define COL_TEXT_GRAY  0x0094A3B8
#define COL_TERM_BG    0x00090D16
#define COL_NOTE_BG    0x00FFFEF5
#define COL_NOTE_LINE  0x00E2E8F0
#define COL_CTX_BG     0x001E293B
#define COL_CTX_HI     0x002563EB
#define COL_TB_BTN     0x00253545
#define COL_TB_BTN_ACT 0x002563EB
#define COL_ICON_BLUE  0x003B82F6
#define COL_ICON_GREEN 0x0022C55E
#define COL_ICON_AMBER 0x00F59E0B
#define COL_ICON_GRAY  0x0064748B

/* Window types */
#define MAX_WINS 8
#define WIN_WELCOME  0
#define WIN_TERMINAL 1
#define WIN_SYSMON   2
#define WIN_ABOUT    3
#define WIN_NOTEPAD  4
#define WIN_FILES    5
#define WIN_GLCUBE   6

typedef struct window window_t;

struct window {
    int  id;
    char title[32];
    int  x, y, w, h;        /* current position and size */
    int  ox, oy, ow, oh;    /* saved position for un-maximize */
    bool open;
    bool minimized;
    bool maximized;
    int  type;
};

/* Modular service interface */
typedef struct {
    int type;
    const char *name;
    void (*on_init)(window_t *w);
    void (*on_open)(window_t *w);
    void (*on_render)(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);
    void (*on_key)(window_t *w, int char_code, rtc_time_t *t, uint32_t frame_cnt);
} wm_service_t;

/* Global state */
extern window_t g_wins[MAX_WINS];
extern int g_num_wins;
extern int g_focus;
extern bool ctx_menu_open;
extern int ctx_menu_x, ctx_menu_y;
extern bool start_menu;

/* Context menu dimensions */
#define CTX_MENU_W 140
#define CTX_MENU_ENTRY_H 22
#define CTX_MENU_ENTRIES 4
#define CTX_MENU_H (CTX_MENU_ENTRIES * CTX_MENU_ENTRY_H + 8)

/* Start menu dimensions */
#define START_MENU_W 160
#define START_MENU_ENTRIES 8
#define START_ENTRY_H 24
#define START_HEADER_H 28
#define START_MENU_H (START_HEADER_H + START_MENU_ENTRIES * START_ENTRY_H + 8)

/* Desktop icons */
typedef struct {
    int x, y;
    char label[16];
    int win_type;
} desktop_icon_t;

#define NUM_ICONS 5
extern const desktop_icon_t icons[NUM_ICONS];

/* Functions */
void init_wins(void);
int find_win_by_type(int type);
void open_win_type(int type);
void render_window(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);
void render_window_body(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);

void wm_register_service(const wm_service_t *service);
const wm_service_t *wm_get_service(int type);

/* Desktop UI */
void draw_wallpaper(void);
void render_desktop_icons(void);
void render_taskbar(rtc_time_t *t, bool start_open);
void render_start_menu(void);
void render_context_menu(void);

/* Helpers */
void format_time_str(char *buf, rtc_time_t *t);
void format_date_str(char *buf, rtc_time_t *t);

/* Icon helpers */
void draw_icon_terminal(int x, int y);
void draw_icon_system(int x, int y);
void draw_icon_folder(int x, int y);
void draw_icon_notepad(int x, int y);

/* Service registrations */
void welcome_service_init(void);
void terminal_service_init(void);
void notepad_service_init(void);
void sysmon_service_init(void);
void files_service_init(void);
void glcube_service_init(void);

#endif
