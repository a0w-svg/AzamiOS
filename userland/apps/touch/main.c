/* ============================================================================
 * AzamiOS Userspace — File Timestamp / Creation (touch.elf)
 * File: userland/apps/touch/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: touch file ...\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            printf("touch: cannot touch '%s'\n", argv[i]);
            ret = 1;
        } else {
            close(fd);
        }
    }
    return ret;
}
