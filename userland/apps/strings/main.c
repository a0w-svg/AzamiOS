/* ============================================================================
 * AzamiOS Userspace — Printable Strings Extractor (strings.elf)
 * File: userland/apps/strings/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/ctype.h"

static void process_stream(int fd, int min_len)
{
    char buf[1024];
    char str_buf[256];
    int str_len = 0;
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c >= 32 && c <= 126) {
                if (str_len < (int)sizeof(str_buf) - 1) {
                    str_buf[str_len++] = (char)c;
                }
            } else if (c == '\t') {
                if (str_len < (int)sizeof(str_buf) - 1) {
                    str_buf[str_len++] = '\t';
                }
            } else {
                if (str_len >= min_len) {
                    str_buf[str_len] = '\0';
                    puts(str_buf);
                }
                str_len = 0;
            }
        }
    }
    if (str_len >= min_len) {
        str_buf[str_len] = '\0';
        puts(str_buf);
    }
}

int main(int argc, char **argv)
{
    int min_len = 4;
    int file_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            min_len = atoi(argv[++i]);
            if (min_len <= 0) min_len = 4;
        } else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') {
            min_len = atoi(&argv[i][1]);
            if (min_len <= 0) min_len = 4;
        } else {
            file_count++;
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) {
                printf("strings: %s: No such file\n", argv[i]);
                continue;
            }
            process_stream(fd, min_len);
            close(fd);
        }
    }

    if (file_count == 0) {
        process_stream(0, min_len);
    }
    return 0;
}
