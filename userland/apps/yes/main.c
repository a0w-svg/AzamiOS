/* ============================================================================
 * AzamiOS Userspace — Output a String Repeatedly (yes.elf)
 * File: userland/apps/yes/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"

int main(int argc, char **argv)
{
    const char *str = (argc > 1) ? argv[1] : "y";
    for (;;) {
        puts(str);
    }
    return 0;
}
