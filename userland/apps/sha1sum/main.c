/* ============================================================================
 * AzamiOS — sha1sum (Compute and check SHA1 message digest)
 * File: userland/apps/sha1sum/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t buffer[64];
} sha1_ctx_t;

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(sha1_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3], e = ctx->state[4];
    uint32_t w[80];

    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        w[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t temp = ROL32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = ROL32(b, 30); b = a; a = temp;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

static void sha1_init(sha1_ctx_t *ctx)
{
    ctx->count = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xefcdab89;
    ctx->state[2] = 0x98badcfe;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xc3d2e1f0;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    size_t index = (size_t)(ctx->count & 63);
    ctx->count += len;

    if (index) {
        size_t left = 64 - index;
        if (len < left) {
            memcpy(&ctx->buffer[index], data, len);
            return;
        }
        memcpy(&ctx->buffer[index], data, left);
        sha1_transform(ctx, ctx->buffer);
        i = left;
    }
    for (; i + 63 < len; i += 64) {
        sha1_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t digest[20])
{
    uint8_t final_count[8];
    uint64_t bits = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        final_count[7 - i] = (uint8_t)(bits >> (i * 8));
    }
    size_t index = (size_t)(ctx->count & 63);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    static const uint8_t padding[64] = { 0x80 };
    sha1_update(ctx, padding, pad_len);
    sha1_update(ctx, final_count, 8);

    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static int process_file(FILE *fp, const char *name)
{
    sha1_ctx_t ctx;
    sha1_init(&ctx);

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        sha1_update(&ctx, buf, n);
    }

    uint8_t digest[20];
    sha1_final(&ctx, digest);

    for (int i = 0; i < 20; i++) {
        printf("%02x", digest[i]);
    }
    printf("  %s\n", name);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        return process_file(stdin, "-");
    }

    int ret = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            process_file(stdin, "-");
            continue;
        }
        FILE *fp = fopen(argv[i], "rb");
        if (!fp) {
            perror(argv[i]);
            ret = 1;
            continue;
        }
        process_file(fp, argv[i]);
        fclose(fp);
    }
    return ret;
}
