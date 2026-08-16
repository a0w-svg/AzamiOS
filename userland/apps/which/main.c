/* ============================================================================
 * AzamiOS Userspace — Locate Command (which.elf)
 * File: userland/apps/which/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"

static int which_find(const char *cmd)
{
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) {
            printf("%s\n", cmd);
            return 0;
        }
        return 1;
    }

    const char *path_env = getenv("PATH");
    if (!path_env) path_env = "/bin:/usr/bin:/";

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
                printf("%s\n", full);
                return 0;
            }
            snprintf(full, sizeof(full), "/%s", cmd);
            if (access(full, X_OK) == 0) {
                printf("%s\n", full);
                return 0;
            }
        } else {
            snprintf(full, sizeof(full), "%s/%s.elf", dir, cmd);
            if (access(full, X_OK) == 0) {
                printf("%s\n", full);
                return 0;
            }
            snprintf(full, sizeof(full), "%s/%s", dir, cmd);
            if (access(full, X_OK) == 0) {
                printf("%s\n", full);
                return 0;
            }
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: which command ...\n");
        return 1;
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (which_find(argv[i]) != 0) {
            ret = 1;
        }
    }
    return ret;
}
