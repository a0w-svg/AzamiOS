/* ============================================================================
 * AzamiOS Userspace — Canonical Hex Dump Utility (hexdump.elf)
 * File: userland/apps/hexdump/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

static void dump_fd(int fd)
{
    unsigned char buf[16];
    size_t offset = 0;
    ssize_t n;

    while ((n = read(fd, buf, 16)) > 0) {
        printf("%08lx  ", (unsigned long)offset);

        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", buf[i]);
            else       printf("   ");
            if (i == 7) printf(" ");
        }

        printf(" |");
        for (int i = 0; i < n; i++) {
            putchar(isprint(buf[i]) ? buf[i] : '.');
        }
        printf("|\n");

        offset += (size_t)n;
    }
    printf("%08lx\n", (unsigned long)offset);
}

int main(int argc, char **argv)
{
    if (argc <= 1) {
        dump_fd(0);
        return 0;
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("hexdump: %s: No such file or directory\n", argv[i]);
            continue;
        }
        dump_fd(fd);
        close(fd);
    }
    return 0;
}
