/* ============================================================================
 * AzamiOS Userspace — paste (Merge lines of files)
 * File: userland/apps/paste/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *delim = "\t";
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-') {
        if (strcmp(argv[opt_idx], "-d") == 0 && opt_idx + 1 < argc) {
            delim = argv[++opt_idx];
        } else if (strncmp(argv[opt_idx], "-d", 2) == 0) {
            delim = argv[opt_idx] + 2;
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else break;
        opt_idx++;
    }

    int num_files = argc - opt_idx;
    if (num_files == 0) {
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, stdin) > 0) {
            fputs(line, stdout);
        }
        if (line) free(line);
        return 0;
    }

    FILE **fps = (FILE **)malloc(sizeof(FILE *) * num_files);
    if (!fps) return 1;

    for (int i = 0; i < num_files; i++) {
        if (strcmp(argv[opt_idx + i], "-") == 0) {
            fps[i] = stdin;
        } else {
            fps[i] = fopen(argv[opt_idx + i], "r");
            if (!fps[i]) {
                perror(argv[opt_idx + i]);
            }
        }
    }

    char **lines = (char **)calloc(num_files, sizeof(char *));
    size_t *caps = (size_t *)calloc(num_files, sizeof(size_t));

    for (;;) {
        int any_active = 0;
        for (int i = 0; i < num_files; i++) {
            if (fps[i]) {
                ssize_t len = getline(&lines[i], &caps[i], fps[i]);
                if (len > 0) {
                    any_active = 1;
                    if (lines[i][len - 1] == '\n') lines[i][len - 1] = '\0';
                } else {
                    if (fps[i] != stdin) fclose(fps[i]);
                    fps[i] = NULL;
                    if (lines[i]) lines[i][0] = '\0';
                }
            } else {
                if (lines[i]) lines[i][0] = '\0';
            }
        }

        if (!any_active) break;

        for (int i = 0; i < num_files; i++) {
            if (lines[i]) fputs(lines[i], stdout);
            if (i < num_files - 1) fputs(delim, stdout);
        }
        putchar('\n');
    }

    for (int i = 0; i < num_files; i++) {
        if (lines[i]) free(lines[i]);
        if (fps[i] && fps[i] != stdin) fclose(fps[i]);
    }
    free(fps);
    free(lines);
    free(caps);

    return 0;
}
