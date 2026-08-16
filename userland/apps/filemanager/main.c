/* ============================================================================
 * AzamiOS Desktop Environment — File Manager
 * File: userland/apps/filemanager/main.c
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/syscall.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       580
#define WIN_H       420
#define MAP_ADDR    ((void *)0x66000000)

static uk_window_t g_win;
static int g_selected = -1;
static int g_hovered  = -1;
static char g_current_path[256] = "/";

/* Dynamically loaded filesystem entries */
typedef struct {
    char name[64];
    char size[24];
    char type[24];
    int  is_dir;
} file_entry_t;

#define MAX_ENTRIES 128
static file_entry_t g_files[MAX_ENTRIES];
static int NFILES = 0;

static void format_file_size(size_t size, char *out, size_t out_len)
{
    if (size >= 1024 * 1024) {
        snprintf(out, out_len, "%u.%u MB", (unsigned int)(size / (1024 * 1024)), (unsigned int)((size % (1024 * 1024)) / 100000));
    } else if (size >= 1024) {
        snprintf(out, out_len, "%u.%u KB", (unsigned int)(size / 1024), (unsigned int)((size % 1024) / 100));
    } else {
        snprintf(out, out_len, "%u B", (unsigned int)size);
    }
}

static void load_directory(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) return;

    strncpy(g_current_path, path, sizeof(g_current_path) - 1);
    g_current_path[sizeof(g_current_path) - 1] = '\0';

    NFILES = 0;
    g_selected = -1;
    g_hovered = -1;

    /* If not in root, add parent directory ".." */
    if (strcmp(g_current_path, "/") != 0) {
        strcpy(g_files[NFILES].name, "..");
        strcpy(g_files[NFILES].size, "-");
        strcpy(g_files[NFILES].type, "Parent Directory");
        g_files[NFILES].is_dir = 1;
        NFILES++;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && NFILES < MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        strncpy(g_files[NFILES].name, ent->d_name, sizeof(g_files[NFILES].name) - 1);
        g_files[NFILES].name[sizeof(g_files[NFILES].name) - 1] = '\0';

        char full_path[512];
        if (strcmp(g_current_path, "/") == 0) {
            snprintf(full_path, sizeof(full_path), "/%s", ent->d_name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%s", g_current_path, ent->d_name);
        }

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) {
                strcpy(g_files[NFILES].type, "Directory");
                strcpy(g_files[NFILES].size, "-");
                g_files[NFILES].is_dir = 1;
            } else {
                strcpy(g_files[NFILES].type, "File");
                format_file_size((size_t)st.st_size, g_files[NFILES].size, sizeof(g_files[NFILES].size));
                g_files[NFILES].is_dir = 0;
            }
        } else {
            if (ent->d_type == DT_DIR) {
                strcpy(g_files[NFILES].type, "Directory");
                strcpy(g_files[NFILES].size, "-");
                g_files[NFILES].is_dir = 1;
            } else {
                strcpy(g_files[NFILES].type, "File");
                strcpy(g_files[NFILES].size, "-");
                g_files[NFILES].is_dir = 0;
            }
        }
        NFILES++;
    }
    closedir(dir);
}

#define ROW_H   24
#define LIST_OY 84
#define LIST_OX  8
#define VISIBLE_ROWS  13

static int g_scroll = 0;

