/* ============================================================================
 * AzamiOS Desktop Environment — Modern App Launcher
 * File: userland/apps/launcher/main.c   v3.0
 *
 * v3.0 Modernization
 * ──────────────────
 *  • Bug 4 fix: g_launching flag prevents double-destroy / focus loss race
 *  • Floating Glassmorphism Design: centered floating modal window (680×440px)
 *    rather than full-screen bottom bar
 *  • Deep Catppuccin Mocha theme with frosted glass aesthetics
 *  • 4×2 app grid with 64px icon areas and glowing hover effects
 *  • Fast, responsive startup and clean exit on focus loss / Escape
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/dirent.h"
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
#define MODAL_H       420
#define MODAL_RADIUS   14

/* Grid geometry */
#define GRID_COLS       4
#define GRID_ROWS       2
#define CELL_W        145
#define CELL_H        140
#define ICON_BOX_SIZE  52
#define HEADER_H       60

/* ── App descriptor ───────────────────────────────────────────────────────── */
typedef struct {
    char name[64];
    char subtitle[64];
    char path[128];
    unsigned int icon_data[32 * 32];
    int has_icon;
} app_entry_t;

#define MAX_APPS 32
static app_entry_t g_apps[MAX_APPS];
static int g_num_apps = 0;

/* ── Global state ─────────────────────────────────────────────────────────── */
static uk_window_t  g_win;
static int          g_hovered = -1;   /* index of hovered app slot, -1 = none */
static int          g_launching = 0;  /* Bug 4 fix: race prevention flag */

static char g_search_query[64] = "";
static int  g_search_len = 0;
static int  g_filtered_indices[MAX_APPS];
static int  g_num_filtered = 0;

