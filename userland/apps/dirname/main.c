/* ============================================================================
 * AzamiOS Userspace — Strip Last Component from File Name (dirname.elf)
 * File: userland/apps/dirname/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: dirname <path>");
        return 1;
    }

    const char *path = argv[1];
    const char *p = path + strlen(path);

    /* Trim trailing slashes */
    while (p > path && *(p - 1) == '/') p--;

    /* Skip filename */
    while (p > path && *(p - 1) != '/') p--;

    /* Trim trailing slashes of parent directory */
    while (p > path && *(p - 1) == '/') p--;

    if (p == path) {
        if (*path == '/') puts("/");
        else puts(".");
        return 0;
    }

    char result[256];
    size_t len = (size_t)(p - path);
    if (len >= sizeof(result)) len = sizeof(result) - 1;
    memcpy(result, path, len);
    result[len] = '\0';

    puts(result);
    return 0;
}
