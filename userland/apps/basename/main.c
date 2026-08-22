/* ============================================================================
 * AzamiOS Userspace — Strip Directory and Suffix from Filenames (basename.elf)
 * File: userland/apps/basename/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("Usage: basename <path> [suffix]");
        return 1;
    }

    const char *path = argv[1];
    const char *p = path + strlen(path);

    /* Trim trailing slashes */
    while (p > path && *(p - 1) == '/') p--;

    const char *end = p;
    while (p > path && *(p - 1) != '/') p--;

    char result[256];
    size_t len = (size_t)(end - p);
    if (len >= sizeof(result)) len = sizeof(result) - 1;
    memcpy(result, p, len);
    result[len] = '\0';

    if (argc > 2) {
        const char *suffix = argv[2];
        size_t slen = strlen(suffix);
        if (len > slen && strcmp(result + len - slen, suffix) == 0) {
            result[len - slen] = '\0';
        }
    }

    puts(result[0] ? result : "/");
    return 0;
}
