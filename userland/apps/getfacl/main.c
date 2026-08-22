/* ============================================================================
 * AzamiOS Userspace — Get File Access Control Lists (getfacl.elf)
 * File: userland/apps/getfacl/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/sys/acl.h"

static void print_usage(void)
{
    printf("Usage: getfacl [-c] <file> ...\n");
    printf("  -c, --omit-header    Do not display the comment header\n");
    printf("  -h, --help           Display this help\n");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }

    int omit_header = 0;
    int first_file_idx = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--omit-header") == 0) {
            omit_header = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            first_file_idx = i;
            break;
        }
    }

    int errors = 0;

    for (int i = first_file_idx; i < argc; i++) {
        if (argv[i][0] == '-') continue;

        const char *path = argv[i];
        struct stat st;
        if (stat(path, &st) < 0) {
            printf("getfacl: %s: No such file or directory\n", path);
            errors++;
            continue;
        }

        acl_t acl = acl_get_file(path, 0);
        if (!acl) {
            printf("getfacl: %s: Failed to retrieve ACL\n", path);
            errors++;
            continue;
        }

        if (!omit_header) {
            printf("# file: %s\n", path);
            printf("# owner: %s\n", (st.st_uid == 0) ? "root" : (st.st_uid == 1000) ? "azami" : "user");
            printf("# group: %s\n", (st.st_gid == 0) ? "root" : (st.st_gid == 10) ? "wheel" : "users");
        }

        ssize_t len = 0;
        char *text = acl_to_text(acl, &len);
        if (text) {
            printf("%s", text);
            free(text);
        }

        acl_free(acl);
        if (i < argc - 1) printf("\n");
    }

    return errors ? 1 : 0;
}