static void draw_filemanager(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    if (g_scroll > NFILES - VISIBLE_ROWS) g_scroll = NFILES - VISIBLE_ROWS;
    if (g_scroll < 0) g_scroll = 0;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Toolbar ────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_YELLOW);
    uk_draw_text(&g_win, 16, 6,  "File Manager", UK_TEXT);

    char path_label[280];
    snprintf(path_label, sizeof(path_label), "Location: %s", g_current_path);
    uk_draw_text(&g_win, 16, 24, path_label, UK_OVERLAY0);
    uk_hline(&g_win, 0, 44, (int)w, UK_SURFACE1);

    /* ── Column headers ─────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 44, (int)w, 36, UK_SURFACE0);
    uk_draw_text(&g_win, LIST_OX + 24, 52, "Name",       UK_SUBTEXT0);
    uk_draw_text(&g_win, LIST_OX + 280, 52, "Size",      UK_SUBTEXT0);
    uk_draw_text(&g_win, LIST_OX + 380, 52, "Type",      UK_SUBTEXT0);
    uk_hline(&g_win, 0, 80, (int)w, UK_SURFACE1);

    /* Vertical dividers */
    uk_vline(&g_win, LIST_OX + 272, 44, 36, UK_SURFACE1);
    uk_vline(&g_win, LIST_OX + 372, 44, 36, UK_SURFACE1);

    /* ── File list ──────────────────────────────────────────────────────── */
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int idx = i + g_scroll;
        if (idx >= NFILES) break;

        int ry = LIST_OY + i * ROW_H;

        /* Row background */
        unsigned int row_bg = UK_BASE;
        if (idx == g_selected) row_bg = UK_SURFACE1;
        else if (idx == g_hovered) row_bg = UK_SURFACE0;
        uk_fill_rect(&g_win, 0, ry, (int)w, ROW_H, row_bg);

        /* Accent bar for selected */
        if (idx == g_selected)
            uk_fill_rect(&g_win, 0, ry, 3, ROW_H, UK_YELLOW);

        /* Icon: Blue folder for directories, Yellow for files */
        unsigned int icon_col = g_files[idx].is_dir ? UK_BLUE : ((idx == g_selected) ? UK_YELLOW : UK_OVERLAY1);
        uk_fill_rounded_rect(&g_win, LIST_OX + 4, ry + 4, 14, 16, 1, icon_col);
        uk_put_pixel(&g_win, LIST_OX + 14, ry + 4, row_bg);
        uk_put_pixel(&g_win, LIST_OX + 15, ry + 5, row_bg);
        uk_put_pixel(&g_win, LIST_OX + 15, ry + 4, UK_SURFACE2);

        unsigned int fg = (idx == g_selected) ? UK_TEXT : (g_files[idx].is_dir ? UK_BLUE : UK_SUBTEXT1);
        uk_draw_text_clip(&g_win, LIST_OX + 24, ry + 4,
                          g_files[idx].name, fg, 240);
        uk_draw_text_clip(&g_win, LIST_OX + 280, ry + 4,
                          g_files[idx].size, UK_OVERLAY1, 88);
        uk_draw_text_clip(&g_win, LIST_OX + 380, ry + 4,
                          g_files[idx].type, UK_OVERLAY1, (int)w - LIST_OX - 384);

        /* Row separator */
        uk_hline(&g_win, 0, ry + ROW_H - 1, (int)w, UK_SURFACE0);
    }

    /* ── Scrollbar ──────────────────────────────────────────────────────── */
    if (NFILES > VISIBLE_ROWS) {
        int sb_x = (int)w - 10;
        int sb_h = (int)h - LIST_OY - 28;
        int thumb_h = sb_h * VISIBLE_ROWS / NFILES;
        int thumb_pos = g_scroll * (sb_h - thumb_h) / (NFILES - VISIBLE_ROWS);
        uk_draw_scrollbar(&g_win, sb_x, LIST_OY, sb_h, thumb_pos, thumb_h);
    }

    /* ── Status bar ─────────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, (int)h - 24, (int)w, 24, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 24, (int)w, UK_SURFACE1);

    if (g_selected >= 0 && g_selected < NFILES) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Selected: %s (%s, %s)",
                 g_files[g_selected].name, g_files[g_selected].type, g_files[g_selected].size);
        uk_draw_text(&g_win, 8, (int)h - 16, msg, UK_TEXT);
    } else {
        char cnt[64];
        snprintf(cnt, sizeof(cnt), "%d items", NFILES);
        uk_draw_text(&g_win, 8, (int)h - 16, cnt, UK_OVERLAY0);
    }

    uk_invalidate(&g_win);
}

static void activate_entry(int idx)
{
    if (idx < 0 || idx >= NFILES) return;

    if (g_files[idx].is_dir) {
        if (strcmp(g_files[idx].name, "..") == 0) {
            /* Navigate to parent */
            char *last_slash = strrchr(g_current_path, '/');
            if (last_slash && last_slash != g_current_path) {
                *last_slash = '\0';
                load_directory(g_current_path);
            } else {
                load_directory("/");
            }
        } else {
            /* Navigate into subfolder */
            char next_path[512];
            if (strcmp(g_current_path, "/") == 0) {
                snprintf(next_path, sizeof(next_path), "/%s", g_files[idx].name);
            } else {
                snprintf(next_path, sizeof(next_path), "%s/%s", g_current_path, g_files[idx].name);
            }
            load_directory(next_path);
        }
        draw_filemanager();
    } else {
        const char *fn = g_files[idx].name;
        if (strstr(fn, ".elf")) {
            char app_path[512];
            if (strcmp(g_current_path, "/") == 0) {
                snprintf(app_path, sizeof(app_path), "/%s", fn);
            } else {
                snprintf(app_path, sizeof(app_path), "%s/%s", g_current_path, fn);
            }
            uk_launch_app(&g_win, app_path);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[filemanager] Starting v3.1 File Manager...");

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "File Manager",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { de_log("[filemanager] FATAL"); return -1; }

    load_directory("/");
    draw_filemanager();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                if (msg.key.scancode == 72 || msg.key.keycode == 'k') { /* Up */
                    if (g_selected > 0) {
                        g_selected--;
                        if (g_selected < g_scroll) g_scroll = g_selected;
                        draw_filemanager();
                    }
                } else if (msg.key.scancode == 80 || msg.key.keycode == 'j') { /* Down */
                    if (g_selected < NFILES - 1) {
                        g_selected++;
                        if (g_selected >= g_scroll + VISIBLE_ROWS) g_scroll = g_selected - VISIBLE_ROWS + 1;
                        draw_filemanager();
                    }
                } else if (msg.key.scancode == 28 || msg.key.keycode == '\n' || msg.key.keycode == 13) { /* Enter */
                    activate_entry(g_selected);
                } else if (msg.key.scancode == 14 || msg.key.keycode == '\b') { /* Backspace: go up */
                    char *last_slash = strrchr(g_current_path, '/');
                    if (last_slash && last_slash != g_current_path) {
                        *last_slash = '\0';
                        load_directory(g_current_path);
                    } else {
                        load_directory("/");
                    }
                    draw_filemanager();
                }
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = (int)msg.mouse.abs_x;
            int my = (int)msg.mouse.abs_y;
            static int was_clicked = 0;
            int is_clicked = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) != 0;

            /* Hit test rows */
            int prev_hover = g_hovered;
            g_hovered = -1;
            if (mx >= 0 && mx < (int)g_win.width && my >= LIST_OY) {
                int idx = (my - LIST_OY) / ROW_H + g_scroll;
                if (idx >= 0 && idx < NFILES) g_hovered = idx;
            }

            if (g_hovered != prev_hover) draw_filemanager();

            if (is_clicked && !was_clicked && g_hovered >= 0) {
                if (g_selected == g_hovered) {
                    /* Double-click: activate entry */
                    activate_entry(g_hovered);
                } else {
                    g_selected = g_hovered;
                    draw_filemanager();
                }
            }
            was_clicked = is_clicked;
        }
    }
    sys_exit(0);
}
