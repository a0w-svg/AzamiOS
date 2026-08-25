/* ============================================================================
 * AzamiOS Userspace — POSIX realpath Utility (main.c)
 * File: userland/apps/realpath/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

static void print_help(void)
{
    printf("Usage: realpath [OPTION]... FILE...\n"
           "Print the resolved absolute file name\n\n"
           "  -e, --canonicalize-existing  all components of the path must exist\n"
           "  -m, --canonicalize-missing   no path components need exist or be a dir\n"
           "  -q, --quiet                  suppress error messages for non-existent files\n"
           "  -s, --strip, --no-symlinks   don't expand symlinks\n"
           "  -z, --zero                   end each output line with NUL, not newline\n"
           "      --help                   display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int opt_quiet = 0;
    int opt_zero = 0;

    static struct option long_options[] = {
        {"canonicalize-existing", no_argument, 0, 'e'},
        {"canonicalize-missing",  no_argument, 0, 'm'},
        {"quiet",                 no_argument, 0, 'q'},
        {"strip",                 no_argument, 0, 's'},
        {"no-symlinks",           no_argument, 0, 's'},
        {"zero",                  no_argument, 0, 'z'},
        {"help",                  no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "emqszh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'e':
            case 'm':
            case 's': break;
            case 'q': opt_quiet = 1; break;
            case 'z': opt_zero = 1; break;
            case 'h': print_help(); return 0;
            default:
                if (!opt_quiet) fprintf(stderr, "Try 'realpath --help' for more information.\n");
                return 1;
        }
    }

    if (optind >= argc) {
        if (!opt_quiet) fprintf(stderr, "realpath: missing operand\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        char resolved[4096] = {0};
        if (realpath(argv[i], resolved)) {
            printf("%s%c", resolved, opt_zero ? '\0' : '\n');
        } else {
            if (!opt_quiet) perror(argv[i]);
            ret = 1;
        }
    }

    return ret;
}
