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

static int restore_from_dump(const char *dump_file)
{
    FILE *fp = fopen(dump_file, "r");
    if (!fp) {
        perror("setfattr: cannot open dump file");
        return 1;
    }

    char line[2048];
    char current_path[512] = { 0 };

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[--len] = '\0';
        }

        if (strncmp(line, "# file: ", 8) == 0) {
            strncpy(current_path, line + 8, sizeof(current_path) - 1);
            current_path[sizeof(current_path) - 1] = '\0';
            continue;
        }

        if (line[0] == '#' || line[0] == '\0') continue;

        if (current_path[0] != '\0') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *val = eq + 1;
                /* If quoted, unquote */
                if (*val == '"') {
                    val++;
                    size_t vlen = strlen(val);
                    if (vlen > 0 && val[vlen - 1] == '"') {
                        val[vlen - 1] = '\0';
                    }
                }
                setxattr(current_path, line, val, strlen(val), 0);
            }
        }
    }

    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    const char *attr_name = NULL;
    const char *attr_val = NULL;
    const char *remove_name = NULL;
    const char *restore_file = NULL;
    const char *paths[32];
    int path_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            attr_name = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            attr_val = argv[++i];
        } else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            remove_name = argv[++i];
        } else if (strncmp(argv[i], "--restore=", 10) == 0) {
            restore_file = argv[i] + 10;
        } else if (strcmp(argv[i], "--restore") == 0 && i + 1 < argc) {
            restore_file = argv[++i];
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("setfattr 2.5.1 (AzamiOS Extended Attributes)\n");
            return 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: setfattr {-n name [-v val] | -x name} file...\n");
            printf("       setfattr --restore=file\n");
            printf("Set extended attributes of filesystem objects.\n");
            printf("  -n, --name=name    Set the value of the named extended attribute\n");
            printf("  -v, --value=val    Set the value of the extended attribute to val\n");
            printf("  -x, --remove=name  Remove the named extended attribute\n");
            printf("      --restore=file Restore extended attributes from file dump\n");
            printf("  -V, --version      Display version information and exit\n");
            printf("  -h, --help         Display this help message and exit\n");
            return 0;
        } else if (argv[i][0] != '-') {
            if (path_count < 32) {
                paths[path_count++] = argv[i];
            }
        }
    }

    if (restore_file) {
        return restore_from_dump(restore_file);
    }

    if (path_count == 0 || (!attr_name && !remove_name)) {
        fprintf(stderr, "setfattr: missing operand\nTry 'setfattr --help' for more information.\n");
        return 1;
    }

    int ret = 0;
    for (int p = 0; p < path_count; p++) {
        const char *path = paths[p];
        if (remove_name) {
            if (removexattr(path, remove_name) < 0) {
                perror("removexattr");
                ret = 1;
            }
        } else if (attr_name) {
            const char *val_to_set = attr_val ? attr_val : "";
            size_t vlen = strlen(val_to_set);
            if (setxattr(path, attr_name, val_to_set, vlen, 0) < 0) {
                perror("setxattr");
                ret = 1;
            }
        }
    }

    return ret;
}
