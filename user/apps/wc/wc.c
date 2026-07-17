/**
 * wc.c — Word, line, and byte count utility
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

static void wc_fd(int fd, const char *name, unsigned long *tot_l, unsigned long *tot_w, unsigned long *tot_c) {
    char buf[1024];
    int n;
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') lines++;
            if (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    printf("%8lu %8lu %8lu %s\n", lines, words, bytes, name ? name : "");
    if (tot_l) *tot_l += lines;
    if (tot_w) *tot_w += words;
    if (tot_c) *tot_c += bytes;
}

int main(int argc, char **argv) {
    if (argc == 1) {
        wc_fd(0, NULL, NULL, NULL, NULL);
    } else {
        unsigned long tl = 0, tw = 0, tc = 0;
        int files = 0;
        for (int i = 1; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                printf("wc: cannot open %s\n", argv[i]);
                continue;
            }
            wc_fd(fd, argv[i], &tl, &tw, &tc);
            close(fd);
            files++;
        }
        if (files > 1) {
            printf("%8lu %8lu %8lu total\n", tl, tw, tc);
        }
    }
    return 0;
}
