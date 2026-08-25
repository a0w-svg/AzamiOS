/* ============================================================================
 * AzamiOS Userspace — Linux Extended Attribute Query Tool (getfattr)
 * File: userland/apps/getfattr/main.c
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
    const char *path = NULL;
    bool dump_all = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            attr_name = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dump") == 0) {
            dump_all = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: getfattr [-n name | -d] <file>\n");
            printf("Get extended attributes of filesystem objects.\n");
            printf("  -n name    Dump the value of the named extended attribute\n");
            printf("  -d         Dump the values of all extended attributes\n");
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "getfattr: missing operand\nTry 'getfattr --help' for more information.\n");
        return 1;
    }

    printf("# file: %s\n", path);

    if (attr_name) {
        char val[256] = { 0 };
        ssize_t len = getxattr(path, attr_name, val, sizeof(val) - 1);
        if (len < 0) {
            perror("getxattr");
            return 1;
        }
        val[len] = '\0';
        printf("%s=\"%s\"\n", attr_name, val);
    } else {
        char list[1024];
        ssize_t list_len = listxattr(path, list, sizeof(list));
        if (list_len < 0) {
            perror("listxattr");
            return 1;
        }
        if (list_len == 0) {
            return 0;
        }

        size_t off = 0;
        while (off < (size_t)list_len) {
            const char *curr_name = list + off;
            if (dump_all) {
                char val[256] = { 0 };
                ssize_t val_len = getxattr(path, curr_name, val, sizeof(val) - 1);
                if (val_len >= 0) {
                    val[val_len] = '\0';
                    printf("%s=\"%s\"\n", curr_name, val);
                } else {
                    printf("%s\n", curr_name);
                }
            } else {
                printf("%s\n", curr_name);
            }
            off += strlen(curr_name) + 1;
        }
    }

    return 0;
}
