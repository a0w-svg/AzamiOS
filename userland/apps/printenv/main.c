/* ============================================================================
 * AzamiOS Userspace — printenv (Print all or part of environment)
 * File: userland/apps/printenv/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

extern char **environ;

int main(int argc, char **argv)
{
    int null_term = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-0") == 0 || strcmp(argv[opt_idx], "--null") == 0) {
            null_term = 1;
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else break;
        opt_idx++;
    }

    if (opt_idx >= argc) {
        if (environ) {
            for (char **env = environ; *env; env++) {
                fputs(*env, stdout);
                putchar(null_term ? '\0' : '\n');
            }
        }
        return 0;
    }

    int found_all = 1;
    for (int i = opt_idx; i < argc; i++) {
        char *val = getenv(argv[i]);
        if (val) {
            fputs(val, stdout);
            putchar(null_term ? '\0' : '\n');
        } else {
            found_all = 0;
        }
    }

    return found_all ? 0 : 1;
}
