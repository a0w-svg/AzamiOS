/* ============================================================================
 * AzamiOS Desktop Environment — Modern App Launcher
 * File: userland/apps/launcher/main.c   v4.0
 *
 * Features:
 * ──────────
 *  • Floating Glassmorphism Modal Window with frosted gradients
 *  • Category Tabs: [All], [Productivity], [Games], [System], [Media]
 *  • Real-time search query filtering with instant feedback
 *  • Rich 32x32 vector/pixel icons with glowing hover tiles
 *  • Bottom User & Status Banner: "azami@azamios • AzamiOS v7.0 • SATA /hdd"
 *  • Full Keyboard Navigation (Arrows, Enter, Escape, Backspace, Type-to-Search)
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/stdlib.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

/* ── Configuration ─────────────────────────────────────────────────────────── */
#define SERVER_CHAN      1
#define DEFAULT_W    1280
#define DEFAULT_H     800
#define TASKBAR_H      52
#define LAUNCHER_MAP  ((void *)0x68000000)

/* Modal Geometry */
#define MODAL_W       680
#define MODAL_H       450
#define MODAL_RADIUS   14

/* Grid geometry */
#define GRID_COLS       4
#define GRID_ROWS       2
#define CELL_W        145
#define CELL_H        135
#define ICON_BOX_SIZE  52
#define HEADER_H       60
#define TABS_Y         68
#define TABS_H         30

/* Categories */
typedef enum {
    CAT_ALL          = 0,
    CAT_PRODUCTIVITY = 1,
    CAT_GAMES        = 2,
    CAT_SYSTEM       = 3,
    CAT_MEDIA        = 4,
    CAT_COUNT        = 5
} app_cat_t;

static const char *g_cat_names[CAT_COUNT] = {
    "All", "Productivity", "Games", "System", "Media"
};
static const int g_cat_widths[CAT_COUNT] = {
    48, 105, 68, 75, 65
};

static int g_selected_cat = CAT_ALL;

/* ── App descriptor ───────────────────────────────────────────────────────── */
typedef struct {
    char name[64];
    char subtitle[64];
    char path[128];
    app_cat_t category;
    unsigned int icon_data[32 * 32];
    int has_icon;
} app_entry_t;

#define MAX_APPS 32
static app_entry_t g_apps[MAX_APPS];
static int g_num_apps = 0;

/* ── Global state ─────────────────────────────────────────────────────────── */
static uk_window_t  g_win;
static int          g_hovered = -1;   /* index of hovered app slot, -1 = none */
static int          g_launching = 0;

static char g_search_query[64] = "";
static int  g_search_len = 0;
static int  g_filtered_indices[MAX_APPS];
static int  g_num_filtered = 0;

static void update_filter(void)
{
    g_num_filtered = 0;
    for (int i = 0; i < g_num_apps; i++) {
        /* Check category */
        if (g_selected_cat != CAT_ALL && g_apps[i].category != (app_cat_t)g_selected_cat) {
            continue;
        }

        if (g_search_len == 0) {
            g_filtered_indices[g_num_filtered++] = i;
        } else {
            char lower_name[64];
            char lower_sub[64];
            char lower_query[64];
            int k;
            for (k = 0; g_apps[i].name[k] && k < 63; k++) {
                char c = g_apps[i].name[k];
                lower_name[k] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            }
            lower_name[k] = '\0';
            for (k = 0; g_apps[i].subtitle[k] && k < 63; k++) {
                char c = g_apps[i].subtitle[k];
                lower_sub[k] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            }
            lower_sub[k] = '\0';
            for (k = 0; g_search_query[k] && k < 63; k++) {
                char c = g_search_query[k];
                lower_query[k] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
            }
            lower_query[k] = '\0';

            if (strstr(lower_name, lower_query) || strstr(lower_sub, lower_query)) {
                g_filtered_indices[g_num_filtered++] = i;
            }
        }
    }
    if (g_hovered >= g_num_filtered) g_hovered = (g_num_filtered > 0) ? 0 : -1;
}

