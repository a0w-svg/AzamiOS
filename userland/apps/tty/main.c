/* ============================================================================
 * AzamiOS Userspace — POSIX tty Utility (main.c)
 * File: userland/apps/tty/main.c
 * ============================================================================ */
#include <stdio.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char *argv[])
{
    int silent = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--silent") == 0 || strcmp(argv[i], "--quiet") == 0) {
            silent = 1;
        }
    }
    int is_term = isatty(0);
    if (!silent) {
        if (is_term) {
            printf("/dev/tty\n");
        } else {
            printf("not a tty\n");
        }
    }
    return is_term ? 0 : 1;
}
