/* ============================================================================
 * AzamiOS Userspace — Word Count Utility (wc.elf)
 * File: userland/apps/wc/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/string.h"
#include "../../libc/include/ctype.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/getopt.h"

static void wc_fd(int fd, long long *out_lines, long long *out_words, long long *out_bytes, long long *out_maxline)
{
    char buf[4096];
    ssize_t n;
    long long lines = 0, words = 0, bytes = 0, max_line = 0, cur_line = 0;
    bool in_word = false;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                lines++;
                if (cur_line > max_line) max_line = cur_line;
                cur_line = 0;
            } else {
                cur_line++;
            }
            if (isspace((unsigned char)buf[i])) {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                words++;
            }
        }
    }
    if (cur_line > max_line) max_line = cur_line;

    *out_lines = lines;
    *out_words = words;
    *out_bytes = bytes;
    *out_maxline = max_line;
}

static void print_counts(bool show_l, bool show_w, bool show_c, bool show_L,
                         long long l, long long w, long long b, long long max_l,
                         const char *name)
{
    bool first = true;
    if (show_l) {
        printf(first ? "%8lld" : " %8lld", l);
        first = false;
    }
    if (show_w) {
        printf(first ? "%8lld" : " %8lld", w);
        first = false;
    }
    if (show_c) {
        printf(first ? "%8lld" : " %8lld", b);
        first = false;
    }
    if (show_L) {
        printf(first ? "%8lld" : " %8lld", max_l);
        first = false;
    }
    if (name) {
        printf(" %s", name);
    }
    printf("\n");
}

int main(int argc, char **argv)
{
    bool show_l = false;
    bool show_w = false;
    bool show_c = false;
    bool show_L = false;

    int opt;
    while ((opt = getopt(argc, argv, "lwcmL")) != -1) {
        switch (opt) {
        case 'l': show_l = true; break;
        case 'w': show_w = true; break;
        case 'c': case 'm': show_c = true; break;
        case 'L': show_L = true; break;
        default:
            fprintf(stderr, "Usage: wc [-l] [-w] [-c] [-L] [file...]\n");
            return 1;
        }
    }

    /* Default: show lines, words, and bytes */
    if (!show_l && !show_w && !show_c && !show_L) {
        show_l = true;
        show_w = true;
        show_c = true;
    }

    if (optind >= argc) {
        long long l, w, b, ml;
        wc_fd(0, &l, &w, &b, &ml);
        print_counts(show_l, show_w, show_c, show_L, l, w, b, ml, NULL);
        return 0;
    }

    long long tot_l = 0, tot_w = 0, tot_b = 0, tot_ml = 0;
    int files_processed = 0;

    for (int i = optind; i < argc; i++) {
        int fd = 0;
        const char *name = argv[i];
        if (strcmp(name, "-") != 0) {
            fd = open(name, O_RDONLY, 0);
            if (fd < 0) {
                fprintf(stderr, "wc: %s: No such file or directory\n", name);
                continue;
            }
        } else {
            name = "-";
        }

        long long l, w, b, ml;
        wc_fd(fd, &l, &w, &b, &ml);
        if (fd > 0) close(fd);

        print_counts(show_l, show_w, show_c, show_L, l, w, b, ml, name);
        tot_l += l;
        tot_w += w;
        tot_b += b;
        if (ml > tot_ml) tot_ml = ml;
        files_processed++;
    }

    if (files_processed > 1) {
        print_counts(show_l, show_w, show_c, show_L, tot_l, tot_w, tot_b, tot_ml, "total");
    }

    return 0;
}
