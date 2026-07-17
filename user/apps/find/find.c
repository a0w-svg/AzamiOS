/**
 * find.c — Directory search utility for AzamiOS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static void walk_dir(const char *path, const char *name_filter) {
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[256];
        if (strcmp(path, "/") == 0) snprintf(full, sizeof(full), "/%s", ent->d_name);
        else snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        if (!name_filter || strstr(ent->d_name, name_filter) != NULL) {
            printf("%s\n", full);
        }
        walk_dir(full, name_filter);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    const char *start_path = ".";
    const char *name_filter = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            name_filter = argv[++i];
        } else if (argv[i][0] != '-') {
            start_path = argv[i];
        }
    }
    walk_dir(start_path, name_filter);
    return 0;
}
