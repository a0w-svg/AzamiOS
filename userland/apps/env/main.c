/* ============================================================================
 * AzamiOS Userspace — POSIX Environment Utility (env.elf)
 * File: userland/apps/env/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"

extern char **environ;

int main(int argc, char **argv)
{
    int i = 1;

    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0) {
            /* Clear environment */
            if (environ) {
                for (int j = 0; environ[j]; j++) {
                    char *eq = strchr(environ[j], '=');
                    if (eq) {
                        char name[64];
                        size_t nlen = (size_t)(eq - environ[j]);
                        if (nlen < sizeof(name)) {
                            strncpy(name, environ[j], nlen);
                            name[nlen] = '\0';
                            unsetenv(name);
                        }
                    }
                }
            }
            i++;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            unsetenv(argv[i + 1]);
            i += 2;
        } else {
            break;
        }
    }

    /* Parse assignments of the form NAME=VALUE */
    while (i < argc && strchr(argv[i], '=') != NULL) {
        putenv(argv[i]);
        i++;
    }

    /* If no command is given, dump the active environment */
    if (i >= argc) {
        if (environ) {
            for (char **ep = environ; *ep != NULL; ep++) {
                printf("%s\n", *ep);
            }
        }
        return 0;
    }

    /* Execute the specified command with PATH lookup */
    execvp(argv[i], &argv[i]);

    fprintf(stderr, "env: '%s': No such file or directory\n", argv[i]);
    return 127;
}
