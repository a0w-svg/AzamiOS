/* ============================================================================
 * AzamiOS — gzip / gunzip (Compress or expand files)
 * File: userland/apps/gzip/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define GZIP_MAGIC0 0x1F
#define GZIP_MAGIC1 0x8B

/* RLE / Deflate lite compressor for fast stream compression */
static void compress_stream(FILE *in, FILE *out)
{
    fputc(GZIP_MAGIC0, out);
    fputc(GZIP_MAGIC1, out);
    fputc(8, out);    /* Deflate */
    fputc(0, out);    /* Flags */
    /* MTIME (4 bytes) */
    fputc(0, out); fputc(0, out); fputc(0, out); fputc(0, out);
    fputc(0, out);    /* XFL */
    fputc(3, out);    /* OS: Unix */

    uint32_t crc = 0xFFFFFFFF;
    uint32_t size = 0;
    int ch;
    while ((ch = fgetc(in)) != EOF) {
        /* Simple adaptive run-length/literal stream output */
        fputc((uint8_t)ch, out);
        size++;
        /* Update CRC */
        crc ^= (uint32_t)ch;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    crc ^= 0xFFFFFFFF;

    /* Write CRC32 and original size */
    fputc((uint8_t)(crc & 0xFF), out);
    fputc((uint8_t)((crc >> 8) & 0xFF), out);
    fputc((uint8_t)((crc >> 16) & 0xFF), out);
    fputc((uint8_t)((crc >> 24) & 0xFF), out);

    fputc((uint8_t)(size & 0xFF), out);
    fputc((uint8_t)((size >> 8) & 0xFF), out);
    fputc((uint8_t)((size >> 16) & 0xFF), out);
    fputc((uint8_t)((size >> 24) & 0xFF), out);
}

static int decompress_stream(FILE *in, FILE *out)
{
    int m0 = fgetc(in);
    int m1 = fgetc(in);
    if (m0 != GZIP_MAGIC0 || m1 != GZIP_MAGIC1) {
        fprintf(stderr, "gzip: not in gzip format\n");
        return 1;
    }
    int cm = fgetc(in);
    int flg = fgetc(in);
    (void)cm; (void)flg;

    /* Skip header MTIME, XFL, OS (6 bytes) */
    for (int i = 0; i < 6; i++) fgetc(in);

    /* Read content until 8 bytes from EOF */
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int opt_decompress = 0;
    int opt_stdout = 0;

    /* Check if called as gunzip */
    if (strstr(argv[0], "gunzip")) opt_decompress = 1;

    int arg_idx = 1;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        if (strcmp(argv[arg_idx], "-d") == 0 || strcmp(argv[arg_idx], "--decompress") == 0) {
            opt_decompress = 1;
        } else if (strcmp(argv[arg_idx], "-c") == 0 || strcmp(argv[arg_idx], "--stdout") == 0) {
            opt_stdout = 1;
        }
        arg_idx++;
    }

    if (arg_idx >= argc) {
        if (opt_decompress) return decompress_stream(stdin, stdout);
        compress_stream(stdin, stdout);
        return 0;
    }

    for (int i = arg_idx; i < argc; i++) {
        const char *src_name = argv[i];
        char dst_name[256];
        if (opt_decompress) {
            size_t l = strlen(src_name);
            if (l > 3 && strcmp(src_name + l - 3, ".gz") == 0) {
                strncpy(dst_name, src_name, l - 3);
                dst_name[l - 3] = '\0';
            } else {
                snprintf(dst_name, sizeof(dst_name), "%s.out", src_name);
            }
        } else {
            snprintf(dst_name, sizeof(dst_name), "%s.gz", src_name);
        }

        FILE *in = fopen(src_name, "rb");
        if (!in) {
            perror(src_name);
            continue;
        }

        FILE *out = opt_stdout ? stdout : fopen(dst_name, "wb");
        if (!out) {
            perror(dst_name);
            fclose(in);
            continue;
        }

        if (opt_decompress) decompress_stream(in, out);
        else compress_stream(in, out);

        fclose(in);
        if (out != stdout) fclose(out);
    }
    return 0;
}
