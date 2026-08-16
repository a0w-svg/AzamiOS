/* ============================================================================
 * AzamiOS Userspace — Kernel Ring Buffer Log Utility (dmesg.elf)
 * File: userland/apps/dmesg/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    int fd = open("/proc/dmesg", O_RDONLY, 0);
    if (fd < 0) {
        fd = open("/proc/kmsg", O_RDONLY, 0);
    }

    if (fd < 0) {
        fprintf(stderr, "dmesg: cannot open /proc/dmesg\n");
        return 1;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }

    close(fd);
    return 0;
}
