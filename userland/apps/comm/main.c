/* ============================================================================
 * AzamiOS Userspace — comm (Compare two sorted files line by line)
 * File: userland/apps/comm/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
    int show_col1 = 1;
    int show_col2 = 1;
    int show_col3 = 1;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-' && argv[opt_idx][1] != '\0') {
        char *arg = argv[opt_idx];
        if (strcmp(arg, "--") == 0) { opt_idx++; break; }
        if (strcmp(arg, "-") == 0) break;
        for (int j = 1; arg[j]; j++) {
            if (arg[j] == '1') show_col1 = 0;
            else if (arg[j] == '2') show_col2 = 0;
            else if (arg[j] == '3') show_col3 = 0;
            else {
                fprintf(stderr, "comm: invalid option -- '%c'\n", arg[j]);
                return 1;
            }
        }
        opt_idx++;
    }

    if (argc - opt_idx < 2) {
        fprintf(stderr, "Usage: comm [-123] file1 file2\n");
        return 1;
    }

    const char *f1_name = argv[opt_idx];
    const char *f2_name = argv[opt_idx + 1];

    FILE *f1 = (strcmp(f1_name, "-") == 0) ? stdin : fopen(f1_name, "r");
    if (!f1) { perror(f1_name); return 1; }

    FILE *f2 = (strcmp(f2_name, "-") == 0) ? stdin : fopen(f2_name, "r");
    if (!f2) { perror(f2_name); if (f1 != stdin) fclose(f1); return 1; }

    char *l1 = NULL, *l2 = NULL;
    size_t c1 = 0, c2 = 0;
    ssize_t n1 = getline(&l1, &c1, f1);
    ssize_t n2 = getline(&l2, &c2, f2);

    while (n1 > 0 || n2 > 0) {
        if (n1 > 0 && n2 > 0) {
            int cmp = strcmp(l1, l2);
            if (cmp == 0) {
                if (show_col3) {
                    if (show_col1) putchar('\t');
                    if (show_col2) putchar('\t');
                    fputs(l1, stdout);
                }
                n1 = getline(&l1, &c1, f1);
                n2 = getline(&l2, &c2, f2);
            } else if (cmp < 0) {
                if (show_col1) fputs(l1, stdout);
                n1 = getline(&l1, &c1, f1);
            } else {
                if (show_col2) {
                    if (show_col1) putchar('\t');
                    fputs(l2, stdout);
                }
                n2 = getline(&l2, &c2, f2);
            }
        } else if (n1 > 0) {
            if (show_col1) fputs(l1, stdout);
            n1 = getline(&l1, &c1, f1);
        } else {
            if (show_col2) {
                if (show_col1) putchar('\t');
                fputs(l2, stdout);
            }
            n2 = getline(&l2, &c2, f2);
        }
    }

    if (l1) free(l1);
    if (l2) free(l2);
    if (f1 != stdin) fclose(f1);
    if (f2 != stdin) fclose(f2);

    return 0;
}
