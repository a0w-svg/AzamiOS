/* ============================================================================
 * AzamiOS Userspace — Memory Free Utility (free.elf)
 * File: userland/apps/free/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/sys/sysinfo.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        printf("free: sysinfo syscall failed\n");
        return 1;
    }

    unsigned long long unit = info.mem_unit ? info.mem_unit : 4096;
    unsigned long long total_mb = (info.totalram * unit) / (1024 * 1024);
    unsigned long long free_mb = (info.freeram * unit) / (1024 * 1024);
    unsigned long long used_mb = total_mb > free_mb ? (total_mb - free_mb) : 0;

    printf("%-10s %10s %10s %10s\n", "", "total", "used", "free");
    printf("%-10s %9lluM %9lluM %9lluM\n", "Mem:", total_mb, used_mb, free_mb);
    printf("%-10s %9lluM %9lluM %9lluM\n", "Swap:", 0ULL, 0ULL, 0ULL);

    return 0;
}
