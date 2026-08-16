/* ============================================================================
 * AzamiOS Userspace — Directory List Utility (ls.elf)
 * File: userland/apps/ls/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"

static void list_dir(const char *path, bool show_all, bool long_list)
{
    DIR *dir = opendir(path);
    if (!dir) {
        /* If it's a regular file or special device, stat and print directly */
        struct stat st;
        if (stat(path, &st) == 0) {
            if (long_list) {
                char type_char = '-';
                if (S_ISDIR(st.st_mode)) type_char = 'd';
                else if (S_ISCHR(st.st_mode)) type_char = 'c';
                else if (S_ISBLK(st.st_mode)) type_char = 'b';
                else if (S_ISLNK(st.st_mode)) type_char = 'l';
                printf("%c%04o %8lld  %s\n", type_char, st.st_mode & 0777, (long long)st.st_size, path);
            } else {
                printf("%s\n", path);
            }
            return;
        }
        printf("ls: cannot access '%s': No such file or directory\n", path);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!show_all && ent->d_name[0] == '.') {
            continue;
        }

        if (long_list) {
            char full_path[512];
            if (strcmp(path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "/%s", ent->d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
            }

            struct stat st;
            char type_char = '-';
            long long size = 0;
            unsigned int mode = 0644;

            if (stat(full_path, &st) == 0) {
                size = (long long)st.st_size;
                mode = st.st_mode;
                if (S_ISDIR(st.st_mode)) type_char = 'd';
                else if (S_ISCHR(st.st_mode)) type_char = 'c';
                else if (S_ISBLK(st.st_mode)) type_char = 'b';
                else if (S_ISLNK(st.st_mode)) type_char = 'l';
            } else {
                if (ent->d_type == DT_DIR) type_char = 'd';
                else if (ent->d_type == DT_CHR) type_char = 'c';
                else if (ent->d_type == DT_BLK) type_char = 'b';
                else if (ent->d_type == DT_LNK) type_char = 'l';
            }

            printf("%c%04o %8lld  %s\n", type_char, mode & 0777, size, ent->d_name);
        } else {
            printf("%s\n", ent->d_name);
        }
    }

    closedir(dir);
}

int main(int argc, char **argv)
{
    bool show_all = false;
    bool long_list = false;
    const char *paths[64];
    int path_count = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            for (int j = 1; argv[i][j] != '\0'; j++) {
                if (argv[i][j] == 'a') show_all = true;
                else if (argv[i][j] == 'l') long_list = true;
            }
        } else {
            if (path_count < 64) {
                paths[path_count++] = argv[i];
            }
        }
    }

    if (path_count == 0) {
        paths[0] = ".";
        path_count = 1;
    }

    for (int i = 0; i < path_count; i++) {
        if (path_count > 1) {
            printf("%s:\n", paths[i]);
        }
        list_dir(paths[i], show_all, long_list);
        if (path_count > 1 && i < path_count - 1) {
            printf("\n");
        }
    }

    return 0;
}
