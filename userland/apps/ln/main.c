/* ============================================================================
 * AzamiOS Userspace — ln (Link files)
 * File: userland/apps/ln/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void print_usage(void)
{
    fprintf(stderr, "Usage: ln [-s] [-f] [-v] <target> <link_name>\n");
}

int main(int argc, char **argv)
{
    int symlink_mode = 0;
    int force = 0;
    int verbose = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-' && argv[opt_idx][1] != '\0') {
        char *arg = argv[opt_idx];
        if (strcmp(arg, "--") == 0) {
            opt_idx++;
            break;
        }
        for (int j = 1; arg[j]; j++) {
            if (arg[j] == 's') symlink_mode = 1;
            else if (arg[j] == 'f') force = 1;
            else if (arg[j] == 'v') verbose = 1;
            else {
                fprintf(stderr, "ln: invalid option -- '%c'\n", arg[j]);
                print_usage();
                return 1;
            }
        }
        opt_idx++;
    }

    if (argc - opt_idx < 2) {
        print_usage();
        return 1;
    }

    const char *target = argv[opt_idx];
    const char *link_name = argv[opt_idx + 1];

    if (force) {
        unlink(link_name);
    }

    int ret;
    if (symlink_mode) {
        ret = symlink(target, link_name);
    } else {
        ret = link(target, link_name);
    }

    if (ret < 0) {
        perror("ln");
        return 1;
    }

    if (verbose) {
        printf("'%s' -> '%s'\n", link_name, target);
    }

    return 0;
}
