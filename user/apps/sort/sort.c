/**
 * sort.c — Line sorting utility
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_LINES 512
static char s_lines[MAX_LINES][256];
static int s_count = 0;

static void read_fd(int fd) {
    char buf[1024];
    int n, lpos = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || lpos >= 255) {
                s_lines[s_count][lpos] = '\0';
                if (s_count < MAX_LINES - 1) s_count++;
                lpos = 0;
            } else if (buf[i] != '\r') {
                s_lines[s_count][lpos++] = buf[i];
            }
        }
    }
    if (lpos > 0) {
        s_lines[s_count][lpos] = '\0';
        if (s_count < MAX_LINES - 1) s_count++;
    }
}

static int cmp_line(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int main(int argc, char **argv) {
    s_count = 0;
    if (argc == 1) {
        read_fd(0);
    } else {
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd >= 0) { read_fd(fd); close(fd); }
        }
    }
    qsort(s_lines, s_count, 256, cmp_line);
    for (int i = 0; i < s_count; i++) {
        printf("%s\n", s_lines[i]);
    }
    return 0;
}
