/**
 * head.c — Output first N lines of files or stdin
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void head_fd(int fd, int max_lines) {
    char buf[1024];
    int n, count = 0;
    while (count < max_lines && (n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            write(1, &buf[i], 1);
            if (buf[i] == '\n') {
                count++;
                if (count >= max_lines) break;
            }
        }
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
        head_fd(0, max_lines);
    } else {
        for (int i = first_file; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                printf("head: cannot open %s\n", argv[i]);
                continue;
            }
            if (argc - first_file > 1) printf("==> %s <==\n", argv[i]);
            head_fd(fd, max_lines);
            close(fd);
        }
    }
    return 0;
}
