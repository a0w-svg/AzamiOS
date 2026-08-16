/* ============================================================================
 * AzamiOS Userspace — Environment Utility (env.elf)
 * File: userland/apps/env/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"

int main(int argc, char **argv, char **envp)
{
    int i = 1;

    /* Parse assignments of the form NAME=VALUE */
    while (i < argc && strchr(argv[i], '=') != NULL) {
        putenv(argv[i]);
        i++;
    }

    /* If no command is given, dump the environment */
    if (i >= argc) {
        if (envp) {
            for (char **ep = envp; *ep != NULL; ep++) {
                printf("%s\n", *ep);
            }
        }
        return 0;
    }

    /* Execute the specified command */
    execvp(argv[i], &argv[i]);

    printf("env: '%s': No such file or directory\n", argv[i]);
    return 127;
}
