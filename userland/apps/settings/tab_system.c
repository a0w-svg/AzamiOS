#include "settings.h"


/* ── System Tab ────────────────────────────────────────────────────────────── */
#define SYS_SEC_Y  90
#define SYS_GRID_Y 122

void draw_system_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, SYS_SEC_Y, (int)w - 40, "Kernel & System Architecture", UK_TEAL);

    struct sysinfo si;
    sysinfo(&si);
    unsigned long total_mb = (si.totalram * si.mem_unit) / (1024 * 1024);
    unsigned long free_mb  = (si.freeram * si.mem_unit) / (1024 * 1024);

    char mem_buf[64];
    snprintf(mem_buf, sizeof(mem_buf), "%lu MB Total (%lu MB Free)", total_mb, free_mb);

    /* Real core count from sysconf(), not a guess: it was a hardcoded "4"
     * until sysconf() itself was fixed to read /proc/cpuinfo's real
     * enumeration (fs/procfs.c, smp_cpu_count()). */
    long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
    char cpu_buf[64];
    snprintf(cpu_buf, sizeof(cpu_buf), "%ld Core%s (Preemptive CFS Scheduling)",
             ncpus, ncpus == 1 ? "" : "s");

    static const char *sys_info[][2] = {
        { "Operating System", "AzamiOS v7.0.0 (x86_64 Microkernel)" },
        { "SMP CPU Cores",    "" },
        { "Memory Model",     "Buddy PMM + 4-Level VMM (PML4)" },
        { "System Memory",    "" },
        { "Storage System",   "Persistent SATA AHCI (/hdd) + Ext2" },
        { "Window Server",    "azwm Compositor (Zero-Copy SHMEM)" },
        { "Audio Controller", "Intel AC97 PCI (/dev/dsp)" },
        { "Power Management", "Intel PIIX4 ACPI PM (I/O 0xB000)" },
        { "Security Engine",  "LSM + YAMA + Auto-Accept Policy" },
    };

    int py = SYS_GRID_Y;
    for (int i = 0; i < 9; i++) {
        uk_draw_panel(&g_win, px, py, (int)w - 40, 24, UK_SURFACE0);
        uk_draw_text(&g_win, px + 10, py + 4, sys_info[i][0], UK_SUBTEXT0);
        if (i == 1) {
            uk_draw_text(&g_win, px + 180, py + 4, cpu_buf, UK_GREEN);
        } else if (i == 3) {
            uk_draw_text(&g_win, px + 180, py + 4, mem_buf, UK_GREEN);
        } else {
            uk_draw_text(&g_win, px + 180, py + 4, sys_info[i][1], UK_TEXT);
        }
        py += 28;
    }
}


void handle_system_mouse(int mx, int my)
{
}
