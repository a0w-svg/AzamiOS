/* ============================================================================
 * AzamiOS Userspace — Print Working Directory (pwd.elf)
 * File: userland/apps/pwd/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    char cwd[512];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("%s\n", cwd);
        return 0;
    } else {
        puts("/");
        return 1;
    }
}
