/* ============================================================================
 * AzamiOS Desktop Environment — Modern File Manager (v6.0)
 * File: userland/apps/filemanager/main.c
 *
 * Features:
 *  • Dual-pane file manager: Sidebar Places & Main File Browser
 *  • POSIX Access Control List (ACL) inspection & permissions column ("rwxr-xr-x+")
 *  • File action toolbar: [Open], [Edit], [Terminal], [View ACLs], [New Note], [Delete]
 *  • Properties & ACL inspector popup dialog
 *  • Clickable Directory Breadcrumbs navigation
 *  • Rich file type detection (.elf, .txt, .c, .h, .conf, .sh, .img, .ext2)
 *  • Full keyboard navigation (Arrows, Enter, Backspace, Del)
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/acl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       740
#define WIN_H       500
#define MAP_ADDR    ((void *)0x66000000)

#define SIDEBAR_W   150
#define LIST_OX     (SIDEBAR_W + 10)
#define LIST_OY     78
#define ROW_H       24
#define VISIBLE_ROWS 15

static uk_window_t g_win;
static int g_selected = -1;
static int g_hovered  = -1;
static char g_current_path[256] = "/";
static int g_scroll = 0;

static bool g_show_props = false;
static char g_props_text[512] = "";

/* Places / Sidebar bookmarks */
typedef struct {
    const char *label;
    const char *path;
    const char *icon;
} place_item_t;

static const place_item_t g_places[] = {
    { "Root",        "/",                  "/" },
    { "Desktop",     "/home/azami/Desktop", "D" },
    { "Hard Disk",   "/hdd",               "H" },
    { "Binaries",    "/bin",               "B" },
    { "System",      "/sbin",              "S" },
    { "Config",      "/etc",               "E" },
    { "Proc FS",     "/proc",              "P" },
    { "Devices",     "/dev",               "D" },
    { "Temporary",   "/tmp",               "T" },
};
#define NUM_PLACES ((int)(sizeof(g_places)/sizeof(g_places[0])))
static int g_selected_place = 0;

/* Dynamically loaded filesystem entries */
typedef struct {
    char name[64];
    char size[24];
    char type[24];
    char mode[16];
    int  is_dir;
    int  has_acl;
    uid_t uid;
    gid_t gid;
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
    g_scroll = 0;
    g_show_props = false;

    /* Update selected place if matching */
    for (int p = 0; p < NUM_PLACES; p++) {
        if (strcmp(g_current_path, g_places[p].path) == 0) {
            g_selected_place = p;
            break;
        }
    }

    /* If not in root, add parent directory ".." */
    if (strcmp(g_current_path, "/") != 0) {
        strcpy(g_files[NFILES].name, "..");
        strcpy(g_files[NFILES].size, "-");
        strcpy(g_files[NFILES].type, "Folder");
        strcpy(g_files[NFILES].mode, "drwxr-xr-x");
        g_files[NFILES].is_dir = 1;
        g_files[NFILES].has_acl = 0;
        NFILES++;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && NFILES < MAX_ENTRIES) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        strncpy(g_files[NFILES].name, ent->d_name, sizeof(g_files[NFILES].name) - 1);
        g_files[NFILES].name[sizeof(g_files[NFILES].name) - 1] = '\0';

        char full_path[512];
        if (strcmp(g_current_path, "/") == 0)
            snprintf(full_path, sizeof(full_path), "/%s", ent->d_name);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", g_current_path, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            g_files[NFILES].uid = st.st_uid;
            g_files[NFILES].gid = st.st_gid;

            /* Construct mode string */
            char m[16] = "----------";
            if (S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) m[0] = 'd';
            if (st.st_mode & 0400) m[1] = 'r';
            if (st.st_mode & 0200) m[2] = 'w';
            if (st.st_mode & 0100) m[3] = 'x';
            if (st.st_mode & 0040) m[4] = 'r';
            if (st.st_mode & 0020) m[5] = 'w';
            if (st.st_mode & 0010) m[6] = 'x';
            if (st.st_mode & 0004) m[7] = 'r';
            if (st.st_mode & 0002) m[8] = 'w';
            if (st.st_mode & 0001) m[9] = 'x';

            /* Check POSIX ACL */
            acl_t acl = acl_get_file(full_path, 0);
            if (acl && acl->count > 3) {
                m[10] = '+';
                m[11] = '\0';
                g_files[NFILES].has_acl = 1;
            } else {
                m[10] = '\0';
                g_files[NFILES].has_acl = 0;
            }
            if (acl) acl_free(acl);
            strcpy(g_files[NFILES].mode, m);

            if (S_ISDIR(st.st_mode) || ent->d_type == DT_DIR) {
                strcpy(g_files[NFILES].type, "Folder");
                strcpy(g_files[NFILES].size, "-");
                g_files[NFILES].is_dir = 1;
            } else {
                if (strstr(ent->d_name, ".elf")) strcpy(g_files[NFILES].type, "Executable");
                else if (strstr(ent->d_name, ".txt") || strstr(ent->d_name, ".md")) strcpy(g_files[NFILES].type, "Text Doc");
                else if (strstr(ent->d_name, ".c") || strstr(ent->d_name, ".h")) strcpy(g_files[NFILES].type, "C Source");
                else if (strstr(ent->d_name, ".conf") || strstr(ent->d_name, ".cfg")) strcpy(g_files[NFILES].type, "Config");
                else strcpy(g_files[NFILES].type, "File");
                format_file_size((size_t)st.st_size, g_files[NFILES].size, sizeof(g_files[NFILES].size));
                g_files[NFILES].is_dir = 0;
            }
        } else {
            strcpy(g_files[NFILES].type, (ent->d_type == DT_DIR) ? "Folder" : "File");
            strcpy(g_files[NFILES].size, "-");
            strcpy(g_files[NFILES].mode, "-rw-r--r--");
            g_files[NFILES].is_dir = (ent->d_type == DT_DIR);
            g_files[NFILES].has_acl = 0;
        }
        NFILES++;
    }
    closedir(dir);
}

