/* ============================================================================
 * AzamiOS Userspace — Change Mode (chmod.elf)
 * File: userland/apps/chmod/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/sys/stat.h"

static int parse_octal(const char *s)
{
    int val = 0;
    while (*s >= '0' && *s <= '7') {
        val = (val << 3) | (*s - '0');
        s++;
    }
    return val;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: chmod <octal-mode> file ...\n");
        return 1;
    }

    mode_t mode = (mode_t)parse_octal(argv[1]);
    int ret = 0;

    for (int i = 2; i < argc; i++) {
        if (chmod(argv[i], mode) != 0) {
            printf("chmod: cannot access '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
