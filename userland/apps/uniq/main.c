/* ============================================================================
 * AzamiOS Userspace — Uniq Utility (uniq.elf)
 * File: userland/apps/uniq/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define MAX_LINE_LEN 512

static void process_uniq_fd(int fd, int count_mode, int dup_only, int uniq_only)
{
    char prev[MAX_LINE_LEN] = "";
    char curr[MAX_LINE_LEN] = "";
    int line_count = 0;
    int first = 1;

    int idx = 0;
    char c;
    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            curr[idx] = '\0';
            idx = 0;

            if (first) {
                strcpy(prev, curr);
                line_count = 1;
                first = 0;
            } else if (strcmp(prev, curr) == 0) {
                line_count++;
            } else {
                /* Output previous line based on flags */
                int show = 1;
                if (dup_only && line_count < 2) show = 0;
                if (uniq_only && line_count > 1) show = 0;

                if (show) {
                    if (count_mode) printf("%7d %s\n", line_count, prev);
                    else            printf("%s\n", prev);
                }

                strcpy(prev, curr);
                line_count = 1;
            }
        } else if (idx < MAX_LINE_LEN - 1) {
            curr[idx++] = c;
        }
    }

    if (idx > 0) {
        curr[idx] = '\0';
        if (first) {
            strcpy(prev, curr);
            line_count = 1;
            first = 0;
        } else if (strcmp(prev, curr) == 0) {
            line_count++;
        } else {
            int show = 1;
            if (dup_only && line_count < 2) show = 0;
            if (uniq_only && line_count > 1) show = 0;
            if (show) {
                if (count_mode) printf("%7d %s\n", line_count, prev);
                else            printf("%s\n", prev);
            }
            strcpy(prev, curr);
            line_count = 1;
        }
    }

    if (!first) {
        int show = 1;
        if (dup_only && line_count < 2) show = 0;
        if (uniq_only && line_count > 1) show = 0;

        if (show) {
            if (count_mode) printf("%7d %s\n", line_count, prev);
            else            printf("%s\n", prev);
        }
    }
}

int main(int argc, char **argv)
{
    int count_mode = 0;
    int dup_only = 0;
    int uniq_only = 0;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) count_mode = 1;
        else if (strcmp(argv[i], "-d") == 0) dup_only = 1;
        else if (strcmp(argv[i], "-u") == 0) uniq_only = 1;
        else if (!file) file = argv[i];
    }

    int fd = 0;
    if (file && strcmp(file, "-") != 0) {
        fd = open(file, O_RDONLY, 0);
        if (fd < 0) {
            printf("uniq: %s: No such file or directory\n", file);
            return 1;
        }
    }

    process_uniq_fd(fd, count_mode, dup_only, uniq_only);

    if (fd > 0) close(fd);
    return 0;
}
