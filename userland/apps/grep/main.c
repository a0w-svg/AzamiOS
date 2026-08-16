/* ============================================================================
 * AzamiOS Userspace — Pattern Search Utility (grep.elf)
 * File: userland/apps/grep/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/getopt.h"

static char *strcasestr_custom(const char *haystack, const char *needle)
{
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (tolower((unsigned char)*haystack) == tolower((unsigned char)*needle)) {
            const char *h = haystack, *n = needle;
            while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

static int grep_fd(int fd, const char *pattern, const char *filename,
                   bool ignore_case, bool line_num, bool invert, bool count_only,
                   bool files_with_matches, bool quiet, bool print_filename)
{
    char line_buf[2048];
    int line_len = 0;
    char c;
    int line_idx = 1;
    int matches = 0;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n') {
            line_buf[line_len] = '\0';
            bool found = false;
            if (ignore_case) {
                found = (strcasestr_custom(line_buf, pattern) != NULL);
            } else {
                found = (strstr(line_buf, pattern) != NULL);
            }

            if (invert) found = !found;

            if (found) {
                matches++;
                if (quiet) return 1;
                if (files_with_matches) {
                    printf("%s\n", filename);
                    return 1;
                }
                if (!count_only) {
                    if (print_filename) printf("%s:", filename);
                    if (line_num) printf("%d:", line_idx);
                    printf("%s\n", line_buf);
                }
            }

            line_len = 0;
            line_idx++;
        } else if (line_len < (int)sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        }
    }

    if (line_len > 0) {
        line_buf[line_len] = '\0';
        bool found = false;
        if (ignore_case) {
            found = (strcasestr_custom(line_buf, pattern) != NULL);
        } else {
            found = (strstr(line_buf, pattern) != NULL);
        }
        if (invert) found = !found;
        if (found) {
            matches++;
            if (quiet) return 1;
            if (files_with_matches) {
                printf("%s\n", filename);
                return 1;
            }
            if (!count_only) {
                if (print_filename) printf("%s:", filename);
                if (line_num) printf("%d:", line_idx);
                printf("%s\n", line_buf);
            }
        }
    }

    if (count_only && !quiet) {
        if (print_filename) printf("%s:", filename);
        printf("%d\n", matches);
    }

    return matches;
}

int main(int argc, char **argv)
{
    bool ignore_case = false;
    bool line_num = false;
    bool invert = false;
    bool count_only = false;
    bool files_with_matches = false;
    bool quiet = false;
    bool suppress_filename = false;
    const char *pattern = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "inve:clqh")) != -1) {
        switch (opt) {
        case 'i': ignore_case = true; break;
        case 'n': line_num = true; break;
        case 'v': invert = true; break;
        case 'c': count_only = true; break;
        case 'l': files_with_matches = true; break;
        case 'q': quiet = true; break;
        case 'h': suppress_filename = true; break;
        case 'e': pattern = optarg; break;
        default:
            fprintf(stderr, "Usage: grep [-invclqh] [-e pattern] [pattern] [file...]\n");
            return 2;
        }
    }

    if (!pattern) {
        if (optind >= argc) {
            fprintf(stderr, "grep: missing pattern\n");
            return 2;
        }
        pattern = argv[optind++];
    }

    if (optind >= argc) {
        int m = grep_fd(0, pattern, "(standard input)", ignore_case, line_num, invert,
                        count_only, files_with_matches, quiet, false);
        return (m > 0) ? 0 : 1;
    }

    bool print_filename = (!suppress_filename && (argc - optind > 1));
    int total_matches = 0;

    for (int i = optind; i < argc; i++) {
        const char *name = argv[i];
        int fd = 0;
        if (strcmp(name, "-") != 0) {
            fd = open(name, O_RDONLY, 0);
            if (fd < 0) {
                if (!quiet) fprintf(stderr, "grep: %s: No such file or directory\n", name);
                continue;
            }
        } else {
            name = "(standard input)";
        }

        int m = grep_fd(fd, pattern, name, ignore_case, line_num, invert,
                        count_only, files_with_matches, quiet, print_filename);
        total_matches += m;
        if (fd > 0) close(fd);
    }

    return (total_matches > 0) ? 0 : 1;
}
