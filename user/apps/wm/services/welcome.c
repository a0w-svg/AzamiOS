/**
 * welcome.c — AzamiOS v6.0 Welcome & About Window Services
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
    draw_text(tx, ty, "Welcome to AzamiOS v6.0 Modern OS!", COL_TITLE_ACT, COL_WIN_BODY);
    ty += 24;
    draw_text(tx, ty, "Windows 10 Fluent Blue Desktop Environment", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 20;
    draw_rect(tx, ty, bw - 40, 1, COL_NOTE_LINE);
    ty += 14;
    draw_text(tx, ty, "Display  : 1280x800 WXGA True Color (32 BPP)", COL_TEXT_BLUE, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "GPU      : Intel i915 Hardware / Bochs VBE LFB", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "Renderer : Win10 Aero Snap & Taskbar Windows", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "CPU      : x86_64 Multicore Preemptive", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "Security : Stack Canary Canaries + ASLR ON", COL_TEXT_GREEN, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "Apps     : Python REPL, Lynx Browser, NetMon", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "Games    : Snake, Minesweeper, Tetris, 3D Demo", COL_TEXT_DARK, COL_WIN_BODY);
    ty += 22;
    draw_text(tx, ty, "Right-click desktop or drag to screen edges for Aero Snap!", COL_TEXT_GRAY, COL_WIN_BODY);
    ty += 16;
    draw_text(tx, ty, "Open Kernel Debugger for live dmesg, BSOD & GDB stub", COL_TEXT_GRAY, COL_WIN_BODY);
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
    draw_rect(tx, ty, 44, 44, 0x000078D7);
    draw_rect(tx + 4, ty + 4, 36, 36, COL_WIN_BODY);
    draw_text(tx + 10, ty + 12, "Win", 0x000078D7, COL_WIN_BODY);
    draw_text(tx + 10, ty + 24, " 10", COL_TEXT_DARK, COL_WIN_BODY);
    /* Info text */
    int ix = tx + 56;
    draw_text(ix, ty + 4, "AzamiOS v6.0", COL_TITLE_ACT, COL_WIN_BODY);
    draw_text(ix, ty + 18, "Modern Microkernel OS", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(ix, ty + 32, "With Windows 10 DE & GNU Suite", COL_TEXT_GRAY, COL_WIN_BODY);
    ty += 58;
    draw_rect(tx, ty, bw - 40, 1, COL_NOTE_LINE);
    ty += 12;
    draw_text(tx, ty,      "Version       : AzamiOS v6.0 (Build 2026.07)", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "Resolution    : 1280x800 (WXGA)", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "Memory & Sec  : PMM/VMM + Canaries + ASLR", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "Multicore     : SMP 4-Core Scheduling", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "Graphics      : Intel i915 + Bochs VBE LFB", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "GNU Utilities : grep, find, sed, awk, du, df...", COL_TEXT_DARK, COL_WIN_BODY); ty += 15;
    draw_text(tx, ty,      "Networking    : DHCP, DNS Resolver, NTP Sync", COL_TEXT_DARK, COL_WIN_BODY); ty += 20;
    draw_text(tx, ty,      "Built with precision & high optimization (-Ofast)", COL_TEXT_GRAY, COL_WIN_BODY);
}

void welcome_service_init(void) {
    static const wm_service_t welcome_srv = {
        WIN_WELCOME, "Welcome", WM_SRV_FLAG_NONE,
        NULL, NULL, NULL, welcome_render, NULL
    };
    static const wm_service_t about_srv = {
        WIN_ABOUT, "About", WM_SRV_FLAG_NONE,
        NULL, NULL, NULL, about_render, NULL
    };
    wm_register_service(&welcome_srv);
    wm_register_service(&about_srv);
}