/* ── Cell geometry helpers ────────────────────────────────────────────────── */
static void cell_rect(int idx, int *cx, int *cy, int *cw, int *ch)
{
    int col = idx % GRID_COLS;
    int row = idx / GRID_COLS;
    int total_grid_w = GRID_COLS * CELL_W;
    int start_x = ((int)g_win.width - total_grid_w) / 2;
    int start_y = HEADER_H + TABS_H + 10;

    *cx = start_x + col * CELL_W;
    *cy = start_y + row * CELL_H;
    *cw = CELL_W;
    *ch = CELL_H;
}

static int hit_cell(int mx, int my)
{
    int slot;
    for (slot = 0; slot < g_num_filtered; slot++) {
        int cx, cy, cw, ch;
        cell_rect(slot, &cx, &cy, &cw, &ch);
        if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch)
            return slot;
    }
    return -1;
}

/* ── Render single app cell ───────────────────────────────────────────────── */
static void draw_cell(int slot)
{
    if (slot < 0 || slot >= g_num_filtered) return;
    int app_idx = g_filtered_indices[slot];

    int cx, cy, cw, ch;
    cell_rect(slot, &cx, &cy, &cw, &ch);

    /* Erase cell background */
    uk_fill_rect(&g_win, cx, cy, cw, ch, (slot == g_hovered) ? 0xFF242436 : UK_MANTLE);

    if (slot == g_hovered) {
        /* Hover card glow */
        uk_fill_rounded_rect(&g_win, cx + 4, cy + 4, cw - 8, ch - 8, 10, UK_SURFACE0);
        uk_draw_rounded_rect_outline(&g_win, cx + 4, cy + 4, cw - 8, ch - 8, 10, UK_MAUVE);
    }

    /* Icon box: centred horizontally */
    int icon_x = cx + (cw - ICON_BOX_SIZE) / 2;
    int icon_y = cy + 10;

    /* Icon background tile with drop shadow */
    uk_fill_rounded_rect(&g_win, icon_x + 2, icon_y + 3,
                         ICON_BOX_SIZE, ICON_BOX_SIZE, 10, 0x55000000);
    uk_fill_rounded_rect(&g_win, icon_x, icon_y,
                         ICON_BOX_SIZE, ICON_BOX_SIZE, 10,
                         (slot == g_hovered) ? UK_SURFACE1 : UK_BASE);

    int draw_x = icon_x + (ICON_BOX_SIZE - 32) / 2;
    int draw_y = icon_y + (ICON_BOX_SIZE - 32) / 2;
    
    if (g_apps[app_idx].has_icon) {
        for (int py = 0; py < 32; py++) {
            for (int px = 0; px < 32; px++) {
                unsigned int c = g_apps[app_idx].icon_data[py * 32 + px];
                if (c & 0xFF000000) {
                    unsigned char a = (c >> 24) & 0xFF;
                    if (a == 255) {
                        uk_put_pixel(&g_win, draw_x + px, draw_y + py, c);
                    } else if (a > 0) {
                        int dest_x = draw_x + px;
                        int dest_y = draw_y + py;
                        if (dest_x >= 0 && dest_x < (int)g_win.width && dest_y >= 0 && dest_y < (int)g_win.height) {
                            unsigned int bg = g_win.pixels[dest_y * g_win.width + dest_x];
                            uk_put_pixel(&g_win, dest_x, dest_y, uk_blend(bg, c, a));
                        }
                    }
                }
            }
        }
    } else {
        const char *name = g_apps[app_idx].name;
        if (strstr(name, "setting") || strstr(name, "config") || strstr(name, "pref")) {
            uk_icon_settings(&g_win, draw_x, draw_y);
        } else if (strstr(name, "calc") || strstr(name, "math")) {
            uk_icon_calculator(&g_win, draw_x, draw_y);
        } else if (strstr(name, "clock") || strstr(name, "time")) {
            uk_icon_clock(&g_win, draw_x, draw_y);
        } else if (strstr(name, "sys") || strstr(name, "mon") || strstr(name, "top")) {
            uk_icon_sysmon(&g_win, draw_x, draw_y);
        } else if (strstr(name, "file") || strstr(name, "fm") || strstr(name, "dir")) {
            uk_icon_filemanager(&g_win, draw_x, draw_y);
        } else if (strstr(name, "term") || strstr(name, "sh") || strstr(name, "cli")) {
            uk_icon_terminal(&g_win, draw_x, draw_y);
        } else if (strstr(name, "about") || strstr(name, "info")) {
            uk_icon_about(&g_win, draw_x, draw_y);
        } else if (strstr(name, "edit") || strstr(name, "note") || strstr(name, "text")) {
            uk_icon_texteditor(&g_win, draw_x, draw_y);
        } else {
            /* Dynamic app badge fallback */
            unsigned int hash = 0;
            for (int k = 0; name[k]; k++) hash = hash * 31 + (unsigned char)name[k];
            unsigned int badge_colors[] = { UK_MAUVE, UK_BLUE, UK_SAPPHIRE, UK_TEAL, UK_GREEN, UK_PEACH, UK_LAVENDER };
            unsigned int badge_col = badge_colors[hash % (sizeof(badge_colors) / sizeof(badge_colors[0]))];

            uk_fill_rounded_rect(&g_win, draw_x, draw_y, 32, 32, 8, badge_col);
            uk_fill_rounded_rect(&g_win, draw_x + 2, draw_y + 2, 28, 28, 6, UK_BASE);

            char initial[2] = { name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0], '\0' };
            if (initial[0] >= 'A' && initial[0] <= 'Z') {
                uk_draw_text_2x(&g_win, draw_x + 8, draw_y + 6, initial, badge_col);
            } else {
                uk_icon_texteditor(&g_win, draw_x, draw_y);
            }
        }
    }

    /* App name (centred) */
    unsigned int name_col = (slot == g_hovered) ? UK_TEXT : UK_SUBTEXT1;
    int nlen = strlen(g_apps[app_idx].name);
    int nx   = cx + cw / 2 - (nlen * 8) / 2;
    int ny   = icon_y + ICON_BOX_SIZE + 8;
    uk_draw_text(&g_win, nx, ny, g_apps[app_idx].name, name_col);

    /* Subtitle (centred, dimmer) */
    int slen = strlen(g_apps[app_idx].subtitle);
    int sx   = cx + cw / 2 - (slen * 8) / 2;
    uk_draw_text(&g_win, sx, ny + 16, g_apps[app_idx].subtitle, UK_OVERLAY0);
}

