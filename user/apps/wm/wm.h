/**
 * wm.h — AzamiOS Modular Window Manager v5.0
 *
 * ARCHITECTURAL OVERVIEW:
 * This header defines the core interfaces for the AzamiOS Window Manager.
 *
 * v5.0 additions:
 *   - O(1) Service Lookup:   wm_get_service() uses direct table indexing (constant time).
 *   - O(1) Window Lookup:    find_win_by_type() uses direct table indexing (constant time).
 *   - -Ofast Optimization:   Compiled with maximum optimization for extreme rendering speed.
 *
 * v4.0 additions:
 *   - wm_de_t:           Desktop Environment abstraction — all rendering routes through
 *                        a single `g_de` pointer, swappable at runtime without restart.
 *   - wm_theme_t:        Runtime color palette — overrides compile-time COL_* defaults.
 *   - wm_notification_t: Toast notification ring buffer for system-level messages.
 *   - Settings service:  New WIN_SETTINGS window for live DE/theme switching.
 *
 * SAFETY PRINCIPLES (unchanged):
 *   1. Encapsulation: State changes route through validated API endpoints.
 *   2. Modularity:    Apps register as decoupled services via `wm_service_t`.
 *   3. Memory Safety: Explicit buffer lengths and bounded string helpers.
 */

#ifndef WM_H
#define WM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <gui.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

/* ── Screen constants ────────────────────────────────────────────────────── */
#define SCREEN_W       1280
#define SCREEN_H       800
#define TASKBAR_H      36
#define TITLEBAR_H     26
#define DESKTOP_H      (SCREEN_H - TASKBAR_H)
#define STATUS_BAR_H   20   /* Top status bar (Modern DE only) */

/* ── Terminal constants ──────────────────────────────────────────────────── */
#define TERM_COLS      80
#define TERM_ROWS      40

/* ── Notepad constants ───────────────────────────────────────────────────── */
#define NOTE_COLS      70
#define NOTE_ROWS      30

/* ── Base color palette (Classic DE / fallback) ──────────────────────────── */
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
#define COL_TEXT_YELLOW 0x00FBBF24
#define COL_TEXT_CYAN  0x0038BDF8
#define COL_TEXT_RED   0x00EF4444
#define COL_TEXT_BLUE  0x002563EB
#define COL_TEXT_GRAY  0x0094A3B8
#define COL_TERM_BG    0x00090D16
#define COL_NOTE_BG    0x00FFFEF5
#define COL_NOTE_LINE  0x00E2E8F0
#define COL_IDE_BG     0x000D1117
#define COL_IDE_OUT    0x00161B22
#define COL_CTX_BG     0x001E293B
#define COL_CTX_HI     0x002563EB
#define COL_TB_BTN     0x00253545
#define COL_TB_BTN_ACT 0x002563EB
#define COL_ICON_BLUE  0x003B82F6
#define COL_ICON_GREEN 0x0022C55E
#define COL_ICON_AMBER 0x00F59E0B
#define COL_ICON_GRAY  0x0064748B

/* ── Window type IDs ─────────────────────────────────────────────────────── */
#define MAX_WINS       24
#define WIN_WELCOME    0
#define WIN_TERMINAL   1
#define WIN_SYSMON     2
#define WIN_ABOUT      3
#define WIN_NOTEPAD    4
#define WIN_FILES      5
#define WIN_GLCUBE     6
#define WIN_PONG       7
#define WIN_IDE        8
#define WIN_SETTINGS   9
#define WIN_SNAKE      10
#define WIN_MINESWEEPER 11
#define WIN_TETRIS     12
#define WIN_PYTHON     13
#define WIN_DEBUGGER   14
#define WIN_BROWSER    15
#define WIN_NETMON     16

/* ── Service Capability Flags ────────────────────────────────────────────── */
#define WM_SRV_FLAG_NONE     0x00
#define WM_SRV_FLAG_ANIMATED 0x01  /* Requires continuous frame updates (e.g. 3D) */
#define WM_SRV_FLAG_OVERLAY  0x02  /* Renders above all windows (e.g. notifications) */
#define WM_SRV_FLAG_GAME     0x04  /* Game window */

