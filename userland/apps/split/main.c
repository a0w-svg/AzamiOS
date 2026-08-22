/* ============================================================================
 * AzamiOS Userspace — split (Split a file into pieces)
 * File: userland/apps/split/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

static void make_suffix(char *s, int suffix_len, int file_idx)
{
    for (int i = suffix_len - 1; i >= 0; i--) {
        s[i] = 'a' + (file_idx % 26);
        file_idx /= 26;
    }
    s[suffix_len] = '\0';
}

int main(int argc, char **argv)
{
    unsigned long lines_per_file = 1000;
    unsigned long bytes_per_file = 0;
    const char *prefix = "x";
    const char *input_file = "-";
    int opt_idx = 1;

    while (opt_idx < argc && argv[opt_idx][0] == '-' && argv[opt_idx][1] != '\0') {
        if (strcmp(argv[opt_idx], "-l") == 0 && opt_idx + 1 < argc) {
            lines_per_file = strtoul(argv[++opt_idx], NULL, 10);
            bytes_per_file = 0;
        } else if (strncmp(argv[opt_idx], "-l", 2) == 0) {
            lines_per_file = strtoul(argv[opt_idx] + 2, NULL, 10);
            bytes_per_file = 0;
        } else if (strcmp(argv[opt_idx], "-b") == 0 && opt_idx + 1 < argc) {
            bytes_per_file = strtoul(argv[++opt_idx], NULL, 10);
            lines_per_file = 0;
        } else if (strncmp(argv[opt_idx], "-b", 2) == 0) {
            bytes_per_file = strtoul(argv[opt_idx] + 2, NULL, 10);
            lines_per_file = 0;
        } else if (strcmp(argv[opt_idx], "--") == 0) {
            opt_idx++;
            break;
        } else if (strcmp(argv[opt_idx], "-") == 0) {
            break;
        } else break;
        opt_idx++;
    }

    if (opt_idx < argc) {
        input_file = argv[opt_idx++];
    }
    if (opt_idx < argc) {
        prefix = argv[opt_idx++];
    }

    FILE *in = (strcmp(input_file, "-") == 0) ? stdin : fopen(input_file, "r");
    if (!in) {
        perror(input_file);
        return 1;
    }

    int file_idx = 0;
    char out_name[512];
    FILE *out = NULL;

    if (bytes_per_file > 0) {
        char buf[4096];
        unsigned long curr_bytes = 0;
        while (!feof(in)) {
            size_t to_read = sizeof(buf);
            if (bytes_per_file - curr_bytes < to_read) {
                to_read = bytes_per_file - curr_bytes;
            }
            size_t n = fread(buf, 1, to_read, in);
            if (n == 0) break;
            if (!out) {
                char suffix[8];
                make_suffix(suffix, 2, file_idx++);
                snprintf(out_name, sizeof(out_name), "%s%s", prefix, suffix);
                out = fopen(out_name, "w");
                if (!out) { perror(out_name); break; }
            }
            fwrite(buf, 1, n, out);
            curr_bytes += n;
            if (curr_bytes >= bytes_per_file) {
                fclose(out);
                out = NULL;
                curr_bytes = 0;
            }
        }
        if (out) fclose(out);
    } else {
        char *line = NULL;
        size_t cap = 0;
        unsigned long curr_lines = 0;
        while (getline(&line, &cap, in) > 0) {
            if (!out) {
                char suffix[8];
                make_suffix(suffix, 2, file_idx++);
                snprintf(out_name, sizeof(out_name), "%s%s", prefix, suffix);
                out = fopen(out_name, "w");
                if (!out) { perror(out_name); break; }
            }
            fputs(line, out);
            curr_lines++;
            if (curr_lines >= lines_per_file) {
                fclose(out);
                out = NULL;
                curr_lines = 0;
            }
        }
        if (line) free(line);
        if (out) fclose(out);
    }

    if (in != stdin) fclose(in);
    return 0;
}
