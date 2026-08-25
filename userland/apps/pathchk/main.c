/* ============================================================================
 * AzamiOS Userspace — POSIX pathchk Utility (main.c)
 * File: userland/apps/pathchk/main.c
 * ============================================================================ */
#include <stdio.h>
#include <string.h>
#include <limits.h>

static int check_path(const char *path, int posix_portability)
{
    size_t len = strlen(path);
    if (posix_portability && len > 256) {
        fprintf(stderr, "pathchk: pathname length %zu exceeds POSIX limit (256)\n", len);
        return 1;
    }
    if (len > PATH_MAX) {
        fprintf(stderr, "pathchk: pathname length %zu exceeds PATH_MAX (%d)\n", len, PATH_MAX);
        return 1;
    }

    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t complen = slash ? (size_t)(slash - p) : strlen(p);
        if (posix_portability && complen > 14) {
            fprintf(stderr, "pathchk: component length %zu exceeds POSIX limit (14)\n", complen);
            return 1;
        }
        if (complen > NAME_MAX) {
            fprintf(stderr, "pathchk: component length %zu exceeds NAME_MAX (%d)\n", complen, NAME_MAX);
            return 1;
        }
        if (posix_portability) {
            for (size_t i = 0; i < complen; i++) {
                char c = p[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
                    fprintf(stderr, "pathchk: non-portable character '%c' in component\n", c);
                    return 1;
                }
            }
        }
        if (!slash) break;
        p = slash + 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int portability = 0;
    int first_idx = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) {
            portability = 1;
            first_idx = i + 1;
        } else {
            break;
        }
    }
    if (first_idx >= argc) {
        fprintf(stderr, "usage: pathchk [-p] pathname...\n");
        return 1;
    }

    int ret = 0;
    for (int i = first_idx; i < argc; i++) {
        if (check_path(argv[i], portability) != 0) ret = 1;
    }
    return ret;
}
