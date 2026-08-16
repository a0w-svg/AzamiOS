/* ============================================================================
 * AzamiOS Userspace — Output Last Lines (tail.elf)
 * File: userland/apps/tail/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define MAX_RING 1024

static void tail_fd(int fd, int max_lines)
{
    if (max_lines <= 0) return;
    if (max_lines > MAX_RING) max_lines = MAX_RING;

    char *lines[MAX_RING];
    int count = 0;

    for (int i = 0; i < MAX_RING; i++) lines[i] = NULL;

    char line_buf[1024];
    int line_len = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n') {
            line_buf[line_len] = '\0';
            if (lines[count % max_lines]) free(lines[count % max_lines]);
            lines[count % max_lines] = strdup(line_buf);
            count++;
            line_len = 0;
        } else if (line_len < (int)sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        }
    }
    if (line_len > 0) {
        line_buf[line_len] = '\0';
        if (lines[count % max_lines]) free(lines[count % max_lines]);
        lines[count % max_lines] = strdup(line_buf);
        count++;
    }

    int start = (count > max_lines) ? (count - max_lines) : 0;
    for (int i = start; i < count; i++) {
        char *l = lines[i % max_lines];
        if (l) {
            printf("%s\n", l);
            free(l);
            lines[i % max_lines] = NULL;
        }
    }
}

int main(int argc, char **argv)
{
    int max_lines = 10;
    int start = 1;

    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        max_lines = atoi(argv[2]);
        if (max_lines < 0) max_lines = 0;
        if (max_lines > MAX_RING) max_lines = MAX_RING;
        start = 3;
    }

    if (start >= argc) {
        tail_fd(0, max_lines);
        return 0;
    }

    for (int i = start; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("tail: cannot open '%s'\n", argv[i]);
            continue;
        }
        if (argc - start > 1) {
            printf("==> %s <==\n", argv[i]);
        }
        tail_fd(fd, max_lines);
        close(fd);
    }

    return 0;
}