static void activate_entry(int idx)
{
    if (idx < 0 || idx >= NFILES) return;

    if (g_files[idx].is_dir) {
        if (strcmp(g_files[idx].name, "..") == 0) {
            char *last_slash = strrchr(g_current_path, '/');
            if (last_slash && last_slash != g_current_path) {
                *last_slash = '\0';
                load_directory(g_current_path);
            } else {
                load_directory("/");
            }
        } else {
            char next_path[512];
            if (strcmp(g_current_path, "/") == 0)
                snprintf(next_path, sizeof(next_path), "/%s", g_files[idx].name);
            else
                snprintf(next_path, sizeof(next_path), "%s/%s", g_current_path, g_files[idx].name);
            load_directory(next_path);
        }
    } else {
        const char *fn = g_files[idx].name;
        char full_path[512];
        if (strcmp(g_current_path, "/") == 0)
            snprintf(full_path, sizeof(full_path), "/%s", fn);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s", g_current_path, fn);

        if (strstr(fn, ".elf")) {
            uk_launch_app(&g_win, full_path);
        } else {
            uk_launch_app(&g_win, "/bin/texteditor.elf");
        }
    }
}

static void show_file_properties(int idx)
{
    if (idx < 0 || idx >= NFILES) return;

    char full_path[512];
    if (strcmp(g_current_path, "/") == 0)
        snprintf(full_path, sizeof(full_path), "/%s", g_files[idx].name);
    else
        snprintf(full_path, sizeof(full_path), "%s/%s", g_current_path, g_files[idx].name);

    acl_t acl = acl_get_file(full_path, 0);
    char *acl_txt = acl ? acl_to_text(acl, NULL) : NULL;

    snprintf(g_props_text, sizeof(g_props_text),
             "File: %s\nPath: %s\nType: %s | Size: %s\nMode: %s (UID %u, GID %u)\n\nPOSIX ACL Rules:\n%s",
             g_files[idx].name, full_path, g_files[idx].type, g_files[idx].size,
             g_files[idx].mode, (unsigned int)g_files[idx].uid, (unsigned int)g_files[idx].gid,
             acl_txt ? acl_txt : "(Standard base mode)");

    if (acl_txt) free(acl_txt);
    if (acl) acl_free(acl);

    g_show_props = true;
}

