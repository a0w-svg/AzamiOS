/* ============================================================================
 * AzamiOS Userspace — File Concatenator (cat.elf)
 * File: userland/apps/cat/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

static void dump_fd(int fd)
{
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(1, buf + written, n - written);
            if (w <= 0) break;
            written += w;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc <= 1) {
        dump_fd(0);
        return 0;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            dump_fd(0);
            continue;
        }

        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("cat: %s: No such file or directory\n", argv[i]);
            ret = 1;
            continue;
        }

        dump_fd(fd);
        close(fd);
    }

    return ret;
}
