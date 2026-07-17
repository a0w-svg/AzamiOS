/**
 * grep.c — Pattern matching tool for AzamiOS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static void search_fd(int fd, const char *pattern, const char *fname, int show_fname) {
    char buf[1024];
    int n, line_num = 1;
    char line[512];
    int lpos = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 511) {
                line[lpos] = '\0';
                if (strstr(line, pattern) != NULL) {
                    if (show_fname) printf("%s:%d:%s\n", fname, line_num, line);
                    else printf("%s\n", line);
                }
                line_num++;
                lpos = 0;
            } else if (buf[i] != '\r') {
                line[lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        line[lpos] = '\0';
        if (strstr(line, pattern) != NULL) {
            if (show_fname) printf("%s:%d:%s\n", fname, line_num, line);
            else printf("%s\n", line);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("Usage: grep <pattern> [files...]\n");
        return 1;
    }
    const char *pattern = argv[1];
    if (argc == 2) {
        search_fd(0, pattern, "(stdin)", 0);
    } else {
        for (int i = 2; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                printf("grep: %s: No such file or directory\n", argv[i]);
                continue;
            }
            search_fd(fd, pattern, argv[i], argc > 3);
            close(fd);
        }
    }
    return 0;
}