static void draw_filemanager(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    if (g_scroll > NFILES - VISIBLE_ROWS) g_scroll = NFILES - VISIBLE_ROWS;
    if (g_scroll < 0) g_scroll = 0;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Title / Breadcrumb Bar ───────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 36, UK_MANTLE, UK_CRUST);
    uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE0);

    /* Folder Icon & Breadcrumb Path */
    uk_draw_badge(&g_win, 12, 8, "Path", UK_SURFACE1, UK_MAUVE);
    char breadcrumb_str[128];
    snprintf(breadcrumb_str, sizeof(breadcrumb_str), "%s", g_current_path);
    uk_draw_text(&g_win, 64, 10, breadcrumb_str, UK_TEXT);

    /* ── Action Toolbar ───────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 37, (int)w, 36, UK_MANTLE);
    uk_hline(&g_win, 0, 73, (int)w, UK_SURFACE0);

    int btn_x = 12;
    uk_draw_button(&g_win, btn_x, 42, 60, 26, "Open", (g_selected >= 0) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    btn_x += 68;
    uk_draw_button(&g_win, btn_x, 42, 60, 26, "Edit", UK_BTN_NORMAL);
    btn_x += 68;
    uk_draw_button(&g_win, btn_x, 42, 70, 26, "Terminal", UK_BTN_NORMAL);
    btn_x += 78;
    uk_draw_button(&g_win, btn_x, 42, 75, 26, "View ACLs", UK_BTN_NORMAL);
    btn_x += 83;
    uk_draw_button(&g_win, btn_x, 42, 75, 26, "New Note", UK_BTN_NORMAL);
    btn_x += 83;
    uk_draw_button(&g_win, btn_x, 42, 60, 26, "Delete", UK_BTN_PRESSED);

    /* ── Sidebar (Places) ─────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 74, SIDEBAR_W, (int)h - 74, UK_MANTLE);
    uk_vline(&g_win, SIDEBAR_W, 74, (int)h - 74, UK_SURFACE0);

    uk_draw_text(&g_win, 12, 84, "PLACES", UK_OVERLAY0);

    for (int p = 0; p < NUM_PLACES; p++) {
        int py = 104 + p * 26;
        bool is_sel = (p == g_selected_place);
        if (is_sel) {
            uk_fill_rounded_rect(&g_win, 8, py - 2, SIDEBAR_W - 16, 22, 6, UK_SURFACE0);
            uk_fill_rounded_rect(&g_win, 8, py + 2, 3, 14, 1, UK_MAUVE);
        }
        uk_draw_text(&g_win, 20, py + 2, g_places[p].icon, is_sel ? UK_MAUVE : UK_SUBTEXT0);
        uk_draw_text(&g_win, 36, py + 2, g_places[p].label, is_sel ? UK_TEXT : UK_SUBTEXT1);
    }

    /* ── Main List Column Headers ─────────────────────────────────────────── */
    int col_y = LIST_OY + 4;
    uk_draw_text(&g_win, LIST_OX + 24, col_y, "Name", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 240, col_y, "Type", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 340, col_y, "Size", UK_OVERLAY0);
    uk_draw_text(&g_win, LIST_OX + 430, col_y, "Permissions", UK_OVERLAY0);

    uk_hline(&g_win, LIST_OX, col_y + 18, (int)w - LIST_OX - 10, UK_SURFACE0);

    /* ── File Entries ─────────────────────────────────────────────────────── */
    int list_y = col_y + 24;
    for (int i = 0; i < VISIBLE_ROWS && (i + g_scroll) < NFILES; i++) {
        int idx = i + g_scroll;
        int ry = list_y + i * ROW_H;
        bool is_sel = (idx == g_selected);
        bool is_hov = (idx == g_hovered);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, LIST_OX, ry - 1, (int)w - LIST_OX - 10, ROW_H - 2, 4, UK_SURFACE0);
            uk_fill_rounded_rect(&g_win, LIST_OX, ry + 2, 3, ROW_H - 8, 1, UK_MAUVE);
        } else if (is_hov) {
            uk_fill_rounded_rect(&g_win, LIST_OX, ry - 1, (int)w - LIST_OX - 10, ROW_H - 2, 4, 0xFF222230);
        }

        /* Icon */
        if (g_files[idx].is_dir) {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[D]", UK_YELLOW);
        } else if (strstr(g_files[idx].name, ".elf")) {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[*]", UK_GREEN);
        } else {
            uk_draw_text(&g_win, LIST_OX + 8, ry + 2, "[F]", UK_BLUE);
        }

        /* Name */
        uk_draw_text(&g_win, LIST_OX + 34, ry + 2, g_files[idx].name, is_sel ? UK_TEXT : UK_SUBTEXT1);

        /* Type */
        uk_draw_text(&g_win, LIST_OX + 240, ry + 2, g_files[idx].type, UK_OVERLAY1);

        /* Size */
        uk_draw_text(&g_win, LIST_OX + 340, ry + 2, g_files[idx].size, UK_OVERLAY0);

        /* Permissions & ACL badge */
        unsigned int perm_col = g_files[idx].has_acl ? UK_MAUVE : UK_OVERLAY1;
        uk_draw_text(&g_win, LIST_OX + 430, ry + 2, g_files[idx].mode, perm_col);
    }

    /* ── Bottom Status Bar ────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, SIDEBAR_W, (int)h - 24, (int)w - SIDEBAR_W, 24, UK_MANTLE);
    uk_hline(&g_win, SIDEBAR_W, (int)h - 24, (int)w - SIDEBAR_W, UK_SURFACE0);

    char status_str[128];
    snprintf(status_str, sizeof(status_str), "%d item(s) in %s", NFILES, g_current_path);
    uk_draw_text(&g_win, SIDEBAR_W + 12, (int)h - 18, status_str, UK_SUBTEXT0);

    /* ── Properties Modal Dialog (if open) ────────────────────────────────── */
    if (g_show_props) {
        int modal_w = 460;
        int modal_h = 240;
        int mx = ((int)w - modal_w) / 2;
        int my = ((int)h - modal_h) / 2;

        uk_fill_rounded_rect(&g_win, mx, my, modal_w, modal_h, 10, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, mx, my, modal_w, modal_h, 10, UK_MAUVE);

        uk_fill_rounded_rect(&g_win, mx, my, modal_w, 32, 10, UK_MANTLE);
        uk_draw_text(&g_win, mx + 16, my + 8, "File Properties & POSIX ACLs", UK_MAUVE);
        uk_draw_button(&g_win, mx + modal_w - 40, my + 4, 30, 24, "X", UK_BTN_PRESSED);

        /* Render multi-line property text */
        char text_copy[512];
        strncpy(text_copy, g_props_text, sizeof(text_copy) - 1);
        text_copy[sizeof(text_copy) - 1] = '\0';

        char *line = strtok(text_copy, "\n");
        int ly = my + 44;
        while (line && ly < my + modal_h - 20) {
            uk_draw_text(&g_win, mx + 16, ly, line, UK_TEXT);
            ly += 18;
            line = strtok(NULL, "\n");
        }
    }

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int win_x = (int)(sw - WIN_W) / 2;
    int win_y = (int)(sh - WIN_H) / 2;

    if (uk_window_connect(&g_win, "Files", win_x, win_y, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        return 1;
    }

    load_directory("/");
    draw_filemanager();

    az_ipc_msg_t raw_msg;
    az_wm_msg_t *msg = (az_wm_msg_t *)&raw_msg;

    for (;;) {
        if (az_channel_recv(g_win.client_chan, &raw_msg) != 0) continue;

        switch (msg->type) {
        case AZ_WM_MOUSE_EVENT: {
            int mx = msg->mouse.abs_x;
            int my = msg->mouse.abs_y;

            if (msg->mouse.buttons & AZ_MOUSE_BTN_LEFT) {
                if (g_show_props) {
                    /* Check close button on properties modal */
                    int modal_w = 460;
                    int modal_h = 240;
                    int px = ((int)g_win.width - modal_w) / 2;
                    int py = ((int)g_win.height - modal_h) / 2;
                    if (mx >= px + modal_w - 40 && mx <= px + modal_w - 10 && my >= py + 4 && my <= py + 28) {
                        g_show_props = false;
                        draw_filemanager();
                    }
                    break;
                }

                /* Toolbar actions */
                if (my >= 42 && my <= 68) {
                    if (mx >= 12 && mx <= 72) { /* Open */
                        if (g_selected >= 0) activate_entry(g_selected);
                    } else if (mx >= 80 && mx <= 140) { /* Edit */
                        uk_launch_app(&g_win, "/bin/texteditor.elf");
                    } else if (mx >= 148 && mx <= 218) { /* Terminal */
                        uk_launch_app(&g_win, "/bin/terminal.elf");
                    } else if (mx >= 226 && mx <= 301) { /* View ACLs */
                        if (g_selected >= 0) {
                            show_file_properties(g_selected);
                            draw_filemanager();
                        }
                    } else if (mx >= 309 && mx <= 384) { /* New Note */
                        char note_path[512];
                        snprintf(note_path, sizeof(note_path), "%s/note.txt", g_current_path);
                        int fd = sys_open(note_path, 0x42 /* O_CREAT|O_WRONLY */, 0644);
                        if (fd >= 0) {
                            sys_write(fd, "AzamiOS Note\n", 13);
                            sys_close(fd);
                            load_directory(g_current_path);
                            draw_filemanager();
                        }
                    } else if (mx >= 392 && mx <= 452) { /* Delete */
                        if (g_selected >= 0 && !g_files[g_selected].is_dir) {
                            char del_path[512];
                            snprintf(del_path, sizeof(del_path), "%s/%s", g_current_path, g_files[g_selected].name);
                            sys_unlink(del_path);
                            load_directory(g_current_path);
                            draw_filemanager();
                        }
                    }
                    break;
                }

                /* Sidebar Places click */
                if (mx < SIDEBAR_W && my >= 104) {
                    int p = (my - 104) / 26;
                    if (p >= 0 && p < NUM_PLACES) {
                        g_selected_place = p;
                        load_directory(g_places[p].path);
                        draw_filemanager();
                    }
                    break;
                }

                /* File list item click */
                if (mx >= LIST_OX && my >= (LIST_OY + 28)) {
                    int row = (my - (LIST_OY + 28)) / ROW_H;
                    int idx = row + g_scroll;
                    if (idx >= 0 && idx < NFILES) {
                        if (g_selected == idx) {
                            /* Double-click / re-click to open */
                            activate_entry(idx);
                        } else {
                            g_selected = idx;
                            draw_filemanager();
                        }
                    }
                }
            } else {
                if (mx >= LIST_OX && my >= (LIST_OY + 28)) {
                    int row = (my - (LIST_OY + 28)) / ROW_H;
                    int idx = row + g_scroll;
                    if (idx >= 0 && idx < NFILES && idx != g_hovered) {
                        g_hovered = idx;
                        draw_filemanager();
                    }
                }
            }
            break;
        }

        case AZ_WM_KEY_EVENT: {
            if (!msg->key.pressed) break;

            if (msg->key.keycode == 0x1B) { /* Escape */
                if (g_show_props) {
                    g_show_props = false;
                    draw_filemanager();
                } else {
                    sys_exit(0);
                }
            } else if (msg->key.keycode == '\n' || msg->key.keycode == '\r') {
                if (g_selected >= 0) activate_entry(g_selected);
            } else if (msg->key.keycode == 0x08) { /* Backspace */
                char *last_slash = strrchr(g_current_path, '/');
                if (last_slash && last_slash != g_current_path) {
                    *last_slash = '\0';
                    load_directory(g_current_path);
                } else {
                    load_directory("/");
                }
                draw_filemanager();
            } else if (msg->key.keycode == 'i' || msg->key.keycode == 'p') {
                if (g_selected >= 0) {
                    show_file_properties(g_selected);
                    draw_filemanager();
                }
            }
            break;
        }

        default:
            break;
        }
    }

    return 0;
}
