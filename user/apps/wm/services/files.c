#include "../wm.h"

static void files_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_WIN_BODY);
    int tx = bx + 12, ty = by + 8;
    /* Toolbar */
    draw_rect(bx, by, bw, 20, COL_WIN_FRAME);
    draw_text(tx, ty + 2, "Path: /  (initrd + FAT32)", COL_TEXT_GRAY, COL_WIN_FRAME);
    ty = by + 26;
    draw_rect(tx - 4, ty - 2, bw - 16, 1, COL_NOTE_LINE);
    ty += 6;
    /* File entries with icons */
    /* initrd entries */
    draw_rect(tx, ty, 14, 10, COL_ICON_BLUE);
    draw_text(tx + 2, ty + 1, "B", COL_TEXT_WHITE, COL_ICON_BLUE);
    draw_text(tx + 20, ty + 1, "wm       (initrd)", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 18;
    draw_rect(tx, ty, 14, 10, COL_ICON_BLUE);
    draw_text(tx + 2, ty + 1, "B", COL_TEXT_WHITE, COL_ICON_BLUE);
    draw_text(tx + 20, ty + 1, "shell    (initrd)", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 18;
    draw_rect(bx + 12, ty, bw - 24, 1, COL_NOTE_LINE);
    ty += 8;
    /* FAT32 entries */
    draw_rect(tx, ty, 14, 10, COL_ICON_GREEN);
    draw_text(tx + 2, ty + 1, "F", COL_TEXT_WHITE, COL_ICON_GREEN);
    draw_text(tx + 20, ty + 1, "README.TXT  (FAT32)", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 24;
    draw_rect(bx + 12, ty, bw - 24, 1, COL_NOTE_LINE);
    ty += 10;
    /* Stats */
    draw_text(tx, ty, "3 items  |  2 volumes mounted", COL_TEXT_GRAY, COL_WIN_BODY);
}

void files_service_init(void) {
    static const wm_service_t files_srv = {
        WIN_FILES,
        "File Manager",
        NULL,
        NULL,
        files_render,
        NULL
    };
    wm_register_service(&files_srv);
}
