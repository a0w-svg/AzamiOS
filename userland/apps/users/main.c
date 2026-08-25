/* ============================================================================
 * AzamiOS Userspace — POSIX users Utility (main.c)
 * File: userland/apps/users/main.c
 * ============================================================================ */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    char *user = getlogin();
    if (!user || !*user) user = getenv("USER");
    if (!user || !*user) user = "root";
    printf("%s\n", user);
    return 0;
}
