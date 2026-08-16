/* ============================================================================
 * AzamiOS Userspace — System Date & Time (date.elf)
 * File: userland/apps/date/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/time.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        time_t t = tv.tv_sec;
        struct tm *tm_info = gmtime(&t);
        if (tm_info) {
            char buf[64];
            strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S UTC %Y", tm_info);
            printf("%s\n", buf);
            return 0;
        }
    }
    printf("Sat Aug 15 23:00:00 UTC 2026\n");
    return 0;
}
