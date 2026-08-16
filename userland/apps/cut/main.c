/* ============================================================================
 * AzamiOS Userspace — Cut Utility (cut.elf)
 * File: userland/apps/cut/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define MAX_LINE_LEN 1024

static void cut_line_fields(char *line, char delim, int field)
{
    int current_field = 1;
    char *p = line;
    char *start = p;

    while (*p) {
        if (*p == delim) {
            if (current_field == field) {
                *p = '\0';
                printf("%s\n", start);
                return;
            }
            current_field++;
            start = p + 1;
        }
        p++;
    }

    if (current_field == field) {
        printf("%s\n", start);
    } else if (current_field == 1) {
        printf("%s\n", line);
    }
}

static void cut_line_bytes(char *line, int start_byte, int end_byte)
{
    int len = strlen(line);
    for (int i = 1; i <= len; i++) {
        if (i >= start_byte && (end_byte == 0 || i <= end_byte)) {
            putchar(line[i - 1]);
        }
    }
    putchar('\n');
}

static void process_cut_fd(int fd, char delim, int field, int start_byte, int end_byte)
{
    char buf[MAX_LINE_LEN];
    int idx = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            buf[idx] = '\0';
            idx = 0;
            if (field > 0) cut_line_fields(buf, delim, field);
            else if (start_byte > 0) cut_line_bytes(buf, start_byte, end_byte);
            else printf("%s\n", buf);
        } else if (idx < MAX_LINE_LEN - 1) {
            buf[idx++] = c;
        }
    }
    if (idx > 0) {
        buf[idx] = '\0';
        if (field > 0) cut_line_fields(buf, delim, field);
        else if (start_byte > 0) cut_line_bytes(buf, start_byte, end_byte);
        else printf("%s\n", buf);
    }
}

int main(int argc, char **argv)
{
    char delim = '\t';
    int field = 0;
    int start_byte = 0;
    int end_byte = 0;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            delim = argv[++i][0];
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            field = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            const char *range = argv[++i];
            start_byte = atoi(range);
            char *dash = strchr(range, '-');
            if (dash) end_byte = atoi(dash + 1);
            else      end_byte = start_byte;
        } else if (!file) {
            file = argv[i];
        }
    }

    int fd = 0;
    if (file && strcmp(file, "-") != 0) {
        fd = open(file, O_RDONLY, 0);
        if (fd < 0) {
            printf("cut: %s: No such file or directory\n", file);
            return 1;
        }
    }

    process_cut_fd(fd, delim, field, start_byte, end_byte);

    if (fd > 0) close(fd);
    return 0;
}
