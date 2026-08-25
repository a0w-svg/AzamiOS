/* ============================================================================
 * AzamiOS Userspace — Linux Extended Attribute Setting Tool (setfattr)
 * File: userland/apps/setfattr/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/xattr.h>

int main(int argc, char **argv)
{
    const char *attr_name = NULL;
    const char *attr_val = NULL;
    const char *remove_name = NULL;
    const char *path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            attr_name = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            attr_val = argv[++i];
        } else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            remove_name = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: setfattr {-n name [-v val] | -x name} <file>\n");
            printf("Set extended attributes of filesystem objects.\n");
            printf("  -n name    Set the value of the named extended attribute\n");
            printf("  -v val     Set the value of the extended attribute to val\n");
            printf("  -x name    Remove the named extended attribute\n");
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    if (!path || (!attr_name && !remove_name)) {
        fprintf(stderr, "setfattr: missing operand\nTry 'setfattr --help' for more information.\n");
        return 1;
    }

    if (remove_name) {
        if (removexattr(path, remove_name) < 0) {
            perror("removexattr");
            return 1;
        }
        return 0;
    }

    if (attr_name) {
        const char *val_to_set = attr_val ? attr_val : "";
        size_t vlen = strlen(val_to_set);
        if (setxattr(path, attr_name, val_to_set, vlen, 0) < 0) {
            perror("setxattr");
            return 1;
        }
    }

    return 0;
}
