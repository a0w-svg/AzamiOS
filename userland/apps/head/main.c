/* ============================================================================
 * AzamiOS Userspace — Output First Lines (head.elf)
 * File: userland/apps/head/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

static void head_fd(int fd, int max_lines)
{
    char buf[1];
    int lines = 0;
    while (read(fd, buf, 1) == 1) {
        putchar(buf[0]);
        if (buf[0] == '\n') {
            lines++;
            if (lines >= max_lines) break;
        }
    }
}

int main(int argc, char **argv)
{
    int max_lines = 10;
    int start = 1;

    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        max_lines = atoi(argv[2]);
        start = 3;
    }

    if (start >= argc) {
        head_fd(0, max_lines);
        return 0;
    }

    for (int i = start; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("head: cannot open '%s'\n", argv[i]);
            continue;
        }
        if (argc - start > 1) {
            printf("==> %s <==\n", argv[i]);
        }
        head_fd(fd, max_lines);
        close(fd);
    }

    return 0;
}
