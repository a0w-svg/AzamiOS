/* ============================================================================
 * AzamiOS Userspace — Echo Utility (echo.elf)
 * File: userland/apps/echo/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"

int main(int argc, char **argv)
{
    bool newline = true;
    int start = 1;

    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = false;
        start = 2;
    }

    for (int i = start; i < argc; i++) {
        printf("%s", argv[i]);
        if (i < argc - 1) {
            putchar(' ');
        }
    }

    if (newline) {
        putchar('\n');
    }

    return 0;
}
