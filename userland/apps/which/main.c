/* ============================================================================
 * AzamiOS Userspace — Locate Command (which.elf)
 * File: userland/apps/which/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/unistd.h"

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] [--] programname [...]\n\n"
           "Options:\n"
           "  -a         print all matching pathnames of each argument\n"
           "  -s         silent mode, no output, just exit status\n"
           "  --help     display this help and exit\n"
           "  --version  output version information and exit\n",
           prog);
}

static int which_find(const char *cmd, bool all_matches, bool silent)
{
    int found_count = 0;

    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) {
            if (!silent) printf("%s\n", cmd);
            return 0;
        }
        return 1;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) path_env = "/bin:/usr/bin:/sbin:/usr/sbin:/";

    char paths[512];
    strncpy(paths, path_env, sizeof(paths) - 1);
    paths[sizeof(paths) - 1] = '\0';

    char *saveptr = NULL;
    char *dir = strtok_r(paths, ":", &saveptr);

    while (dir) {
        char full[512];
        if (strcmp(dir, "/") == 0) {
            snprintf(full, sizeof(full), "/%s.elf", cmd);
            if (access(full, X_OK) == 0) {
                if (!silent) printf("%s\n", full);
                found_count++;
                if (!all_matches) return 0;
            }
            snprintf(full, sizeof(full), "/%s", cmd);
            if (access(full, X_OK) == 0) {
                if (!silent) printf("%s\n", full);
                found_count++;
                if (!all_matches) return 0;
            }
        } else {
            snprintf(full, sizeof(full), "%s/%s.elf", dir, cmd);
            if (access(full, X_OK) == 0) {
                if (!silent) printf("%s\n", full);
                found_count++;
                if (!all_matches) return 0;
            }
            snprintf(full, sizeof(full), "%s/%s", dir, cmd);
            if (access(full, X_OK) == 0) {
                if (!silent) printf("%s\n", full);
                found_count++;
                if (!all_matches) return 0;
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    return found_count > 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    bool all_matches = false;
    bool silent = false;
    int arg_start = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            arg_start = i + 1;
            break;
        }
        if (strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0) {
            printf("which (AzamiOS coreutils) 7.0\n");
            return 0;
        }
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j]; j++) {
                if (argv[i][j] == 'a') all_matches = true;
                else if (argv[i][j] == 's') silent = true;
                else {
                    fprintf(stderr, "which: invalid option -- '%c'\n", argv[i][j]);
                    return 1;
                }
            }
            arg_start = i + 1;
        } else {
            arg_start = i;
            break;
        }
    }

    if (arg_start >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    int ret = 0;
    for (int i = arg_start; i < argc; i++) {
        if (which_find(argv[i], all_matches, silent) != 0) {
            ret = 1;
        }
    }
    return ret;
}
