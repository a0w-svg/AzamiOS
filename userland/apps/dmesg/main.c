/* ============================================================================
 * AzamiOS Userspace — Kernel Ring Buffer Log Utility (dmesg.elf)
 * File: userland/apps/dmesg/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/sys/klog.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n"
           "Display or control the kernel ring buffer.\n\n"
           "Options:\n"
           "  -c, --read-clear  read and clear all messages\n"
           "  -C, --clear       clear the kernel ring buffer\n"
           "  -h, --help        display this help\n"
           "  -V, --version     display version\n",
           prog);
}

int main(int argc, char **argv)
{
    bool opt_clear = false;
    bool opt_only_clear = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("dmesg from util-linux (AzamiOS compatibility) 7.0\n");
            return 0;
        }
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--read-clear") == 0) {
            opt_clear = true;
            continue;
        }
        if (strcmp(argv[i], "-C") == 0 || strcmp(argv[i], "--clear") == 0) {
            opt_only_clear = true;
            continue;
        }
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'c') opt_clear = true;
                else if (argv[i][j] == 'C') opt_only_clear = true;
                else if (argv[i][j] == 'h') { print_usage(argv[0]); return 0; }
                else if (argv[i][j] == 'V') {
                    printf("dmesg from util-linux (AzamiOS compatibility) 7.0\n");
                    return 0;
                } else {
                    fprintf(stderr, "dmesg: invalid option -- '%c'\n", argv[i][j]);
                    return 1;
                }
            }
        }
    }

    if (opt_only_clear) {
        if (klogctl(5, NULL, 0) < 0) {
            fprintf(stderr, "dmesg: klogctl clear failed\n");
            return 1;
        }
        return 0;
    }

    /* Print kernel log */
    int fd = open("/proc/dmesg", O_RDONLY, 0);
    if (fd < 0) {
        fd = open("/proc/kmsg", O_RDONLY, 0);
    }

    if (fd >= 0) {
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, (size_t)n);
        }
        close(fd);
    } else {
        /* Try klogctl */
        char kbuf[8192];
        int n = klogctl(3, kbuf, sizeof(kbuf) - 1);
        if (n > 0) {
            write(1, kbuf, (size_t)n);
        } else {
            fprintf(stderr, "dmesg: read kernel buffer failed (permission denied or no /proc/dmesg)\n");
            return 1;
        }
    }

    if (opt_clear) {
        klogctl(5, NULL, 0);
    }

    return 0;
}
