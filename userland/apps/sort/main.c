/* ============================================================================
 * AzamiOS Userspace — Sort Utility (sort.elf)
 * File: userland/apps/sort/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define MAX_LINES 2048
#define MAX_LINE_LEN 512

static char *g_lines[MAX_LINES];
static int g_num_lines = 0;
static int g_reverse = 0;
static int g_numeric = 0;

static int compare_lines(const void *a, const void *b)
{
    const char *s1 = *(const char **)a;
    const char *s2 = *(const char **)b;
    int res = 0;

    if (g_numeric) {
        int n1 = atoi(s1);
        int n2 = atoi(s2);
        res = (n1 > n2) - (n1 < n2);
    } else {
        res = strcmp(s1, s2);
    }

    return g_reverse ? -res : res;
}

static void read_lines_from_fd(int fd)
{
    char buf[MAX_LINE_LEN];
    int idx = 0;
    char c;

    while (read(fd, &c, 1) == 1 && g_num_lines < MAX_LINES) {
        if (c == '\n' || c == '\r') {
            buf[idx] = '\0';
            g_lines[g_num_lines++] = strdup(buf);
            idx = 0;
        } else if (idx < MAX_LINE_LEN - 1) {
            buf[idx++] = c;
        }
    }
    if (idx > 0 && g_num_lines < MAX_LINES) {
        buf[idx] = '\0';
        g_lines[g_num_lines++] = strdup(buf);
    }
}

int main(int argc, char **argv)
{
    int file_count = 0;
    const char *files[32];

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            g_reverse = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            g_numeric = 1;
        } else if (file_count < 32) {
            files[file_count++] = argv[i];
        }
    }

    if (file_count == 0) {
        read_lines_from_fd(0);
    } else {
        for (int i = 0; i < file_count; i++) {
            int fd = open(files[i], O_RDONLY, 0);
            if (fd < 0) {
                printf("sort: %s: No such file or directory\n", files[i]);
                continue;
            }
            read_lines_from_fd(fd);
            close(fd);
        }
    }

    qsort(g_lines, g_num_lines, sizeof(char *), compare_lines);

    for (int i = 0; i < g_num_lines; i++) {
        printf("%s\n", g_lines[i]);
        free(g_lines[i]);
    }

    return 0;
}
