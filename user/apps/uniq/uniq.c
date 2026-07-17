/**
 * uniq.c — Filter adjacent duplicate lines
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void process_fd(int fd) {
    char buf[1024];
    int n, lpos = 0;
    char line[512];
    char prev[512];
    prev[0] = '\0';
    int first = 1;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 511) {
                line[lpos] = '\0';
                if (first || strcmp(line, prev) != 0) {
                    printf("%s\n", line);
                    strcpy(prev, line);
                    first = 0;
                }
                lpos = 0;
            } else if (buf[i] != '\r') {
                line[lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        if (first || strcmp(line, prev) != 0) {
            printf("%s\n", line);
        }
    }
}

int main(int argc, char **argv) {
    if (argc == 1) {
        process_fd(0);
    } else {
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            printf("uniq: cannot open %s\n", argv[1]);
            return 1;
        }
        process_fd(fd);
        close(fd);
    }
    return 0;
}
