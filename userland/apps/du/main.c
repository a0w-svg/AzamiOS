/* ============================================================================
 * AzamiOS Userspace — Disk Usage Utility (du.elf)
 * File: userland/apps/du/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include <stdbool.h>

static bool g_human_readable = false;
static bool g_summary_only = false;
static bool g_all_files = false;
static int  g_max_depth = 100;

static void format_size(unsigned long long bytes, char *out, size_t max_len)
{
    if (!g_human_readable) {
        unsigned long long kb = (bytes + 1023ULL) / 1024ULL;
        if (kb == 0 && bytes > 0) kb = 1;
        snprintf(out, max_len, "%llu", kb);
        return;
    }

    if (bytes < 1024) {
        snprintf(out, max_len, "%lluB", bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        snprintf(out, max_len, "%lluK", (bytes + 1023ULL) / 1024ULL);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, max_len, "%lluM", (bytes + 1024ULL * 512ULL) / (1024ULL * 1024ULL));
    } else {
        snprintf(out, max_len, "%lluG", (bytes + 1024ULL * 1024ULL * 512ULL) / (1024ULL * 1024ULL * 1024ULL));
    }
}

static unsigned long long calculate_du(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) < 0) return 0;

    unsigned long long total_bytes = (unsigned long long)st.st_size;

    if (!S_ISDIR(st.st_mode)) {
        if (g_all_files && depth <= g_max_depth && !g_summary_only) {
            char sbuf[32];
            format_size(total_bytes, sbuf, sizeof(sbuf));
            printf("%-8s %s\n", sbuf, path);
        }
        return total_bytes;
    }

    DIR *d = opendir(path);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }

            char subpath[512];
            if (strcmp(path, "/") == 0) {
                snprintf(subpath, sizeof(subpath), "/%s", entry->d_name);
            } else {
                snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);
            }

            total_bytes += calculate_du(subpath, depth + 1);
        }
        closedir(d);
    }

    if ((!g_summary_only && depth <= g_max_depth) || (g_summary_only && depth == 0)) {
        char sbuf[32];
        format_size(total_bytes, sbuf, sizeof(sbuf));
        printf("%-8s %s\n", sbuf, path);
    }

    return total_bytes;
}

int main(int argc, char **argv)
{
    const char *paths[32];
    int path_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--human-readable") == 0) {
            g_human_readable = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--summary") == 0) {
            g_summary_only = true;
        } else if (strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "--all") == 0) {
            g_all_files = true;
        } else if (strncmp(argv[i], "-d", 2) == 0) {
            if (argv[i][2] != '\0') {
                g_max_depth = atoi(&argv[i][2]);
            } else if (i + 1 < argc) {
                g_max_depth = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("Usage: du [OPTION]... [FILE]...");
            puts("Summarize disk usage of the set of FILEs, recursively for directories.");
            puts("  -a, --all             write counts for all files, not just directories");
            puts("  -h, --human-readable  print sizes in human readable format (e.g., 1K 234M 2G)");
            puts("  -s, --summary         display only a total for each argument");
            puts("  -d N                  maximum depth of directory traversal");
            return 0;
        } else if (argv[i][0] != '-') {
            if (path_count < 32) paths[path_count++] = argv[i];
        }
    }

    if (path_count == 0) {
        paths[0] = ".";
        path_count = 1;
    }

    for (int i = 0; i < path_count; i++) {
        calculate_du(paths[i], 0);
    }

    return 0;
}
