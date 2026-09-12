/* ============================================================================
 * AzamiOS Userspace — Linux Extended Attribute Query Tool (getfattr)
 * File: userland/apps/getfattr/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/xattr.h>

static bool match_wildcard(const char *pattern, const char *text)
{
    if (!pattern || strcmp(pattern, "-") == 0) return true;
    while (*pattern && *text) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return true;
            while (*text) {
                if (match_wildcard(pattern, text)) return true;
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

static void print_encoded(const char *val, size_t len, const char *encoding)
{
    if (encoding && strcmp(encoding, "hex") == 0) {
        printf("0x");
        for (size_t i = 0; i < len; i++) {
            printf("%02x", (unsigned char)val[i]);
        }
    } else {
        /* Default: text */
        for (size_t i = 0; i < len; i++) {
            if (val[i] == '"' || val[i] == '\\') putchar('\\');
            putchar(val[i]);
        }
    }
}

int main(int argc, char **argv)
{
    const char *attr_name = NULL;
    const char *path = NULL;
    const char *pattern = NULL;
    const char *encoding = "text";
    bool dump_all = false;
    bool only_values = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            attr_name = argv[++i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dump") == 0) {
            dump_all = true;
        } else if (strcmp(argv[i], "--only-values") == 0) {
            only_values = true;
        } else if ((strcmp(argv[i], "-e") == 0 || strcmp(argv[i], "--encoding") == 0) && i + 1 < argc) {
            encoding = argv[++i];
        } else if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--match") == 0) && i + 1 < argc) {
            pattern = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("getfattr 2.5.1 (AzamiOS Extended Attributes)\n");
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: getfattr [-d|-n name] [-e enc] [-m pat] [--only-values] <file...>\n");
            printf("Dump extended attributes of filesystem objects.\n");
            printf("  -n, --name=name         Dump the value of the named extended attribute\n");
            printf("  -d, --dump              Dump the values of all extended attributes\n");
            printf("  -e, --encoding=enc      Encode values ('text' or 'hex')\n");
            printf("  -m, --match=pattern     Only include attributes matching pattern (or '-' for all)\n");
            printf("      --only-values       Dump raw attribute value(s) only\n");
            printf("  -V, --version           Print version information and exit\n");
            printf("  -h, --help              Print this help message and exit\n");
            return 0;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "getfattr: missing operand\nTry 'getfattr --help' for more information.\n");
        return 1;
    }

    if (!only_values) {
        printf("# file: %s\n", path);
    }

    if (attr_name) {
        char val[1024] = { 0 };
        ssize_t len = getxattr(path, attr_name, val, sizeof(val) - 1);
        if (len < 0) {
            perror("getxattr");
            return 1;
        }
        val[len] = '\0';
        if (only_values) {
            print_encoded(val, (size_t)len, encoding);
            putchar('\n');
        } else {
            printf("%s=\"", attr_name);
            print_encoded(val, (size_t)len, encoding);
            printf("\"\n");
        }
    } else {
        char list[2048];
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
            if (!pattern || match_wildcard(pattern, curr_name)) {
                if (dump_all) {
                    char val[1024] = { 0 };
                    ssize_t val_len = getxattr(path, curr_name, val, sizeof(val) - 1);
                    if (val_len >= 0) {
                        val[val_len] = '\0';
                        if (only_values) {
                            print_encoded(val, (size_t)val_len, encoding);
                            putchar('\n');
                        } else {
                            printf("%s=\"", curr_name);
                            print_encoded(val, (size_t)val_len, encoding);
                            printf("\"\n");
                        }
                    } else if (!only_values) {
                        printf("%s\n", curr_name);
                    }
                } else if (!only_values) {
                    printf("%s\n", curr_name);
                }
            }
            off += strlen(curr_name) + 1;
        }
    }

    return 0;
}