/* ============================================================================
 * draw_launcher — render the entire floating launcher modal
 * ============================================================================ */
static void draw_launcher(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* ── Background: Smooth frosted gradient ──────────────────────────────── */
    uk_gradient_v(&g_win, 0, 0, (int)w, (int)h, UK_MANTLE, UK_CRUST);

    /* Surrounding glowing border outline */
    uk_hline(&g_win, 0, 0, (int)w, UK_MAUVE);
    uk_hline(&g_win, 0, (int)h - 1, (int)w, UK_SURFACE1);
    uk_vline(&g_win, 0, 0, (int)h, UK_MAUVE);
    uk_vline(&g_win, (int)w - 1, 0, (int)h, UK_SURFACE1);

    /* ── Header bar with search/brand aesthetic ───────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, HEADER_H, 0xFF242438, UK_BASE);

    /* Mauve accent indicator on left */
    uk_fill_rounded_rect(&g_win, 8, 12, 4, HEADER_H - 24, 2, UK_MAUVE);

    /* "Applications" title */
    uk_draw_text_2x(&g_win, 24, 12, "Applications", UK_TEXT);

    /* Subtitle hint */
    uk_draw_text(&g_win, 24, 38, "Type to search or browse categories", UK_OVERLAY0);

    /* Search box (pill style on right) */
    int sb_w = 200;
    int sb_h = 32;
    int sb_x = (int)w - sb_w - 20;
    int sb_y = 14;
    uk_fill_rounded_rect(&g_win, sb_x, sb_y, sb_w, sb_h, 8, (g_search_len > 0) ? UK_SURFACE1 : UK_SURFACE0);
    if (g_search_len > 0) {
        char sdisplay[68];
        strncpy(sdisplay, g_search_query, sizeof(sdisplay) - 2);
        sdisplay[sizeof(sdisplay) - 2] = '\0';
        int sl = strlen(sdisplay);
        sdisplay[sl] = '_';
        sdisplay[sl + 1] = '\0';
        uk_draw_text(&g_win, sb_x + 10, sb_y + 8, sdisplay, UK_MAUVE);
    } else {
        uk_draw_text(&g_win, sb_x + 10, sb_y + 8, "Search apps...", UK_OVERLAY1);
    }

    /* Divider */
    uk_hline(&g_win, 0, HEADER_H, (int)w, UK_SURFACE0);

    /* ── Category Tabs Bar ────────────────────────────────────────────────── */
    int tab_x = 24;
    for (int c = 0; c < CAT_COUNT; c++) {
        int tw = g_cat_widths[c];
        bool is_sel = (c == g_selected_cat);
        unsigned int tab_bg = is_sel ? UK_MAUVE : UK_SURFACE0;
        unsigned int tab_fg = is_sel ? UK_CRUST : UK_TEXT;

        uk_fill_rounded_rect(&g_win, tab_x, TABS_Y, tw, 24, 6, tab_bg);
        uk_draw_text_centred(&g_win, tab_x + tw / 2, TABS_Y + 4, g_cat_names[c], tab_fg);
        tab_x += tw + 8;
    }

    /* ── App grid ─────────────────────────────────────────────────────────── */
    for (int slot = 0; slot < g_num_filtered; slot++) {
        draw_cell(slot);
    }

    if (g_num_filtered == 0) {
        uk_draw_text_centred(&g_win, (int)w / 2, (int)h / 2 + 10, "No matching applications found", UK_OVERLAY1);
    }

    /* ── Footer Banner Bar ───────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, (int)h - 28, (int)w, 28, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 28, (int)w, UK_SURFACE1);

    uk_draw_text(&g_win, 16, (int)h - 20, "User: azami (UID 1000) • AzamiOS v7.0", UK_SUBTEXT0);
    uk_draw_text(&g_win, (int)w - 240, (int)h - 20, "Enter: Launch | Esc: Dismiss", UK_OVERLAY1);

    uk_invalidate(&g_win);
}

/* ============================================================================
 * _start
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[launcher] AzamiOS Modern App Launcher starting...");

    static const char *scan_dirs[] = { "/bin", "/sbin", "/", NULL };
    for (int s = 0; scan_dirs[s] != NULL; s++) {
        const char *sdir = scan_dirs[s];
        DIR *dir = opendir(sdir);
        if (dir) {
            struct dirent *d;
            while ((d = readdir(dir)) != NULL) {
                if (d->d_name[0] != '.') {
                    int len = strlen(d->d_name);
                    if (len > 4 && strcmp(d->d_name + len - 4, ".elf") == 0) {
                        int name_len = len - 4;
                        if (name_len > 63) name_len = 63;
                        char appname[64];
                        strncpy(appname, d->d_name, name_len);
                        appname[name_len] = '\0';

                        /* Filter out internal system services & CLI tools */
                        static const char *cli_list[] = {
                            "init", "sessiond", "azwm", "wallpaper", "taskbar", "launcher", "gui_test",
                            "sh", "play", "ls", "cat", "echo", "pwd", "uname", "head", "tail", "wc",
                            "grep", "mkdir", "rm", "touch", "cp", "mv", "date", "uptime", "df", "free",
                            "ifconfig", "ping", "lspci", "env", "which", "sleep", "kill", "chmod", "clear",
                            "hexdump", "base64", "md5sum", "cut", "sort", "uniq", "ps", "top", "dmesg",
                            "tree", "cal", "watch", "netstat", "poweroff", "reboot", "getfacl", "setfacl", NULL
                        };
                        int is_cli = 0;
                        for (int k = 0; cli_list[k] != NULL; k++) {
                            if (strcmp(appname, cli_list[k]) == 0) {
                                is_cli = 1;
                                break;
                            }
                        }
                        if (is_cli) continue;

                        int already_added = 0;
                        for (int a = 0; a < g_num_apps; a++) {
                            if (strcmp(g_apps[a].name, appname) == 0) {
                                already_added = 1;
                                break;
                            }
                        }
                        if (already_added) continue;

                        if (g_num_apps < MAX_APPS) {
                            app_entry_t *app = &g_apps[g_num_apps++];
                            strncpy(app->name, appname, sizeof(app->name) - 1);
                            app->name[sizeof(app->name) - 1] = '\0';

                            const char *sub = "Application";
                            app_cat_t cat = CAT_PRODUCTIVITY;

                            if (strcmp(appname, "terminal") == 0) { sub = "Command Terminal"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "filemanager") == 0) { sub = "Files & Storage"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "texteditor") == 0) { sub = "Code & Text Editor"; cat = CAT_PRODUCTIVITY; }
                            else if (strcmp(appname, "sysmon") == 0) { sub = "Activity Monitor"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "calculator") == 0) { sub = "Math & Calculations"; cat = CAT_PRODUCTIVITY; }
                            else if (strcmp(appname, "clock") == 0) { sub = "Clock & World Times"; cat = CAT_PRODUCTIVITY; }
                            else if (strcmp(appname, "settings") == 0) { sub = "System Control"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "about") == 0) { sub = "System Info"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "fetch") == 0) { sub = "System Telemetry"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "screenshot") == 0) { sub = "Screen Capture"; cat = CAT_SYSTEM; }
                            else if (strcmp(appname, "paint") == 0) { sub = "Paint & Sketch"; cat = CAT_PRODUCTIVITY; }
                            else if (strcmp(appname, "audioplayer") == 0) { sub = "Music & Synthwave"; cat = CAT_MEDIA; }
                            else if (strcmp(appname, "minesweeper") == 0) { sub = "Minesweeper Puzzle"; cat = CAT_GAMES; }
                            else if (strcmp(appname, "2048") == 0) { sub = "2048 Number Puzzle"; cat = CAT_GAMES; }
                            else if (strcmp(appname, "snake") == 0) { sub = "Arcade Snake Game"; cat = CAT_GAMES; }

                            strncpy(app->subtitle, sub, sizeof(app->subtitle) - 1);
                            app->subtitle[sizeof(app->subtitle) - 1] = '\0';
                            app->category = cat;
                            
                            if (strcmp(sdir, "/") == 0) {
                                snprintf(app->path, sizeof(app->path), "/%s", d->d_name);
                            } else {
                                snprintf(app->path, sizeof(app->path), "%s/%s", sdir, d->d_name);
                            }
                            
                            char icn_path[128];
                            snprintf(icn_path, sizeof(icn_path), "/usr/share/icons/%s.icn", app->name);
                            int icn_fd = sys_open(icn_path, 0, 0);
                            if (icn_fd < 0) {
                                snprintf(icn_path, sizeof(icn_path), "%s/%s.icn", sdir, app->name);
                                icn_fd = sys_open(icn_path, 0, 0);
                            }
                            if (icn_fd < 0) {
                                snprintf(icn_path, sizeof(icn_path), "/%s.icn", app->name);
                                icn_fd = sys_open(icn_path, 0, 0);
                            }

                            if (icn_fd >= 0) {
                                int nr = sys_read(icn_fd, app->icon_data, sizeof(app->icon_data));
                                app->has_icon = (nr == (int)sizeof(app->icon_data)) ? 1 : 0;
                                sys_close(icn_fd);
                            } else {
                                app->has_icon = 0;
                            }
                        }
                    }
                }
            }
            closedir(dir);
        }
    }

    az_fb_info_t fb;
    unsigned int sw = DEFAULT_W, sh = DEFAULT_H;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int win_x = (int)(sw - MODAL_W) / 2;
    int win_y = (int)(sh - TASKBAR_H - MODAL_H) / 2;
    if (win_y < 20) win_y = 20;

    if (uk_window_connect(&g_win, "", win_x, win_y, MODAL_W, MODAL_H,
                          LAUNCHER_MAP, SERVER_CHAN) < 0) {
        de_log("[launcher] FATAL: Failed to create window");
        sys_exit(1);
    }

    uk_set_zorder(&g_win, AZ_WM_ZORDER_TOP);
    update_filter();
    draw_launcher();

    az_ipc_msg_t raw_msg;
    az_wm_msg_t *msg = (az_wm_msg_t *)&raw_msg;

    de_log("[launcher] Entering event loop.");

    for (;;) {
        if (az_channel_recv(g_win.client_chan, &raw_msg) != 0) {
            continue;
        }

        switch (msg->type) {
        case AZ_WM_MOUSE_EVENT: {
            int mx = msg->mouse.abs_x;
            int my = msg->mouse.abs_y;

            if (msg->mouse.buttons & AZ_MOUSE_BTN_LEFT) {
                /* Check Category Tabs click */
                if (my >= TABS_Y && my <= TABS_Y + 24) {
                    int tx = 24;
                    for (int c = 0; c < CAT_COUNT; c++) {
                        int tw = g_cat_widths[c];
                        if (mx >= tx && mx <= tx + tw) {
                            if (g_selected_cat != c) {
                                g_selected_cat = c;
                                update_filter();
                                draw_launcher();
                            }
                            break;
                        }
                        tx += tw + 8;
                    }
                }

                int clicked = hit_cell(mx, my);
                if (clicked >= 0 && clicked < g_num_filtered && !g_launching) {
                    g_launching = 1;
                    int app_idx = g_filtered_indices[clicked];
                    uk_launch_app(&g_win, g_apps[app_idx].path);
                    sys_exit(0);
                }
            } else {
                int new_hover = hit_cell(mx, my);
                if (new_hover != g_hovered) {
                    g_hovered = new_hover;
                    draw_launcher();
                }
            }
            break;
        }

        case AZ_WM_KEY_EVENT: {
            if (!msg->key.pressed) break;

            if (msg->key.keycode == 0x1B) { /* Escape */
                if (!g_launching) {
                    g_launching = 1;
                    sys_exit(0);
                }
            } else if (msg->key.keycode == '\n' || msg->key.keycode == '\r') {
                if (g_hovered >= 0 && g_hovered < g_num_filtered && !g_launching) {
                    g_launching = 1;
                    int app_idx = g_filtered_indices[g_hovered];
                    uk_launch_app(&g_win, g_apps[app_idx].path);
                    sys_exit(0);
                }
            } else if (msg->key.keycode == 0x08) { /* Backspace */
                if (g_search_len > 0) {
                    g_search_query[--g_search_len] = '\0';
                    update_filter();
                    draw_launcher();
                }
            } else if (msg->key.keycode >= 32 && msg->key.keycode <= 126) {
                if (g_search_len < 60) {
                    g_search_query[g_search_len++] = (char)msg->key.keycode;
                    g_search_query[g_search_len] = '\0';
                    update_filter();
                    draw_launcher();
                }
            }
            break;
        }

        case AZ_WM_FOCUS_CHANGE: {
            if (!msg->focus.focused && !g_launching) {
                g_launching = 1;
                sys_exit(0);
            }
            break;
        }

        default:
            break;
        }
    }

    return 0;
}
