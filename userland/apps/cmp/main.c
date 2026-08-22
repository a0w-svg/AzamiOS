/* ============================================================================
 * AzamiOS Userspace — cmp (Compare two files byte by byte)
 * File: userland/apps/cmp/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
    int silent = 0;
    int print_chars = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-' && argv[opt_idx][1] != '\0') {
        char *arg = argv[opt_idx];
        if (strcmp(arg, "--") == 0) { opt_idx++; break; }
        if (strcmp(arg, "-") == 0) break;
        for (int j = 1; arg[j]; j++) {
            if (arg[j] == 's') silent = 1;
            else if (arg[j] == 'l') print_chars = 1;
            else {
                fprintf(stderr, "cmp: invalid option -- '%c'\n", arg[j]);
                return 2;
            }
        }
        opt_idx++;
    }

    if (argc - opt_idx < 2) {
        fprintf(stderr, "Usage: cmp [-l | -s] file1 file2\n");
        return 2;
    }

    const char *f1_name = argv[opt_idx];
    const char *f2_name = argv[opt_idx + 1];

    FILE *f1 = (strcmp(f1_name, "-") == 0) ? stdin : fopen(f1_name, "rb");
    if (!f1) { if (!silent) perror(f1_name); return 2; }

    FILE *f2 = (strcmp(f2_name, "-") == 0) ? stdin : fopen(f2_name, "rb");
    if (!f2) { if (!silent) perror(f2_name); if (f1 != stdin) fclose(f1); return 2; }

    unsigned long byte_count = 1;
    unsigned long line_count = 1;
    int diff_found = 0;

    for (;;) {
        int c1 = fgetc(f1);
        int c2 = fgetc(f2);

        if (c1 == EOF && c2 == EOF) {
            break;
        }

        if (c1 == EOF || c2 == EOF) {
            if (!silent) {
                fprintf(stderr, "cmp: EOF on %s\n", (c1 == EOF) ? f1_name : f2_name);
            }
            diff_found = 1;
            break;
        }

        if (c1 != c2) {
            diff_found = 1;
            if (!silent) {
                if (print_chars) {
                    printf("%lu %o %o\n", byte_count, (unsigned int)c1, (unsigned int)c2);
                } else {
                    printf("%s %s differ: byte %lu, line %lu\n", f1_name, f2_name, byte_count, line_count);
                    break;
                }
            } else {
                break;
            }
        }

        if (c1 == '\n') line_count++;
        byte_count++;
    }

    if (f1 != stdin) fclose(f1);
    if (f2 != stdin) fclose(f2);

    return diff_found ? 1 : 0;
}
