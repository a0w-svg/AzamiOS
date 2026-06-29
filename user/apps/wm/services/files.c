/**
 * files.c — AzamiOS File Manager Service
 *
 * EDUCATIONAL ARCHITECTURE & SECURITY EXPLANATIONS:
 * 1. Read-Only Display Isolation:
 *    The graphical file browser displays volume mappings without direct mutator callbacks.
 *    By separating browsing rendering from low-level filesystem drivers (`sys_open`, etc.),
 *    the service prevents UI exploits from arbitrarily modifying root storage sectors.
 * 2. Static Compositing:
 *    Registers with `WM_SRV_FLAG_NONE`, rendering UI components only when brought into focus.
 */

#include "../wm.h"

static char g_dir_entries[12][32];
static int g_dir_count = -1;

static void files_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;

    if (g_dir_count < 0) {
        g_dir_count = 0;
        DIR *d = opendir("/");
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d)) != NULL && g_dir_count < 10) {
                strncpy(g_dir_entries[g_dir_count], ent->d_name, 31);
                g_dir_entries[g_dir_count][31] = '\0';
                g_dir_count++;
            }
            closedir(d);
        }
    }

    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_WIN_BODY);
    int tx = bx + 12, ty = by + 8;
    /* Toolbar */
    draw_rect(bx, by, bw, 20, COL_WIN_FRAME);
    draw_text(tx, ty + 2, "Path: /  (FHS Root Virtual Filesystem)", COL_TEXT_GRAY, COL_WIN_FRAME);
    ty = by + 26;
    draw_rect(tx - 4, ty - 2, bw - 16, 1, COL_NOTE_LINE);
    ty += 6;

    for (int i = 0; i < g_dir_count; i++) {
        draw_rect(tx, ty, 14, 10, COL_ICON_BLUE);
        draw_text(tx + 2, ty + 1, "D", COL_TEXT_WHITE, COL_ICON_BLUE);
        draw_text(tx + 20, ty + 1, g_dir_entries[i], COL_TEXT_DARK, COL_WIN_BODY);
        ty += 16;
    }

    ty += 4;
    draw_rect(bx + 12, ty, bw - 24, 1, COL_NOTE_LINE);
    ty += 8;
    char stat_buf[64];
    sprintf(stat_buf, "%d system directories active in VFS", g_dir_count);
    draw_text(tx, ty, stat_buf, COL_TEXT_GRAY, COL_WIN_BODY);
}

void files_service_init(void) {
    static const wm_service_t files_srv = {
        WIN_FILES,
        "File Manager",
        WM_SRV_FLAG_NONE,
        NULL,
        NULL,
        NULL,
        files_render,
        NULL
    };
    wm_register_service(&files_srv);
}
