/* ============================================================================
 * AzamiOS Userspace — Clear Screen (clear.elf)
 * File: userland/apps/clear/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* ANSI escape: clear screen and move cursor to home (1,1) */
    printf("\033[2J\033[H");
    return 0;
}
