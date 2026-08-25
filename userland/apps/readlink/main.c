/* ============================================================================
 * AzamiOS Userspace — POSIX readlink Utility (main.c)
 * File: userland/apps/readlink/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>

static void print_help(void)
{
    printf("Usage: readlink [OPTION]... FILE...\n"
           "Print value of a symbolic link or canonical file name\n\n"
           "  -f, --canonicalize            canonicalize by following every symlink in\n"
           "                                every component of the given name recursively\n"
           "  -e, --canonicalize-existing   canonicalize by following every symlink, all\n"
           "                                components must exist\n"
           "  -m, --canonicalize-missing    canonicalize without requiring components to exist\n"
           "  -n, --no-newline              do not output the trailing delimiter\n"
           "  -q, -s, --quiet, --silent     suppress most error messages\n"
           "  -v, --verbose                 report error messages\n"
           "      --help                    display this help and exit\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int opt_canonicalize = 0;
    int opt_no_newline = 0;
    int opt_quiet = 0;
    int opt_verbose = 0;

    static struct option long_options[] = {
        {"canonicalize",          no_argument, 0, 'f'},
        {"canonicalize-existing", no_argument, 0, 'e'},
        {"canonicalize-missing",  no_argument, 0, 'm'},
        {"no-newline",            no_argument, 0, 'n'},
        {"quiet",                 no_argument, 0, 'q'},
        {"silent",                no_argument, 0, 's'},
        {"verbose",               no_argument, 0, 'v'},
        {"help",                  no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "femnsqvh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'f':
            case 'e':
            case 'm': opt_canonicalize = 1; break;
            case 'n': opt_no_newline = 1; break;
            case 'q':
            case 's': opt_quiet = 1; break;
            case 'v': opt_verbose = 1; break;
            case 'h': print_help(); return 0;
            default:
                if (!opt_quiet) fprintf(stderr, "Try 'readlink --help' for more information.\n");
                return 1;
        }
    }

    if (optind >= argc) {
        if (!opt_quiet) fprintf(stderr, "readlink: missing operand\n");
        return 1;
    }

    int ret = 0;
    for (int i = optind; i < argc; i++) {
        const char *path = argv[i];
        if (opt_canonicalize) {
            char resolved[4096] = {0};
            if (realpath(path, resolved)) {
                printf("%s%s", resolved, opt_no_newline ? "" : "\n");
            } else {
                if (opt_verbose && !opt_quiet) perror(path);
                ret = 1;
            }
        } else {
            char buf[4096] = {0};
            ssize_t len = readlink(path, buf, sizeof(buf) - 1);
            if (len >= 0) {
                buf[len] = '\0';
                printf("%s%s", buf, opt_no_newline ? "" : "\n");
            } else {
                if (opt_verbose && !opt_quiet) perror(path);
                ret = 1;
            }
        }
    }

    return ret;
}
