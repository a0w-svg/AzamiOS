/**
 * sysmon.c — AzamiOS Real-Time System Monitor Service
 *
 * EDUCATIONAL ARCHITECTURE & MEMORY SAFETY EXPLANATIONS:
 * 1. Safe Formatting of Telemetry Data:
 *    System statistics like frame counts and uptime undergo stack formatting.
 *    Using explicit buffer lengths (`fb[32]`, `tb[16]`) alongside bounded formatting
 *    helpers (`format_time_str`) prevents stack overflow vulnerabilities during 1Hz telemetry updates.
 * 2. Static Capability Declaration:
 *    System Monitor registers with `WM_SRV_FLAG_NONE` because its telemetry updates
 *    coincide naturally with the OS real-time clock tick events processed in `main.c`.
 */

#include "../wm.h"
#include <sys/cpu.h>

static void sysmon_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)blink;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;

    static cpu_info_t cpu;
    static int cpu_cached = 0;
    if (!cpu_cached) {
        get_cpu_info(&cpu);
        cpu_cached = 1;
    }
    char cpu_str[64];
    strncpy(cpu_str, cpu.vendor, sizeof(cpu_str) - 1);
    cpu_str[sizeof(cpu_str) - 1] = '\0';
    strncat(cpu_str, " 4-Core SMP", sizeof(cpu_str) - strlen(cpu_str) - 1);
    if (cpu.has_fpu || cpu.has_sse) {
        strncat(cpu_str, " (", sizeof(cpu_str) - strlen(cpu_str) - 1);
        if (cpu.has_fpu) strncat(cpu_str, "FPU ", sizeof(cpu_str) - strlen(cpu_str) - 1);
        if (cpu.has_sse) strncat(cpu_str, "SSE ", sizeof(cpu_str) - strlen(cpu_str) - 1);
        if (cpu.has_apic) strncat(cpu_str, "APIC", sizeof(cpu_str) - strlen(cpu_str) - 1);
        strncat(cpu_str, ")", sizeof(cpu_str) - strlen(cpu_str) - 1);
    }

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
    draw_text(tx, ty, "CPU    : ", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(tx + 72, ty, cpu_str, COL_TEXT_DARK, COL_WIN_BODY);
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
        WM_SRV_FLAG_NONE,
        NULL,
        NULL,
        NULL,
        sysmon_render,
        NULL
    };
    wm_register_service(&sysmon_srv);
}
