/* ============================================================================
 * AzamiOS Userspace — Make Directory Utility (mkdir.elf)
 * File: userland/apps/mkdir/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/sys/stat.h"

static int make_path(const char *path, mode_t mode)
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mkdir [-p] directory ...\n");
        return 1;
    }

    bool parents = false;
    int start = 1;
    if (strcmp(argv[1], "-p") == 0) {
        parents = true;
        start = 2;
    }

    int ret = 0;
    for (int i = start; i < argc; i++) {
        int r = parents ? make_path(argv[i], 0755) : mkdir(argv[i], 0755);
        if (r != 0) {
            printf("mkdir: cannot create directory '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
