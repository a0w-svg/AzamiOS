/**
 * tail.c — Output last N lines of files or stdin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_TAIL_LINES 256
static char s_lines[MAX_TAIL_LINES][512];

static void tail_fd(int fd, int max_lines) {
    if (max_lines > MAX_TAIL_LINES) max_lines = MAX_TAIL_LINES;
    char buf[1024];
    int n, lpos = 0;
    int head_idx = 0, total = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 511) {
                s_lines[head_idx][lpos] = '\0';
                head_idx = (head_idx + 1) % max_lines;
                total++;
                lpos = 0;
            } else if (buf[i] != '\r') {
                s_lines[head_idx][lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        s_lines[head_idx][lpos] = '\0';
        head_idx = (head_idx + 1) % max_lines;
        total++;
    }

    int print_count = (total < max_lines) ? total : max_lines;
    int start_idx = (total < max_lines) ? 0 : head_idx;
    for (int i = 0; i < print_count; i++) {
        printf("%s\n", s_lines[(start_idx + i) % max_lines]);
    }
}

int main(int argc, char **argv) {
    int max_lines = 10;
    int first_file = 1;

    if (argc > 1 && argv[1][0] == '-') {
        if (argv[1][1] == 'n' && argc > 2) {
            max_lines = atoi(argv[2]);
            first_file = 3;
        } else {
            max_lines = atoi(&argv[1][1]);
            if (max_lines <= 0) max_lines = 10;
            first_file = 2;
        }
    }

    if (first_file >= argc) {
        tail_fd(0, max_lines);
    } else {
        for (int i = first_file; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                printf("tail: cannot open %s\n", argv[i]);
                continue;
            }
            if (argc - first_file > 1) printf("==> %s <==\n", argv[i]);
            tail_fd(fd, max_lines);
            close(fd);
        }
    }
    return 0;
}
