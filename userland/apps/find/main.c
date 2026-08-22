/* ============================================================================
 * AzamiOS Userspace — Find Utility (find.elf)
 * File: userland/apps/find/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include <stdbool.h>

static const char *g_name_pattern = NULL;
static char g_type_filter = '\0'; /* 'f' for regular file, 'd' for directory */
static int g_max_depth = 100;

/* Simple wildcard pattern matcher: supports '*' and exact matching */
static bool match_pattern(const char *pattern, const char *text)
{
    if (!pattern) return true;

    while (*pattern && *text) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true; /* Trailing '*' matches everything */
            while (*text) {
                if (match_pattern(pattern, text)) return true;
                text++;
            }
            return false;
        } else if (*pattern == *text) {
            pattern++;
            text++;
        } else {
            return false;
        }
    }

    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *text == '\0');
}

static void search_directory(const char *base_path, int depth)
{
    if (depth > g_max_depth) return;

    struct stat st;
    if (stat(base_path, &st) < 0) return;

    bool is_dir = S_ISDIR(st.st_mode);
    bool is_reg = S_ISREG(st.st_mode);

    /* Extract the basename */
    const char *bname = strrchr(base_path, '/');
    bname = bname ? (bname + 1) : base_path;
    if (!*bname) bname = base_path;

    /* Check filters */
    bool type_matches = true;
    if (g_type_filter == 'f' && !is_reg) type_matches = false;
    if (g_type_filter == 'd' && !is_dir) type_matches = false;

    bool name_matches = match_pattern(g_name_pattern, bname);

    if (type_matches && name_matches) {
        puts(base_path);
    }

    if (!is_dir) return;

    DIR *d = opendir(base_path);
    if (!d) return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char subpath[512];
        if (strcmp(base_path, "/") == 0) {
            snprintf(subpath, sizeof(subpath), "/%s", entry->d_name);
        } else {
            snprintf(subpath, sizeof(subpath), "%s/%s", base_path, entry->d_name);
        }

        search_directory(subpath, depth + 1);
    }

    closedir(d);
}

int main(int argc, char **argv)
{
    const char *start_paths[32];
    int start_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            g_name_pattern = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            g_type_filter = argv[++i][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0 && i + 1 < argc) {
            g_max_depth = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-print") == 0) {
            /* Default action, ignore */
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            puts("Usage: find [path...] [-name pattern] [-type f|d] [-maxdepth N] [-print]");
            puts("Search for files and directories in a directory hierarchy.");
            return 0;
        } else if (argv[i][0] != '-') {
            if (start_count < 32) {
                start_paths[start_count++] = argv[i];
            }
        }
    }

    if (start_count == 0) {
        start_paths[0] = ".";
        start_count = 1;
    }

    for (int i = 0; i < start_count; i++) {
        search_directory(start_paths[i], 0);
    }

    return 0;
}
