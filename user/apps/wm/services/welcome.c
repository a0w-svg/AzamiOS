/**
 * welcome.c — AzamiOS Welcome & About Window Services
 *
 * EDUCATIONAL ARCHITECTURE & MODULARITY EXPLANATIONS:
 * 1. Static Service Encapsulation:
 *    The Welcome and About dialogs operate as passive, static display services.
 *    They declare `WM_SRV_FLAG_NONE` because they do not require continuous CPU cycles
 *    or frame rendering animations when idle, preserving system battery and CPU time.
 * 2. Safe Window Rendering Boundaries:
 *    Render coordinates derive strictly from clamped parent window dimensions (`w->w`, `w->h`),
 *    preventing UI overflow regardless of user dragging or resizing actions.
 */

#include "../wm.h"

static void welcome_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_WIN_BODY);
    int tx = bx + 20, ty = by + 16;
    draw_text(tx, ty, "Welcome to AzamiOS!", COL_TITLE_ACT, COL_WIN_BODY);
    ty += 24;
    draw_text(tx, ty, "Desktop Environment v3.0", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 20;
    draw_rect(tx, ty, bw - 40, 1, COL_NOTE_LINE);
    ty += 12;
    draw_text(tx, ty, "Display  : 640x480 True Color (32 BPP)", COL_TEXT_BLUE, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "GPU      : Bochs VBE Linear Framebuffer", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "Renderer : 60+ FPS REP MOVSD DMA Copy", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "CPU      : 4-Core SMP Preemptive Sched", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "Audio    : Intel AC'97 PCM DMA Driver", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 14;
    draw_text(tx, ty, "Storage  : ATA PIO + FAT32 + USTAR", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 20;
    draw_text(tx, ty, "Right-click desktop for context menu", COL_TEXT_GRAY, COL_WIN_BODY);
    ty += 12;
    draw_text(tx, ty, "Use START menu or click desktop icons", COL_TEXT_GRAY, COL_WIN_BODY);
}

static void about_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    draw_rect(bx, by, bw, bh, COL_WIN_BODY);
    int tx = bx + 20, ty = by + 12;
    /* Logo area */
    draw_rect(tx, ty, 40, 40, COL_TITLE_ACT);
    draw_rect(tx + 4, ty + 4, 32, 32, COL_WIN_BODY);
    draw_text(tx + 8, ty + 12, "Aza", COL_TITLE_ACT, COL_WIN_BODY);
    draw_text(tx + 8, ty + 22, " mi", COL_TEXT_BLUE, COL_WIN_BODY);
    /* Info text */
    int ix = tx + 50;
    draw_text(ix, ty + 4, "AzamiOS", COL_TITLE_ACT, COL_WIN_BODY);
    draw_text(ix, ty + 16, "Desktop Environment v3.0", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(ix, ty + 28, "i686 Protected Mode Kernel", COL_TEXT_GRAY, COL_WIN_BODY);
    ty += 56;
    draw_rect(tx, ty, bw - 40, 1, COL_NOTE_LINE);
    ty += 10;
    draw_text(tx, ty,      "Architecture  : x86 (i686) 32-bit", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Memory Mgmt   : PMM + VMM Paging", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Scheduler     : Preemptive Round-Robin", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Multicore     : SMP (4 CPU Cores)", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Graphics      : Bochs VBE 32BPP LFB", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Sound         : AC'97 DMA Playback", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Filesystems   : FAT32 + USTAR", COL_TEXT_DARK, COL_WIN_BODY); ty += 13;
    draw_text(tx, ty,      "Drivers       : KBD PS/2 Mouse ATA", COL_TEXT_DARK, COL_WIN_BODY); ty += 20;
    draw_text(tx, ty,      "Built with i686-elf-gcc + NASM", COL_TEXT_GRAY, COL_WIN_BODY);
}

void welcome_service_init(void) {
    static const wm_service_t welcome_srv = {
        WIN_WELCOME,
        "Welcome",
        WM_SRV_FLAG_NONE,
        NULL,
        NULL,
        NULL,
        welcome_render,
        NULL
    };
    static const wm_service_t about_srv = {
        WIN_ABOUT,
        "About",
        WM_SRV_FLAG_NONE,
        NULL,
        NULL,
        NULL,
        about_render,
        NULL
    };
    wm_register_service(&welcome_srv);
    wm_register_service(&about_srv);
}
