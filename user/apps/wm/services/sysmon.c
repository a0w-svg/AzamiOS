#include "../wm.h"

static void sysmon_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)blink;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_WIN_BODY);
    int tx = bx + 16, ty = by + 12;
    draw_text(tx, ty, "AzamiOS System Monitor", COL_TITLE_ACT, COL_WIN_BODY);
    ty += 20;
    draw_rect(tx, ty, bw - 32, 1, COL_NOTE_LINE);
    ty += 10;
    draw_text(tx, ty, "GPU    : Bochs VBE (640x480 32BPP)", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "LFB    : 1.2 MB Identity Mapped", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "CPU    : 4-Core SMP Time-Sliced", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "Audio  : AC'97 Intel ICH (PCI)", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "Disk   : ATA Primary Master", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "FS     : FAT32 + USTAR initrd", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 20;
    char fb[32];
    itoa(frame_cnt, fb, 10);
    draw_text(tx, ty, "Frames : ", COL_TEXT_BLUE, COL_WIN_BODY);
    draw_text(tx + 72, ty, fb, COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    char tb[16];
    format_time_str(tb, t);
    draw_text(tx, ty, "Uptime : ", COL_TEXT_BLUE, COL_WIN_BODY);
    draw_text(tx + 72, ty, tb, COL_TEXT_DARK, COL_WIN_BODY);
}

void sysmon_service_init(void) {
    static const wm_service_t sysmon_srv = {
        WIN_SYSMON,
        "System Monitor",
        NULL,
        NULL,
        sysmon_render,
        NULL
    };
    wm_register_service(&sysmon_srv);
}
