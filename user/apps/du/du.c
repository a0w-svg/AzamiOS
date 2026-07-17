/**
 * du.c — Disk usage estimation utility for AzamiOS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static unsigned long walk_du(const char *path, int summarize) {
    DIR *d = opendir(path);
    if (!d) return 4; /* default block size for files/dirs if unopenable */

    unsigned long total = 4; /* directory block itself */
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char full[256];
        if (strcmp(path, "/") == 0) snprintf(full, sizeof(full), "/%s", ent->d_name);
        else snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);

        unsigned long sz = walk_du(full, summarize);
        total += sz;
    }
    closedir(d);
    if (!summarize) {
        printf("%lu\t%s\n", total, path);
    }
    return total;
}

int main(int argc, char **argv) {
    const char *target = ".";
    int summarize = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-sh") == 0) summarize = 1;
        else if (argv[i][0] != '-') target = argv[i];
    }
    unsigned long tot = walk_du(target, summarize);
    if (summarize) {
        if (tot > 1024) printf("%luM\t%s\n", tot / 1024, target);
        else printf("%luK\t%s\n", tot, target);
    }
    return 0;
}