/* ── Desktop Environment IDs ─────────────────────────────────────────────── */
#define WM_DE_CLASSIC  0
#define WM_DE_MODERN   1
#define WM_DE_WIN10    2

/* ── Max filesystem themes loadable at runtime ───────────────────────────── */
#define MAX_THEMES 8

typedef struct window window_t;

/**
 * struct window — Represents an active UI window instance.
 * SAFETY NOTE: `title` must always be modified via `wm_set_title()`.
 */
struct window {
    int  id;
    char title[32];
    int  x, y, w, h;        /* current position and size */
    int  ox, oy, ow, oh;    /* saved position for un-maximize */
    bool open;
    bool minimized;
    bool maximized;
    int  snap_state;        /* 0=none, 1=left, 2=right, 3=max */
    int  type;
};

/**
 * wm_service_t — Modular Application Service Descriptor.
 * Applications register via this struct; the core WM is fully decoupled
 * from specific application internals.
 */
typedef struct {
    int type;
    const char *name;
    uint32_t flags;
    void (*on_init)(window_t *w);
    void (*on_open)(window_t *w);
    void (*on_close)(window_t *w);
    void (*on_render)(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);
    void (*on_key)(window_t *w, int char_code, rtc_time_t *t, uint32_t frame_cnt);
} wm_service_t;

/**
 * wm_theme_t — Runtime color palette loaded from a *.theme file.
 * All fields are populated by wm_config.c at startup from the filesystem;
 * no colours are hardcoded in C source.
 */
typedef struct {
    int      id;
    char     name[32];       /* Human-readable name (from theme file) */
    char     font[16];       /* Font name, e.g. "default" (reserved for future) */
    uint32_t wall_top;
    uint32_t wall_bot;
    uint32_t taskbar;
    uint32_t taskbar_line;
    uint32_t start_btn;
    uint32_t title_active;
    uint32_t title_inactive;
    uint32_t win_frame;
    uint32_t win_body;
    uint32_t accent;         /* Primary accent color */
    uint32_t accent2;        /* Secondary accent */
    uint32_t text_primary;
    uint32_t text_secondary;
} wm_theme_t;

/**
 * wm_notification_t — A single toast notification entry.
 */
#define WM_NOTIF_MAX    4
#define WM_NOTIF_TICKS  180  /* ~6 seconds at ~30fps dirty ticks */

typedef struct {
    char     msg[48];
    uint32_t color;     /* accent color for the left bar */
    int      ticks;     /* countdown to zero = expired */
    bool     active;
} wm_notification_t;

/**
 * wm_de_t — Desktop Environment descriptor.
 * Swapping `g_de` to a different descriptor is all that is needed to
 * change the entire visual style at runtime. No restart required.
 */
typedef struct wm_de wm_de_t;
struct wm_de {
    int         id;
    const char *name;

    /* Core visual callbacks */
    void (*draw_wallpaper)(void);
    void (*render_taskbar)(rtc_time_t *t, bool start_open, int notif_count);
    void (*render_start_menu)(void);
    void (*render_context_menu)(void);

    /* Window chrome — called by render_window() for frame/titlebar */
    void (*render_window_chrome)(window_t *w, bool is_active);

    /* Status bar (Modern DE: top bar; Classic DE: no-op) */
    void (*render_status_bar)(rtc_time_t *t);
};

/* ── Global state ────────────────────────────────────────────────────────── */
extern window_t            g_wins[MAX_WINS];
extern int                 g_num_wins;
extern int                 g_focus;
extern bool                ctx_menu_open;
extern int                 ctx_menu_x, ctx_menu_y;
extern bool                start_menu;

/* ── DE / Theme / Notification globals ──────────────────────────────────── */
extern wm_de_t            *g_de;
extern wm_theme_t         *g_theme;
extern wm_notification_t   g_notif_queue[WM_NOTIF_MAX];

/* ── Theme registry (populated from *.theme files at startup) ───────────── */
extern wm_theme_t  g_themes[MAX_THEMES];
extern char        g_theme_filenames[MAX_THEMES][32];
extern int         g_num_themes;

