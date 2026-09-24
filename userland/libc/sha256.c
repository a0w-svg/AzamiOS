/* ============================================================================
 * AzamiOS Userspace — SHA-256 (sha256.c)
 * File: userland/libc/sha256.c
 *
 * Same algorithm userland/apps/lockscreen/main.c already implements and
 * verifies /etc/shadow entries against; factored out here so the account
 * tools that need to *write* a shadow entry (passwd.elf, useradd.elf) hash
 * against the identical implementation rather than a fourth hand-copied one.
 * ============================================================================ */

#include "include/sha256.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/unistd.h"
#include "include/sys/random.h"
#include "include/fcntl.h"

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
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

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
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

static void sha256_init_ctx(sha256_ctx_t *ctx)
{
    ctx->count = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_update_ctx(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i = 0;
    size_t buffer_idx = (size_t)((ctx->count >> 3) & 63);
    ctx->count += (uint64_t)len << 3;

    if (buffer_idx > 0) {
        size_t needed = 64 - buffer_idx;
        if (len >= needed) {
            memcpy(&ctx->buffer[buffer_idx], data, needed);
            sha256_transform(ctx, ctx->buffer);
            i += needed;
        } else {
            memcpy(&ctx->buffer[buffer_idx], data, len);
            return;
        }
    }
    for (; i + 64 <= len; i += 64) {
        sha256_transform(ctx, &data[i]);
    }
    if (i < len) {
        memcpy(ctx->buffer, &data[i], len - i);
    }
}

static void sha256_final_ctx(sha256_ctx_t *ctx, uint8_t hash[32])
{
    /* The message length SHA-256 appends is the length of the *message*,
     * not of the padded block, so it has to be captured before the padding
     * below pushes ctx->count past it. Reading ctx->count afterwards (as
     * this did) encodes 8*(len + 1 + zeros) instead of 8*len and produces a
     * digest that is self-consistent but is not SHA-256: sha256sum.elf,
     * the SHA-256 in every other implementation, and the real
     * SHA-256 constants already sitting in the /etc/shadow this image
     * ships all disagreed with it. */
    uint64_t message_bits = ctx->count;
    uint8_t pad = 0x80;
    sha256_update_ctx(ctx, &pad, 1);
    while ((ctx->count >> 3) % 64 != 56) {
        uint8_t zero = 0;
        sha256_update_ctx(ctx, &zero, 1);
    }
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = (uint8_t)(message_bits >> (56 - i * 8));
    }
    sha256_update_ctx(ctx, len_bytes, 8);
    for (int i = 0; i < 8; i++) {
        hash[i * 4 + 0] = (uint8_t)(ctx->state[i] >> 24);
        hash[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha256_hash_string(const char *str, char out_hex[65])
{
    sha256_ctx_t ctx;
    uint8_t hash[32];
    sha256_init_ctx(&ctx);
    sha256_update_ctx(&ctx, (const uint8_t *)str, strlen(str));
    sha256_final_ctx(&ctx, hash);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexd[(hash[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hexd[hash[i] & 0xF];
    }
    out_hex[64] = '\0';
}

/* Same digest, over a file instead of a string: read a chunk, fold it in,
 * repeat -- a package archive is megabytes, so it never gets slurped whole
 * into memory the way sha256_hash_string()'s argument is. Uses the raw
 * open/read syscalls rather than stdio so a caller in the middle of its own
 * FILE * bookkeeping (pkg.elf's index parsing) cannot trip over a shared
 * buffer. */
int sha256_hash_file(const char *path, char out_hex[65])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    sha256_ctx_t ctx;
    sha256_init_ctx(&ctx);

    uint8_t buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        sha256_update_ctx(&ctx, buf, (size_t)n);
    }
    close(fd);
    if (n < 0) return -1;

    uint8_t hash[32];
    sha256_final_ctx(&ctx, hash);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_hex[i * 2]     = hexd[(hash[i] >> 4) & 0xF];
        out_hex[i * 2 + 1] = hexd[hash[i] & 0xF];
    }
    out_hex[64] = '\0';
    return 0;
}

void sha256_gen_salt_hex(char *out_hex, size_t len)
{
    static const char hexd[] = "0123456789abcdef";
    size_t nbytes = (len + 1) / 2;
    uint8_t raw[64];
    if (nbytes > sizeof(raw)) nbytes = sizeof(raw);
    /* Real entropy from the kernel CSPRNG, not the PID/time/anything an
     * attacker could guess or narrow down. */
    getrandom(raw, nbytes, 0);
    size_t i;
    for (i = 0; i < len && i / 2 < nbytes; i++) {
        uint8_t byte = raw[i / 2];
        out_hex[i] = (i % 2 == 0) ? hexd[(byte >> 4) & 0xF] : hexd[byte & 0xF];
    }
    out_hex[i] = '\0';
}

void sha256_make_shadow_entry(const char *password, size_t salt_len, char *out_buf, size_t out_buf_len)
{
    char salt[64];
    if (salt_len >= sizeof(salt)) salt_len = sizeof(salt) - 1;
    sha256_gen_salt_hex(salt, salt_len);

    char salted[320];
    snprintf(salted, sizeof(salted), "%s%s", salt, password);

    char hash_hex[65];
    sha256_hash_string(salted, hash_hex);

    snprintf(out_buf, out_buf_len, "$sha256$%s$%s", salt, hash_hex);
}