static void update_filter(void)
{
    g_num_filtered = 0;
    for (int i = 0; i < g_num_apps; i++) {
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
    int start_y = HEADER_H + 15;

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

static void draw_cell(int slot)
{
    if (slot < 0 || slot >= g_num_filtered) return;
    int app_idx = g_filtered_indices[slot];
    int cx, cy, cw, ch;
    cell_rect(slot, &cx, &cy, &cw, &ch);

    /* Background cell area */
    uk_fill_rect(&g_win, cx, cy, cw, ch, UK_MANTLE);

    /* Cell background: highlight and frosted pill on hover */
    if (slot == g_hovered) {
        uk_fill_rounded_rect(&g_win, cx + 6, cy + 4,
                             cw - 12, ch - 8, 10, UK_SURFACE0);
        /* Subtle glow border */
        int bx, by;
        for (bx = cx + 6; bx < cx + cw - 6; bx++) {
            uk_put_pixel(&g_win, bx, cy + 4,       UK_MAUVE);
            uk_put_pixel(&g_win, bx, cy + ch - 5,  UK_MAUVE);
        }
        for (by = cy + 4; by < cy + ch - 4; by++) {
            uk_put_pixel(&g_win, cx + 6,      by,  UK_MAUVE);
            uk_put_pixel(&g_win, cx + cw - 7, by,  UK_MAUVE);
        }
    }

    /* Icon box: centred horizontally */
    int icon_x = cx + (cw - ICON_BOX_SIZE) / 2;
    int icon_y = cy + 12;

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
    int ny   = icon_y + ICON_BOX_SIZE + 10;
    uk_draw_text(&g_win, nx, ny, g_apps[app_idx].name, name_col);

    /* Subtitle (centred, dimmer) */
    int slen = strlen(g_apps[app_idx].subtitle);
    int sx   = cx + cw / 2 - (slen * 8) / 2;
    uk_draw_text(&g_win, sx, ny + 16, g_apps[app_idx].subtitle, UK_OVERLAY0);
}

static void draw_hover_update(int prev_hover, int new_hover)
{
    if (prev_hover >= 0) draw_cell(prev_hover);
    if (new_hover >= 0) draw_cell(new_hover);
    uk_invalidate(&g_win);
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
    uk_draw_text(&g_win, 24, 38, "Type to search or select an app", UK_OVERLAY0);

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

    /* ── App grid ─────────────────────────────────────────────────────────── */
    int slot;
    for (slot = 0; slot < g_num_filtered; slot++) {
        draw_cell(slot);
    }

    if (g_num_filtered == 0) {
        uk_draw_text_centred(&g_win, (int)w / 2, (int)h / 2, "No matching applications found", UK_OVERLAY1);
    }

    /* ── Footer bar ───────────────────────────────────────────────────────── */
    uk_hline(&g_win, 0, (int)h - 26, (int)w, UK_SURFACE0);
    uk_draw_text_centred(&g_win, (int)w / 2, (int)h - 18,
                         "Enter: Launch  |  ESC: Dismiss  |  Type to Search", UK_OVERLAY0);

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
        int dir_fd = sys_open(sdir, 0, 0);
        if (dir_fd >= 0) {
            char buf[2048];
            int nread;
            while ((nread = sys_getdents64(dir_fd, buf, sizeof(buf))) > 0) {
                int bpos = 0;
                while (bpos < nread) {
                    struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + bpos);
                    if (d->d_reclen == 0) { nread = 0; break; }
                    if (d->d_type == DT_REG) {
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
                                "tree", "cal", "watch", "netstat", "poweroff", "reboot", NULL
                            };
                            int is_cli = 0;
                            for (int k = 0; cli_list[k] != NULL; k++) {
                                if (strcmp(appname, cli_list[k]) == 0) {
                                    is_cli = 1;
                                    break;
                                }
                            }
                            if (is_cli) {
                                bpos += d->d_reclen;
                                continue;
                            }

                            /* Avoid duplicates if scanned across directories */
                            int already_added = 0;
                            for (int a = 0; a < g_num_apps; a++) {
                                if (strcmp(g_apps[a].name, appname) == 0) {
                                    already_added = 1;
                                    break;
                                }
                            }
                            if (already_added) {
                                bpos += d->d_reclen;
                                continue;
                            }

                            if (g_num_apps < MAX_APPS) {
                                app_entry_t *app = &g_apps[g_num_apps++];
                                strncpy(app->name, appname, sizeof(app->name) - 1);
                                app->name[sizeof(app->name) - 1] = '\0';

                                const char *sub = "Application";
                                if (strcmp(appname, "terminal") == 0) sub = "Command Terminal";
                                else if (strcmp(appname, "filemanager") == 0) sub = "Files & Storage";
                                else if (strcmp(appname, "texteditor") == 0) sub = "Code & Text Editor";
                                else if (strcmp(appname, "sysmon") == 0) sub = "Activity Monitor";
                                else if (strcmp(appname, "calculator") == 0) sub = "Math & Calculations";
                                else if (strcmp(appname, "clock") == 0) sub = "Clock & Timer";
                                else if (strcmp(appname, "settings") == 0) sub = "System Control";
                                else if (strcmp(appname, "about") == 0) sub = "System Info";
                                else if (strcmp(appname, "fetch") == 0) sub = "System Telemetry";
                                else if (strcmp(appname, "screenshot") == 0) sub = "Screen Capture";
                                else if (strcmp(appname, "paint") == 0) sub = "Paint & Sketch";

                                strncpy(app->subtitle, sub, sizeof(app->subtitle) - 1);
                                app->subtitle[sizeof(app->subtitle) - 1] = '\0';
                                
                                if (strcmp(sdir, "/") == 0) {
                                    snprintf(app->path, sizeof(app->path), "/%s", d->d_name);
                                } else {
                                    snprintf(app->path, sizeof(app->path), "%s/%s", sdir, d->d_name);
                                }
                                
                                /* Try to load icon from /usr/share/icons, /bin, or / */
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
                    bpos += d->d_reclen;
                }
            }
            sys_close(dir_fd);
        }
    }

    /* ── Get screen geometry ────────────────────────────────────────────── */
    az_fb_info_t fb;
    unsigned int sw = DEFAULT_W, sh = DEFAULT_H;
    if (az_fb_info(&fb) == 0) {
        if (fb.width  > 0) sw = fb.width;
        if (fb.height > 0) sh = fb.height;
    }

    /* Compute floating modal dimensions */
    unsigned int win_w = MODAL_W;
    unsigned int win_h = MODAL_H;
    if (win_w > sw - 40) win_w = sw - 40;
    if (win_h > sh - TASKBAR_H - 40) win_h = sh - TASKBAR_H - 40;

    int win_x = (int)(sw - win_w) / 2;
    int win_y = (int)(sh - win_h - TASKBAR_H - 12);
    if (win_y < 10) win_y = 10;

    /* Initial filter */
    update_filter();

    /* ── Create floating launcher window ────────────────────────────────── */
    int ret = uk_window_connect(&g_win,
                                "AzamiOS App Launcher",
                                win_x, win_y,
                                win_w, win_h,
                                LAUNCHER_MAP,
                                SERVER_CHAN);
    if (ret < 0) {
        de_log("[launcher] FATAL: window connect failed");
        return -1;
    }

    /* Keep on top */
    uk_set_zorder(&g_win, AZ_WM_ZORDER_TOP);

    draw_launcher();

    de_log("[launcher] Entering event loop.");

    /* ── Event loop ─────────────────────────────────────────────────────── */
    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) {
            de_log("[launcher] IPC channel disconnected, exiting loop.");
            break;
        }

        switch (msg.type) {

        case AZ_WM_MOUSE_EVENT: {
            int mx = (int)msg.mouse.abs_x;
            int my = (int)msg.mouse.abs_y;
            unsigned char lclick = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) != 0;

            int prev_hover = g_hovered;
            g_hovered = hit_cell(mx, my);

            if (g_hovered != prev_hover)
                draw_hover_update(prev_hover, g_hovered);

            if (lclick && g_hovered >= 0 && g_hovered < g_num_filtered && !g_launching) {
                g_launching = 1;
                int chosen_app = g_filtered_indices[g_hovered];
                de_log_fmt("[launcher] Launching app: ", g_apps[chosen_app].path);
                uk_launch_app(&g_win, g_apps[chosen_app].path);
                
                az_wm_msg_t destroy;
                memset(&destroy, 0, sizeof(destroy));
                destroy.type = AZ_WM_DESTROY_WINDOW;
                destroy.wid  = g_win.wid;
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&destroy);
                
                sys_exit(0);
            }
            break;
        }

        case AZ_WM_KEY_EVENT:
            if (msg.key.pressed && !g_launching) {
                /* ESC key closes launcher */
                if (msg.key.scancode == 1 || msg.key.keycode == 27) {
                    g_launching = 1;
                    az_wm_msg_t destroy;
                    memset(&destroy, 0, sizeof(destroy));
                    destroy.type = AZ_WM_DESTROY_WINDOW;
                    destroy.wid  = g_win.wid;
                    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&destroy);
                    sys_exit(0);
                } else if (msg.key.scancode == 28 || msg.key.keycode == '\n' || msg.key.keycode == 13) {
                    /* Enter launches selected/first matching app */
                    if (g_num_filtered > 0) {
                        int chosen_slot = (g_hovered >= 0 && g_hovered < g_num_filtered) ? g_hovered : 0;
                        int chosen_app = g_filtered_indices[chosen_slot];
                        g_launching = 1;
                        de_log_fmt("[launcher] Launching app: ", g_apps[chosen_app].path);
                        uk_launch_app(&g_win, g_apps[chosen_app].path);
                        az_wm_msg_t destroy;
                        memset(&destroy, 0, sizeof(destroy));
                        destroy.type = AZ_WM_DESTROY_WINDOW;
                        destroy.wid  = g_win.wid;
                        az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&destroy);
                        sys_exit(0);
                    }
                } else if (msg.key.keycode == 8 || msg.key.keycode == 127 || msg.key.scancode == 14) {
                    /* Backspace in search query */
                    if (g_search_len > 0) {
                        g_search_query[--g_search_len] = '\0';
                        update_filter();
                        draw_launcher();
                    }
                } else if (msg.key.scancode == 75) { /* Left arrow */
                    int prev = g_hovered;
                    if (g_hovered > 0) g_hovered--;
                    else g_hovered = g_num_filtered - 1;
                    draw_hover_update(prev, g_hovered);
                } else if (msg.key.scancode == 77) { /* Right arrow */
                    int prev = g_hovered;
                    if (g_hovered < g_num_filtered - 1) g_hovered++;
                    else g_hovered = 0;
                    draw_hover_update(prev, g_hovered);
                } else if (msg.key.keycode >= 32 && msg.key.keycode <= 126) {
                    /* Printable character appended to search query */
                    if (g_search_len < 32) {
                        g_search_query[g_search_len++] = (char)msg.key.keycode;
                        g_search_query[g_search_len] = '\0';
                        update_filter();
                        draw_launcher();
                    }
                }
            }
            break;

        case AZ_WM_DESTROY_WINDOW:
            sys_exit(0);
            break;

        case AZ_WM_FOCUS_CHANGE:
            if (!msg.focus.focused && !g_launching) {
                g_launching = 1;
                az_wm_msg_t destroy;
                memset(&destroy, 0, sizeof(destroy));
                destroy.type = AZ_WM_DESTROY_WINDOW;
                destroy.wid  = g_win.wid;
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&destroy);
                sys_exit(0);
            }
            break;

        default:
            break;
        }
    }

    sys_exit(0);
}
