/* ============================================================================
 * AzamiOS Userspace — fold (Wrap each input line to fit specified width)
 * File: userland/apps/fold/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void fold_file(FILE *fp, int width, int break_spaces)
{
    int col = 0;
    int c;
    char word[4096];
    int word_len = 0;

    if (!break_spaces) {
        while ((c = fgetc(fp)) != EOF) {
            if (c == '\n') {
                putchar('\n');
                col = 0;
            } else if (c == '\t') {
                int next_tab = (col + 8) & ~7;
                if (next_tab >= width) {
                    putchar('\n');
                    col = 0;
                }
                putchar('\t');
                col = (col + 8) & ~7;
            } else {
                if (col >= width) {
                    putchar('\n');
                    col = 0;
                }
                putchar(c);
                col++;
            }
        }
    } else {
        while ((c = fgetc(fp)) != EOF) {
            if (c == '\n') {
                if (word_len > 0) {
                    word[word_len] = '\0';
                    fputs(word, stdout);
                    word_len = 0;
                }
                putchar('\n');
                col = 0;
            } else if (c == ' ' || c == '\t') {
                if (word_len > 0) {
                    word[word_len] = '\0';
                    fputs(word, stdout);
                    word_len = 0;
                }
                if (col >= width) {
                    putchar('\n');
                    col = 0;
                }
                putchar(c);
                col = (c == '\t') ? ((col + 8) & ~7) : (col + 1);
            } else {
                if (col + word_len >= width) {
                    if (col > 0) {
                        putchar('\n');
                        col = 0;
                    }
                }
                if (word_len < (int)sizeof(word) - 1) {
                    word[word_len++] = (char)c;
                } else {
                    word[word_len] = '\0';
                    fputs(word, stdout);
                    col += word_len;
                    word_len = 0;
                    word[word_len++] = (char)c;
                }
            }
        }
        if (word_len > 0) {
            word[word_len] = '\0';
            fputs(word, stdout);
        }
    }
}

int main(int argc, char **argv)
{
    int width = 80;
    int break_spaces = 0;
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-s") == 0) {
            break_spaces = 1;
        } else if (strcmp(argv[opt_idx], "-w") == 0 && opt_idx + 1 < argc) {
            width = atoi(argv[++opt_idx]);
        } else if (strncmp(argv[opt_idx], "-w", 2) == 0) {
            width = atoi(argv[opt_idx] + 2);
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else break;
        opt_idx++;
    }

    if (width <= 0) width = 80;

    if (opt_idx >= argc) {
        fold_file(stdin, width, break_spaces);
    } else {
        for (int i = opt_idx; i < argc; i++) {
            FILE *fp = (strcmp(argv[i], "-") == 0) ? stdin : fopen(argv[i], "r");
            if (!fp) { perror(argv[i]); continue; }
            fold_file(fp, width, break_spaces);
            if (fp != stdin) fclose(fp);
        }
    }

    return 0;
}
