/* ============================================================================
 * AzamiOS Userspace — unexpand (Convert spaces to tabs)
 * File: userland/apps/unexpand/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void unexpand_file(FILE *fp, int tab_size, int all_spaces)
{
    int c;
    int col = 0;
    int space_count = 0;
    int after_leading = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (c == ' ' && (all_spaces || !after_leading)) {
            space_count++;
            col++;
            if (col % tab_size == 0) {
                putchar('\t');
                space_count = 0;
            }
        } else {
            while (space_count > 0) {
                putchar(' ');
                space_count--;
            }
            if (c == '\n') {
                putchar('\n');
                col = 0;
                after_leading = 0;
            } else if (c == '\t') {
                putchar('\t');
                col = (col + tab_size) & ~(tab_size - 1);
            } else {
                if (c != ' ' && c != '\t') after_leading = 1;
                putchar(c);
                col++;
            }
        }
    }
    while (space_count > 0) {
        putchar(' ');
        space_count--;
    }
}

int main(int argc, char **argv)
{
    int tab_size = 8;
    int all_spaces = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-a") == 0 || strcmp(argv[opt_idx], "--all") == 0) {
            all_spaces = 1;
        } else if (strcmp(argv[opt_idx], "-t") == 0 && opt_idx + 1 < argc) {
            tab_size = atoi(argv[++opt_idx]);
            all_spaces = 1;
        } else if (strncmp(argv[opt_idx], "-t", 2) == 0) {
            tab_size = atoi(argv[opt_idx] + 2);
            all_spaces = 1;
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else break;
        opt_idx++;
    }

    if (tab_size <= 0) tab_size = 8;

    if (opt_idx >= argc) {
        unexpand_file(stdin, tab_size, all_spaces);
    } else {
        for (int i = opt_idx; i < argc; i++) {
            FILE *fp = (strcmp(argv[i], "-") == 0) ? stdin : fopen(argv[i], "r");
            if (!fp) { perror(argv[i]); continue; }
            unexpand_file(fp, tab_size, all_spaces);
            if (fp != stdin) fclose(fp);
        }
    }

    return 0;
}
