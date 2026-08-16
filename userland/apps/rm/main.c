/* ============================================================================
 * AzamiOS Userspace — Remove File / Directory (rm.elf)
 * File: userland/apps/rm/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"

static int remove_recursive(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        return unlink(path);
    }

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path);
    }

    DIR *dir = opendir(path);
    if (!dir) {
        return rmdir(path);
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char sub[512];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        remove_recursive(sub);
    }

    closedir(dir);
    return rmdir(path);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: rm [-r|-f] file ...\n");
        return 1;
    }

    bool recursive = false;
    bool force = false;
    int start = 1;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j] != '\0'; j++) {
                if (argv[i][j] == 'r' || argv[i][j] == 'R') recursive = true;
                else if (argv[i][j] == 'f') force = true;
            }
            start = i + 1;
        } else {
            break;
        }
    }

    int ret = 0;
    for (int i = start; i < argc; i++) {
        int r = recursive ? remove_recursive(argv[i]) : unlink(argv[i]);
        if (r != 0 && !force) {
            printf("rm: cannot remove '%s'\n", argv[i]);
            ret = 1;
        }
    }
    return ret;
}