/* ── Context menu dimensions ─────────────────────────────────────────────── */
#define CTX_MENU_W          148
#define CTX_MENU_ENTRY_H    22
#define CTX_MENU_ENTRIES    4
#define CTX_MENU_H          (CTX_MENU_ENTRIES * CTX_MENU_ENTRY_H + 8)

/* ── Start menu dimensions ───────────────────────────────────────────────── */
#define START_MENU_W        180
#define START_MENU_ENTRIES  10
#define START_ENTRY_H       24
#define START_HEADER_H      36
#define START_MENU_H        (START_HEADER_H + START_MENU_ENTRIES * START_ENTRY_H + 8)

/* ── Desktop icons ───────────────────────────────────────────────────────── */
typedef struct {
    int x, y;
    char label[16];
    int win_type;
} desktop_icon_t;

#define NUM_ICONS 14
extern const desktop_icon_t icons[NUM_ICONS];

/* ── Safe String Utilities ───────────────────────────────────────────────── */
void wm_strlcpy(char *dst, const char *src, size_t max_len);

/* ── Core Functions & Safe Lifecycle API ─────────────────────────────────── */
void init_wins(void);
int  find_win_by_type(int type);
void open_win_type(int type);
void render_window(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);
void render_window_body(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink);

/* Secure API mutators */
void wm_close_window(window_t *w);
void wm_resize_window(window_t *w, int new_w, int new_h);
void wm_set_title(window_t *w, const char *title);

/* DE & Theme switching (runtime, zero-restart) */
void wm_switch_de(int de_id);
void wm_switch_theme(int theme_idx);
void wm_push_notification(const char *msg, uint32_t color);
void wm_tick_notifications(void);

/* Filesystem configuration */
void        wm_config_init(void);          /* load themes + wm.conf from FS */
void        wm_save_conf(void);            /* persist active DE+theme to wm.conf */
int         wm_config_get_de(void);        /* cached de id from last load */
int         wm_config_get_theme_idx(void); /* cached theme idx from last load */
wm_theme_t *wm_get_theme(int idx);         /* get theme by index */

/* Full WM state persistence (exec() round-trip) */
void wm_save_full_state(void);   /* call before exec() */
bool wm_load_full_state(void);   /* call at startup, returns true if restored */
void wm_delete_full_state(void); /* call after successful restore */

/* Modularity Service Registry */
void              wm_register_service(const wm_service_t *service);
const wm_service_t *wm_get_service(int type);
int               wm_get_service_count(void);
const wm_service_t *wm_get_service_by_index(int idx);

/* Desktop UI (routed through g_de internally) */
void draw_wallpaper(void);
void render_desktop_icons(void);
void render_taskbar(rtc_time_t *t, bool start_open);
void render_start_menu(void);
void render_context_menu(void);
void render_toast_notifications(void);

/* DE descriptor accessor (implemented in desktop.c) */
wm_de_t *wm_get_de(int de_id);

/* Helpers */
void format_time_str(char *buf, rtc_time_t *t);
void format_date_str(char *buf, rtc_time_t *t);

/* Icon helpers */
void draw_icon_terminal(int x, int y);
void draw_icon_system(int x, int y);
void draw_icon_folder(int x, int y);
void draw_icon_notepad(int x, int y);
void draw_icon_ide(int x, int y);
void draw_icon_settings(int x, int y);
void draw_icon_game(int x, int y);
void draw_icon_python(int x, int y);
void draw_icon_debug(int x, int y);
void draw_icon_browser(int x, int y);

/* Service registrations */
void welcome_service_init(void);
void terminal_service_init(void);
void notepad_service_init(void);
void sysmon_service_init(void);
void settings_handle_mouse_click(int mx, int my);
void files_service_init(void);
void glcube_service_init(void);
void pong_service_init(void);
void ide_service_init(void);
void settings_service_init(void);
void snake_service_init(void);
void minesweeper_service_init(void);
void tetris_service_init(void);
void python_repl_service_init(void);
void debugger_service_init(void);
void browser_service_init(void);
void netmon_service_init(void);

#endif
