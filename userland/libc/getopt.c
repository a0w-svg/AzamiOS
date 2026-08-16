/* ============================================================================
 * AzamiOS Userspace — POSIX getopt Implementation
 * File: userland/libc/getopt.c
 * ============================================================================ */

#include "include/getopt.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"

char *optarg = NULL;
int   optind = 1;
int   opterr = 1;
int   optopt = '?';

static char *next_char = NULL;

int getopt(int argc, char * const argv[], const char *optstring)
{
    if (optind >= argc || !argv[optind]) {
        return -1;
    }

    if (!next_char || !*next_char) {
        char *arg = argv[optind];
        if (!arg || arg[0] != '-' || arg[1] == '\0') {
            return -1;
        }

        if (arg[0] == '-' && arg[1] == '-' && arg[2] == '\0') {
            optind++;
            return -1;
        }

        next_char = arg + 1;
    }

    char c = *next_char++;
    const char *spec = strchr(optstring, c);

    if (!spec || c == ':') {
        optopt = c;
        if (opterr && optstring[0] != ':') {
            fprintf(stderr, "%s: illegal option -- %c\n", argv[0], c);
        }
        if (!*next_char) {
            optind++;
            next_char = NULL;
        }
        return '?';
    }

    if (spec[1] == ':') {
        if (*next_char) {
            optarg = next_char;
            next_char = NULL;
            optind++;
        } else if (optind + 1 < argc) {
            optind++;
            optarg = argv[optind];
            optind++;
            next_char = NULL;
        } else {
            optopt = c;
            if (opterr && optstring[0] != ':') {
                fprintf(stderr, "%s: option requires an argument -- %c\n", argv[0], c);
            }
            next_char = NULL;
            optind++;
            return (optstring[0] == ':') ? ':' : '?';
        }
    } else {
        if (!*next_char) {
            optind++;
            next_char = NULL;
        }
        optarg = NULL;
    }

    return c;
}
