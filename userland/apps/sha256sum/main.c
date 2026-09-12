/* ============================================================================
 * AzamiOS — sha256sum (Compute and check SHA256 message digest)
 * File: userland/apps/sha256sum/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx_t;

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z) (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROR32(x, 2) ^ ROR32(x, 13) ^ ROR32(x, 22))
#define EP1(x) (ROR32(x, 6) ^ ROR32(x, 11) ^ ROR32(x, 25))
#define SIG0(x) (ROR32(x, 7) ^ ROR32(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROR32(x, 17) ^ ROR32(x, 19) ^ ((x) >> 10))

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#if defined(__x86_64__)
#include <immintrin.h>

static inline int sha256_cpu_has_shani(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;

    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(1), "c"(0));
    /* Need SSSE3 (ECX bit 9) and SSE4.1 (ECX bit 19) */
    if (!(ecx & (1u << 9)) || !(ecx & (1u << 19))) {
        cached = 0;
        return 0;
    }

    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(7), "c"(0));
    /* CPU_EXT_SHA is EBX bit 29 */
    cached = (ebx & (1u << 29)) ? 1 : 0;
    return cached;
}

__attribute__((target("sha,sse4.1,ssse3")))
static void sha256_transform_shani(sha256_ctx_t *ctx, const uint8_t data[64])
{
    const __m128i MASK = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);

    __m128i msg0 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 0)), MASK);
    __m128i msg1 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 16)), MASK);
    __m128i msg2 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 32)), MASK);
    __m128i msg3 = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(data + 48)), MASK);

    __m128i s0 = _mm_set_epi32((int)ctx->state[0], (int)ctx->state[1], (int)ctx->state[4], (int)ctx->state[5]);
    __m128i s1 = _mm_set_epi32((int)ctx->state[2], (int)ctx->state[3], (int)ctx->state[6], (int)ctx->state[7]);
    __m128i orig_s0 = s0;
    __m128i orig_s1 = s1;

    #define SHA256_ROUNDS_4(msg, k_idx) do { \
        __m128i k = _mm_loadu_si128((const __m128i*)(&K[k_idx])); \
        __m128i wk = _mm_add_epi32(msg, k); \
        s1 = _mm_sha256rnds2_epu32(s1, s0, wk); \
        wk = _mm_shuffle_epi32(wk, 0x0e); \
        s0 = _mm_sha256rnds2_epu32(s0, s1, wk); \
    } while (0)

    #define SHA256_MSG_UPDATE(m0, m1, m2, m3) \
        _mm_sha256msg2_epu32(_mm_add_epi32(_mm_sha256msg1_epu32(m0, m1), _mm_alignr_epi8(m3, m2, 4)), m3)

    /* Rounds 0-15 */
    SHA256_ROUNDS_4(msg0, 0);
    SHA256_ROUNDS_4(msg1, 4);
    SHA256_ROUNDS_4(msg2, 8);
    SHA256_ROUNDS_4(msg3, 12);

    /* Rounds 16-63 */
    msg0 = SHA256_MSG_UPDATE(msg0, msg1, msg2, msg3);
    SHA256_ROUNDS_4(msg0, 16);

    msg1 = SHA256_MSG_UPDATE(msg1, msg2, msg3, msg0);
    SHA256_ROUNDS_4(msg1, 20);

    msg2 = SHA256_MSG_UPDATE(msg2, msg3, msg0, msg1);
    SHA256_ROUNDS_4(msg2, 24);

    msg3 = SHA256_MSG_UPDATE(msg3, msg0, msg1, msg2);
    SHA256_ROUNDS_4(msg3, 28);

    msg0 = SHA256_MSG_UPDATE(msg0, msg1, msg2, msg3);
    SHA256_ROUNDS_4(msg0, 32);

    msg1 = SHA256_MSG_UPDATE(msg1, msg2, msg3, msg0);
    SHA256_ROUNDS_4(msg1, 36);

    msg2 = SHA256_MSG_UPDATE(msg2, msg3, msg0, msg1);
    SHA256_ROUNDS_4(msg2, 40);

    msg3 = SHA256_MSG_UPDATE(msg3, msg0, msg1, msg2);
    SHA256_ROUNDS_4(msg3, 44);

    msg0 = SHA256_MSG_UPDATE(msg0, msg1, msg2, msg3);
    SHA256_ROUNDS_4(msg0, 48);

    msg1 = SHA256_MSG_UPDATE(msg1, msg2, msg3, msg0);
    SHA256_ROUNDS_4(msg1, 52);

    msg2 = SHA256_MSG_UPDATE(msg2, msg3, msg0, msg1);
    SHA256_ROUNDS_4(msg2, 56);

    msg3 = SHA256_MSG_UPDATE(msg3, msg0, msg1, msg2);
    SHA256_ROUNDS_4(msg3, 60);

    #undef SHA256_ROUNDS_4
    #undef SHA256_MSG_UPDATE

    s0 = _mm_add_epi32(s0, orig_s0);
    s1 = _mm_add_epi32(s1, orig_s1);

    uint32_t r0[4], r1[4];
    _mm_storeu_si128((__m128i*)r0, s0);
    _mm_storeu_si128((__m128i*)r1, s1);

    ctx->state[0] = r0[3];
    ctx->state[1] = r0[2];
    ctx->state[2] = r1[3];
    ctx->state[3] = r1[2];
    ctx->state[4] = r0[1];
    ctx->state[5] = r0[0];
    ctx->state[6] = r1[1];
    ctx->state[7] = r1[0];
}
#endif

static void sha256_transform_scalar(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    for (int i = 0, j = 0; i < 16; i++, j += 4) {
        m[i] = ((uint32_t)data[j] << 24) | ((uint32_t)data[j + 1] << 16) |
               ((uint32_t)data[j + 2] << 8) | ((uint32_t)data[j + 3]);
    }
    for (int i = 16; i < 64; i++) {
        m[i] = SIG1(m[i - 2]) + m[i - 7] + SIG0(m[i - 15]) + m[i - 16];
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e, f, g) + K[i] + m[i];
        t2 = EP0(a) + MAJ(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
#if defined(__x86_64__)
    if (sha256_cpu_has_shani()) {
        sha256_transform_shani(ctx, data);
        return;
    }
#endif
    sha256_transform_scalar(ctx, data);
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
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
        sha256_transform(ctx, ctx->buffer);
        i = left;
    }
    for (; i + 63 < len; i += 64) {
        sha256_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32])
{
    uint8_t final_count[8];
    uint64_t bits = ctx->count * 8;
    for (int i = 0; i < 8; i++) {
        final_count[7 - i] = (uint8_t)(bits >> (i * 8));
    }
    size_t index = (size_t)(ctx->count & 63);
    size_t pad_len = (index < 56) ? (56 - index) : (120 - index);
    static const uint8_t padding[64] = { 0x80 };
    sha256_update(ctx, padding, pad_len);
    sha256_update(ctx, final_count, 8);

    for (int i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static int process_file(FILE *fp, const char *name)
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);

    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        sha256_update(&ctx, buf, n);
    }

    uint8_t digest[32];
    sha256_final(&ctx, digest);

    for (int i = 0; i < 32; i++) {
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
