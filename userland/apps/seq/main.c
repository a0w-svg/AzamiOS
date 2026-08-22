/* ============================================================================
 * AzamiOS Userspace — Print Sequence of Numbers (seq.elf)
 * File: userland/apps/seq/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"

int main(int argc, char **argv)
{
    const char *sep = "\n";
    int arg_idx = 1;

    if (argc > 2 && strcmp(argv[1], "-s") == 0) {
        sep = argv[2];
        arg_idx = 3;
    }

    int remaining = argc - arg_idx;
    if (remaining < 1) {
        puts("Usage: seq [-s separator] [FIRST [INCR]] LAST");
        return 1;
    }

    long first = 1, step = 1, last = 1;

    if (remaining == 1) {
        last = atol(argv[arg_idx]);
    } else if (remaining == 2) {
        first = atol(argv[arg_idx]);
        last = atol(argv[arg_idx + 1]);
    } else {
        first = atol(argv[arg_idx]);
        step = atol(argv[arg_idx + 1]);
        last = atol(argv[arg_idx + 2]);
    }

    if (step == 0) {
        puts("seq: zero increment step");
        return 1;
    }

    int first_print = 1;
    if (step > 0) {
        for (long i = first; i <= last; i += step) {
            if (!first_print) printf("%s", sep);
            printf("%ld", i);
            first_print = 0;
        }
    } else {
        for (long i = first; i >= last; i += step) {
            if (!first_print) printf("%s", sep);
            printf("%ld", i);
            first_print = 0;
        }
    }
    putchar('\n');
    return 0;
}
