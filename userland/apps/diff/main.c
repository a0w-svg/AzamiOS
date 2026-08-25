/* ============================================================================
 * AzamiOS — diff (Compare files line by line)
 * File: userland/apps/diff/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_LINES 16384
#define MAX_LINE_LEN 2048

static char *lines1[MAX_LINES];
static char *lines2[MAX_LINES];
static int num_lines1 = 0;
static int num_lines2 = 0;

static int opt_unified = 1;
static int opt_brief = 0;
static int opt_ignore_case = 0;
static int opt_ignore_space = 0;

static int read_file(const char *path, char **lines, int *count)
{
    FILE *fp = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    if (!fp) {
        perror(path);
        return -1;
    }

    char buf[MAX_LINE_LEN];
    int c = 0;
    while (fgets(buf, sizeof(buf), fp) && c < MAX_LINES) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
        lines[c++] = strdup(buf);
    }
    *count = c;
    if (fp != stdin) fclose(fp);
    return 0;
}

static int line_equal(const char *s1, const char *s2)
{
    if (opt_ignore_space) {
        while (*s1 && *s2) {
            while (isspace((unsigned char)*s1)) s1++;
            while (isspace((unsigned char)*s2)) s2++;
            char c1 = opt_ignore_case ? tolower((unsigned char)*s1) : *s1;
            char c2 = opt_ignore_case ? tolower((unsigned char)*s2) : *s2;
            if (c1 != c2) return 0;
            if (*s1) s1++;
            if (*s2) s2++;
        }
        while (isspace((unsigned char)*s1)) s1++;
        while (isspace((unsigned char)*s2)) s2++;
        return (*s1 == '\0' && *s2 == '\0');
    }

    if (opt_ignore_case) {
        return (strcasecmp(s1, s2) == 0);
    }
    return (strcmp(s1, s2) == 0);
}

int main(int argc, char **argv)
{
    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-' && argv[arg_idx][1] != '\0') {
        if (strcmp(argv[arg_idx], "-u") == 0) {
            opt_unified = 1;
        } else if (strcmp(argv[arg_idx], "-q") == 0 || strcmp(argv[arg_idx], "--brief") == 0) {
            opt_brief = 1;
        } else if (strcmp(argv[arg_idx], "-i") == 0 || strcmp(argv[arg_idx], "--ignore-case") == 0) {
            opt_ignore_case = 1;
        } else if (strcmp(argv[arg_idx], "-w") == 0 || strcmp(argv[arg_idx], "--ignore-all-space") == 0) {
            opt_ignore_space = 1;
        } else if (strcmp(argv[arg_idx], "--") == 0) {
            arg_idx++;
            break;
        }
        arg_idx++;
    }

    if (argc - arg_idx < 2) {
        fprintf(stderr, "Usage: diff [-u] [-i] [-w] [-q] <file1> <file2>\n");
        return 2;
    }

    const char *file1 = argv[arg_idx];
    const char *file2 = argv[arg_idx + 1];

    if (read_file(file1, lines1, &num_lines1) < 0) return 2;
    if (read_file(file2, lines2, &num_lines2) < 0) return 2;

    int i = 0, j = 0;
    int has_diff = 0;

    /* Check difference */
    while (i < num_lines1 || j < num_lines2) {
        if (i < num_lines1 && j < num_lines2 && line_equal(lines1[i], lines2[j])) {
            i++; j++;
            continue;
        }
        has_diff = 1;
        break;
    }

    if (!has_diff) return 0;
    if (opt_brief) {
        printf("Files %s and %s differ\n", file1, file2);
        return 1;
    }

    /* Print header */
    printf("--- %s\n", file1);
    printf("+++ %s\n", file2);

    i = 0; j = 0;
    while (i < num_lines1 || j < num_lines2) {
        if (i < num_lines1 && j < num_lines2 && line_equal(lines1[i], lines2[j])) {
            printf(" %s\n", lines1[i]);
            i++; j++;
        } else if (i < num_lines1 && (j >= num_lines2 || !line_equal(lines1[i], lines2[j]))) {
            /* Check lookahead */
            int found_in_2 = -1;
            for (int k = j; k < j + 5 && k < num_lines2; k++) {
                if (line_equal(lines1[i], lines2[k])) {
                    found_in_2 = k;
                    break;
                }
            }
            if (found_in_2 >= 0) {
                while (j < found_in_2) {
                    printf("+%s\n", lines2[j++]);
                }
            } else {
                printf("-%s\n", lines1[i++]);
            }
        } else if (j < num_lines2) {
            printf("+%s\n", lines2[j++]);
        }
    }

    for (int k = 0; k < num_lines1; k++) free(lines1[k]);
    for (int k = 0; k < num_lines2; k++) free(lines2[k]);

    return 1;
}
