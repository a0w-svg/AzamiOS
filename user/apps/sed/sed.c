/**
 * sed.c — Stream editor for AzamiOS (supports s/old/new/g)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void replace_in_line(char *line, const char *old_str, const char *new_str, int global) {
    char out[1024];
    int olen = strlen(old_str);
    if (olen == 0) { printf("%s\n", line); return; }

    char *curr = line;
    out[0] = '\0';
    int out_len = 0;

    for (;;) {
        char *hit = strstr(curr, old_str);
        if (!hit) {
            strncat(out, curr, sizeof(out) - out_len - 1);
            break;
        }
        int prefix_len = hit - curr;
        strncat(out, curr, prefix_len);
        strncat(out, new_str, sizeof(out) - strlen(out) - 1);
        out_len = strlen(out);
        curr = hit + olen;
        if (!global) {
            strncat(out, curr, sizeof(out) - out_len - 1);
            break;
        }
    }
    printf("%s\n", out);
}

static void process_fd(int fd, const char *old_str, const char *new_str, int global) {
    char buf[1024];
    int n, lpos = 0;
    char line[512];

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 511) {
                line[lpos] = '\0';
                replace_in_line(line, old_str, new_str, global);
                lpos = 0;
            } else if (buf[i] != '\r') {
                line[lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        replace_in_line(line, old_str, new_str, global);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: sed 's/old/new/[g]' [file]\n");
        return 1;
    }
    char pattern[128];
    strncpy(pattern, argv[1], sizeof(pattern) - 1);
    pattern[sizeof(pattern) - 1] = '\0';

    if (pattern[0] != 's' || pattern[1] != '/') {
        printf("sed: only s/old/new/[g] commands supported\n");
        return 1;
    }

    char *old_str = pattern + 2;
    char *p2 = strchr(old_str, '/');
    if (!p2) return 1;
    *p2 = '\0';
    char *new_str = p2 + 1;
    char *p3 = strchr(new_str, '/');
    int global = 0;
    if (p3) {
        *p3 = '\0';
        if (strchr(p3 + 1, 'g')) global = 1;
    }

    if (argc == 2) {
        process_fd(0, old_str, new_str, global);
    } else {
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) {
            printf("sed: cannot open %s\n", argv[2]);
            return 1;
        }
        process_fd(fd, old_str, new_str, global);
        close(fd);
    }
    return 0;
}
