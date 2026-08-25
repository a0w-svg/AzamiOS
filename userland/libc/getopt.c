/* ============================================================================
 * AzamiOS Userspace — POSIX getopt & GNU getopt_long Implementation
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
    return getopt_long(argc, argv, optstring, NULL, NULL);
}

int getopt_long(int argc, char * const argv[], const char *optstring,
                const struct option *longopts, int *longindex)
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

        /* Check for long option */
        if (arg[0] == '-' && arg[1] == '-' && longopts) {
            char *optname = arg + 2;
            char *eq = strchr(optname, '=');
            size_t namelen = eq ? (size_t)(eq - optname) : strlen(optname);

            for (int i = 0; longopts[i].name != NULL; i++) {
                if (strncmp(longopts[i].name, optname, namelen) == 0 &&
                    strlen(longopts[i].name) == namelen) {
                    if (longindex) *longindex = i;
                    optind++;

                    if (longopts[i].has_arg == required_argument) {
                        if (eq) {
                            optarg = eq + 1;
                        } else if (optind < argc) {
                            optarg = argv[optind++];
                        } else {
                            if (opterr) fprintf(stderr, "%s: option '--%s' requires an argument\n", argv[0], longopts[i].name);
                            optopt = longopts[i].val;
                            return (optstring && optstring[0] == ':') ? ':' : '?';
                        }
                    } else if (longopts[i].has_arg == optional_argument) {
                        optarg = eq ? eq + 1 : NULL;
                    } else {
                        if (eq && opterr) {
                            fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n", argv[0], longopts[i].name);
                        }
                        optarg = NULL;
                    }

                    if (longopts[i].flag) {
                        *(longopts[i].flag) = longopts[i].val;
                        return 0;
                    }
                    return longopts[i].val;
                }
            }

            if (opterr) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
            }
            optind++;
            return '?';
        }

        next_char = arg + 1;
    }

    char c = *next_char++;
    const char *spec = optstring ? strchr(optstring, c) : NULL;

    if (!spec || c == ':') {
        optopt = c;
        if (opterr && optstring && optstring[0] != ':') {
            fprintf(stderr, "%s: invalid option -- '%c'\n", argv[0], c);
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
                fprintf(stderr, "%s: option requires an argument -- '%c'\n", argv[0], c);
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

int getopt_long_only(int argc, char * const argv[], const char *optstring,
                     const struct option *longopts, int *longindex)
{
    return getopt_long(argc, argv, optstring, longopts, longindex);
}
