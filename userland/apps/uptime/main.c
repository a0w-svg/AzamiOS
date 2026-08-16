/* ============================================================================
 * AzamiOS Userspace — System Uptime (uptime.elf)
 * File: userland/apps/uptime/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/sys/sysinfo.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        printf("uptime: syscall failed\n");
        return 1;
    }

    long uptime_sec = info.uptime;
    long days = uptime_sec / 86400;
    long hours = (uptime_sec % 86400) / 3600;
    long mins = (uptime_sec % 3600) / 60;
    long secs = uptime_sec % 60;

    printf("up ");
    if (days > 0) printf("%ld day%s, ", days, days > 1 ? "s" : "");
    if (hours > 0 || days > 0) printf("%02ld:%02ld:%02ld", hours, mins, secs);
    else printf("%ld min%s, %ld sec%s", mins, mins > 1 ? "s" : "", secs, secs > 1 ? "s" : "");
    printf(", %u processes\n", info.procs);

    return 0;
}
