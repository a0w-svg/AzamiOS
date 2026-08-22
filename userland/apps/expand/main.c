/* ============================================================================
 * AzamiOS Userspace — expand (Convert tabs to spaces)
 * File: userland/apps/expand/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void expand_file(FILE *fp, int tab_size, int initial_only)
{
    int col = 0;
    int c;
    int after_leading = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n') {
            putchar('\n');
            col = 0;
            after_leading = 0;
        } else if (c == '\t' && (!initial_only || !after_leading)) {
            int spaces = tab_size - (col % tab_size);
            for (int i = 0; i < spaces; i++) putchar(' ');
            col += spaces;
        } else {
            if (c != ' ' && c != '\t') after_leading = 1;
            putchar(c);
            col++;
        }
    }
}

int main(int argc, char **argv)
{
    int tab_size = 8;
    int initial_only = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-i") == 0 || strcmp(argv[opt_idx], "--initial") == 0) {
            initial_only = 1;
        } else if (strcmp(argv[opt_idx], "-t") == 0 && opt_idx + 1 < argc) {
            tab_size = atoi(argv[++opt_idx]);
        } else if (strncmp(argv[opt_idx], "-t", 2) == 0) {
            tab_size = atoi(argv[opt_idx] + 2);
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else break;
        opt_idx++;
    }

    if (tab_size <= 0) tab_size = 8;

    if (opt_idx >= argc) {
        expand_file(stdin, tab_size, initial_only);
    } else {
        for (int i = opt_idx; i < argc; i++) {
            FILE *fp = (strcmp(argv[i], "-") == 0) ? stdin : fopen(argv[i], "r");
            if (!fp) { perror(argv[i]); continue; }
            expand_file(fp, tab_size, initial_only);
            if (fp != stdin) fclose(fp);
        }
    }

    return 0;
}
