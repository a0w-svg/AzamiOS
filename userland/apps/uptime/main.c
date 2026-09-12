/* ============================================================================
 * AzamiOS Userspace — System Uptime (uptime.elf)
 * File: userland/apps/uptime/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/sysinfo.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n"
           "Options:\n"
           "  -p, --pretty   show uptime in pretty format\n"
           "  -h, --help     display this help and exit\n"
           "  -s, --since    system up since\n"
           "  -V, --version  output version information and exit\n",
           prog);
}

int main(int argc, char **argv)
{
    bool pretty = false;
    bool since = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("uptime from procps-ng (AzamiOS compatibility) 7.0\n");
            return 0;
        }
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pretty") == 0) {
            pretty = true;
            continue;
        }
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--since") == 0) {
            since = true;
            continue;
        }
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                switch (argv[i][j]) {
                    case 'p': pretty = true; break;
                    case 's': since = true; break;
                    case 'h': print_usage(argv[0]); return 0;
                    case 'V':
                        printf("uptime from procps-ng (AzamiOS compatibility) 7.0\n");
                        return 0;
                    default:
                        fprintf(stderr, "uptime: invalid option -- '%c'\n"
                                        "Try 'uptime --help' for more information.\n", argv[i][j]);
                        return 1;
                }
            }
        }
    }

    struct sysinfo info;
    if (sysinfo(&info) != 0) {
        printf("uptime: syscall failed\n");
        return 1;
    }

    long uptime_sec = info.uptime;
    time_t now = time(NULL);

    if (since) {
        time_t boot_time = now > uptime_sec ? (now - uptime_sec) : 0;
        struct tm tm_boot;
        localtime_r(&boot_time, &tm_boot);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_boot);
        printf("%s\n", buf);
        return 0;
    }

    long days = uptime_sec / 86400;
    long hours = (uptime_sec % 86400) / 3600;
    long mins = (uptime_sec % 3600) / 60;

    if (pretty) {
        printf("up ");
        bool has_prev = false;
        if (days > 0) {
            printf("%ld day%s", days, days > 1 ? "s" : "");
            has_prev = true;
        }
        if (hours > 0) {
            if (has_prev) printf(", ");
            printf("%ld hour%s", hours, hours > 1 ? "s" : "");
            has_prev = true;
        }
        if (mins > 0 || !has_prev) {
            if (has_prev) printf(", ");
            printf("%ld minute%s", mins, mins > 1 ? "s" : "");
        }
        putchar('\n');
        return 0;
    }

    /* Standard output format */
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", &tm_now);

    /* Load average */
    double l1 = 0.0, l5 = 0.0, l15 = 0.0;
    int fd = open("/proc/loadavg", O_RDONLY, 0);
    if (fd >= 0) {
        char lbuf[64];
        ssize_t n = read(fd, lbuf, sizeof(lbuf) - 1);
        close(fd);
        if (n > 0) {
            lbuf[n] = '\0';
            sscanf(lbuf, "%lf %lf %lf", &l1, &l5, &l15);
        }
    } else {
        l1 = (double)info.loads[0] / 65536.0;
        l5 = (double)info.loads[1] / 65536.0;
        l15 = (double)info.loads[2] / 65536.0;
    }

    printf(" %s up ", time_str);
    if (days > 0) {
        printf("%ld day%s, ", days, days > 1 ? "s" : "");
    }
    if (hours > 0 || days > 0) {
        printf("%2ld:%02ld, ", hours, mins);
    } else {
        printf("%ld min, ", mins);
    }

    /* User count */
    printf(" 1 user,  load average: %.2f, %.2f, %.2f\n", l1, l5, l15);

    return 0;
}
