/* ============================================================================
 * AzamiOS Userspace — Directory Tree Generator (tree.elf)
 * File: userland/apps/tree/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"

static int g_dirs = 0;
static int g_files = 0;

static void tree_walk(const char *dir_path, const char *prefix, int depth)
{
    if (depth > 20) return;

    DIR *dir = opendir(dir_path);
    if (!dir) return;

    /* Count valid entries to know which is last */
    char names[128][64];
    bool is_dir[128];
    int count = 0;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && count < 128) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, de->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            strncpy(names[count], de->d_name, sizeof(names[count]) - 1);
            is_dir[count] = S_ISDIR(st.st_mode);
            count++;
        }
    }
    closedir(dir);

    for (int i = 0; i < count; i++) {
        bool last = (i == count - 1);
        printf("%s%s %s\n", prefix, last ? "`--" : "|--", names[i]);

        if (is_dir[i]) {
            g_dirs++;
            char sub_prefix[256];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s", prefix, last ? "    " : "|   ");

            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, names[i]);
            tree_walk(full_path, sub_prefix, depth + 1);
        } else {
            g_files++;
        }
    }
}

int main(int argc, char **argv)
{
    const char *root = (argc > 1) ? argv[1] : ".";

    printf("%s\n", root);
    tree_walk(root, "", 0);

    printf("\n%d directories, %d files\n", g_dirs, g_files);
    return 0;
}
