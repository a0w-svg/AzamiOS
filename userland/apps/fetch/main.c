/* ============================================================================
 * AzamiOS — System Information Fetch Utility (fetch.elf)
 * File: userland/apps/fetch/main.c
 * ============================================================================ */
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sysinfo si;
    sysinfo(&si);

    unsigned long unit = si.mem_unit ? si.mem_unit : 4096;
    unsigned long total_mb = (si.totalram * unit) / (1024 * 1024);
    unsigned long free_mb = (si.freeram * unit) / (1024 * 1024);
    unsigned long used_mb = total_mb > free_mb ? (total_mb - free_mb) : 0;

    puts("\033[1;35m      /\\        \033[1;36mOS:\033[0m      AzamiOS v7.0 (x86_64)");
    puts("\033[1;35m     /  \\       \033[1;36mKernel:\033[0m  7.0.0-posix (SMP / CFS Preemptive)");
    puts("\033[1;35m    / /\\ \\      \033[1;36mUptime:\033[0m  Active session");
    puts("\033[1;35m   / /  \\ \\     \033[1;36mShell:\033[0m   Azami Terminal v1.0");
    puts("\033[1;35m  / / /\\ \\ \\    \033[1;36mWM:\033[0m      azwm v2.0 (Double-Buffered)");
    puts("\033[1;35m / / /  \\ \\ \\   \033[1;36mDisplay:\033[0m 1280x800 32bpp (Bochs BGA / VirtIO)");

    char mem_line[128];
    snprintf(mem_line, sizeof(mem_line),
             "\033[1;35m/_/ /_/\\ \\_\\_\\  \033[1;36mMemory:\033[0m   %lu MB / %lu MB (Procs: %d)",
             used_mb, total_mb, (int)si.procs);
    puts(mem_line);

    puts("                \033[1;31m■ \033[1;32m■ \033[1;33m■ \033[1;34m■ \033[1;35m■ \033[1;36m■ \033[1;37m■\033[0m");

    return 0;
}

