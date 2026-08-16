/* ============================================================================
 * AzamiOS Userspace — Base64 Encode / Decode Utility (base64.elf)
 * File: userland/apps/base64/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode_fd(int fd)
{
    unsigned char in[3];
    int line_len = 0;
    ssize_t n;

    while ((n = read(fd, in, 3)) > 0) {
        char out[4];
        out[0] = b64_table[in[0] >> 2];
        out[1] = b64_table[((in[0] & 0x03) << 4) | (n > 1 ? (in[1] >> 4) : 0)];
        out[2] = (n > 1) ? b64_table[((in[1] & 0x0F) << 2) | (n > 2 ? (in[2] >> 6) : 0)] : '=';
        out[3] = (n > 2) ? b64_table[in[2] & 0x3F] : '=';

        for (int i = 0; i < 4; i++) {
            putchar(out[i]);
            line_len++;
            if (line_len >= 76) {
                putchar('\n');
                line_len = 0;
            }
        }
    }
    if (line_len > 0) putchar('\n');
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static void base64_decode_fd(int fd)
{
    int buf[4];
    int count = 0;
    char c;

    while (read(fd, &c, 1) == 1) {
        if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
        int v = b64_val(c);
        if (c == '=') v = 0;
        else if (v < 0) continue;

        buf[count++] = v;
        if (count == 4) {
            unsigned char o1 = (unsigned char)((buf[0] << 2) | (buf[1] >> 4));
            unsigned char o2 = (unsigned char)(((buf[1] & 0x0F) << 4) | (buf[2] >> 2));
            unsigned char o3 = (unsigned char)(((buf[2] & 0x03) << 6) | buf[3]);

            putchar(o1);
            if (buf[2] != 0 || c != '=') putchar(o2);
            if (buf[3] != 0 || c != '=') putchar(o3);
            count = 0;
        }
    }
}

int main(int argc, char **argv)
{
    int decode = 0;
    const char *file = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--decode") == 0) {
            decode = 1;
        } else if (!file) {
            file = argv[i];
        }
    }

    int fd = 0;
    if (file && strcmp(file, "-") != 0) {
        fd = open(file, O_RDONLY, 0);
        if (fd < 0) {
            printf("base64: %s: No such file or directory\n", file);
            return 1;
        }
    }

    if (decode) {
        base64_decode_fd(fd);
    } else {
        base64_encode_fd(fd);
    }

    if (fd > 0) close(fd);
    return 0;
}
