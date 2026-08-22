/* ============================================================================
 * AzamiOS Userspace — POSIX Translate Characters (tr.elf)
 * File: userland/apps/tr/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"

static void expand_set(const char *src, unsigned char *out, int *out_len)
{
    int len = 0;
    while (*src && len < 256) {
        if (src[1] == '-' && src[2] != '\0') {
            unsigned char start = (unsigned char)src[0];
            unsigned char end = (unsigned char)src[2];
            for (unsigned char c = start; c <= end && len < 256; c++) {
                out[len++] = c;
            }
            src += 3;
        } else {
            out[len++] = (unsigned char)*src++;
        }
    }
    *out_len = len;
}

int main(int argc, char **argv)
{
    int opt_d = 0, opt_s = 0;
    int arg_idx = 1;

    while (arg_idx < argc && argv[arg_idx][0] == '-' && argv[arg_idx][1] != '\0') {
        for (int j = 1; argv[arg_idx][j]; j++) {
            if (argv[arg_idx][j] == 'd') opt_d = 1;
            else if (argv[arg_idx][j] == 's') opt_s = 1;
        }
        arg_idx++;
    }

    if (arg_idx >= argc) {
        fprintf(stderr, "usage: tr [-ds] string1 [string2]\n");
        return 1;
    }

    unsigned char set1[256], set2[256];
    int len1 = 0, len2 = 0;

    expand_set(argv[arg_idx++], set1, &len1);
    if (arg_idx < argc) {
        expand_set(argv[arg_idx], set2, &len2);
    }

    /* Build 256-byte translation/deletion map */
    unsigned char map[256];
    int del_map[256] = { 0 };
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;

    if (opt_d) {
        for (int i = 0; i < len1; i++) del_map[set1[i]] = 1;
    } else {
        for (int i = 0; i < len1; i++) {
            map[set1[i]] = (i < len2) ? set2[i] : (len2 > 0 ? set2[len2 - 1] : set1[i]);
        }
    }

    char buf[4096];
    ssize_t n;
    int last_c = -1;

    while ((n = read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (del_map[c]) continue;
            unsigned char mapped = map[c];
            if (opt_s && last_c == mapped) continue;
            last_c = mapped;
            putchar(mapped);
        }
    }

    return 0;
}
