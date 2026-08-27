/* ============================================================================
 * AzamiOS — Kernel CSPRNG (ChaCha20, fast key erasure)
 * File: kernel/lib/random.c
 *
 * Design (mirrors the Linux get_random_bytes() core):
 *   - A 256-bit ChaCha20 key + 64-bit nonce counter is the CSPRNG state.
 *   - Each generation runs one ChaCha20 block (64 bytes). The first 32 bytes
 *     overwrite the key ("fast key erasure" → forward secrecy: past output
 *     cannot be recovered from a later state capture). The remaining 32 bytes
 *     are handed out.
 *   - Seeded from RDSEED (preferred) / RDRAND / RDTSC / address-space layout.
 *   - Hardware entropy (RDRAND) is folded into the key on every refill and a
 *     full reseed is performed periodically.
 *
 * Not a replacement for a blocking entropy estimator — there is no /dev/random
 * "wait for entropy" semantics; both device nodes and getrandom() draw from
 * this same pool (like modern Linux).
 * ============================================================================ */

#include "random.h"
#include "string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

/* ── x86 primitives ──────────────────────────────────────────────────────── */

static inline u64 rdtsc64(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static inline void cpuid_count(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static int g_has_rdrand = -1;
static int g_has_rdseed = -1;

static void hw_rng_detect(void)
{
    u32 a, b, c, d;
    cpuid_count(1, 0, &a, &b, &c, &d);
    g_has_rdrand = (c >> 30) & 1;
    cpuid_count(7, 0, &a, &b, &c, &d);
    g_has_rdseed = (b >> 18) & 1;
}

static int rdrand64(u64 *out)
{
    if (g_has_rdrand < 0) hw_rng_detect();
    if (!g_has_rdrand) return 0;
    for (int i = 0; i < 32; i++) {
        u64 v;
        u8 ok;
        __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

static int rdseed64(u64 *out)
{
    if (g_has_rdseed < 0) hw_rng_detect();
    if (!g_has_rdseed) return 0;
    for (int i = 0; i < 64; i++) {
        u64 v;
        u8 ok;
        __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* ── ChaCha20 block function (RFC 8439, 20 rounds) ───────────────────────── */

#define ROTL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define QR(x, a, b, c, d)                    \
    do {                                     \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL32(x[d], 16); \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL32(x[b], 12); \
        x[a] += x[b]; x[d] ^= x[a]; x[d] = ROTL32(x[d], 8);  \
        x[c] += x[d]; x[b] ^= x[c]; x[b] = ROTL32(x[b], 7);  \
    } while (0)

static void chacha20_block(const u32 key[8], u32 counter, const u32 nonce[3], u8 out[64])
{
    static const u32 k[4] = { 0x61707865, 0x3320646e, 0x79622d32, 0x6b206574 };
    u32 s[16], x[16];

    s[0] = k[0]; s[1] = k[1]; s[2] = k[2]; s[3] = k[3];
    for (int i = 0; i < 8; i++) s[4 + i] = key[i];
    s[12] = counter;
    s[13] = nonce[0]; s[14] = nonce[1]; s[15] = nonce[2];

    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int r = 0; r < 10; r++) {
        QR(x, 0, 4, 8, 12);  QR(x, 1, 5, 9, 13);
        QR(x, 2, 6, 10, 14); QR(x, 3, 7, 11, 15);
        QR(x, 0, 5, 10, 15); QR(x, 1, 6, 11, 12);
        QR(x, 2, 7, 8, 13);  QR(x, 3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) {
        u32 v = x[i] + s[i];
        out[i * 4 + 0] = (u8)(v);
        out[i * 4 + 1] = (u8)(v >> 8);
        out[i * 4 + 2] = (u8)(v >> 16);
        out[i * 4 + 3] = (u8)(v >> 24);
    }
}

/* ── CSPRNG state ────────────────────────────────────────────────────────── */

static spinlock_t g_rng_lock = SPINLOCK_INIT;
static u32  g_key[8];
static u64  g_nonce;
static u32  g_reseed_ctr;
static u8   g_pool[32];
static int  g_pool_pos = 32;   /* 32 == empty */
static int  g_seeded;
static u64  g_blocks_since_reseed;

#define RESEED_INTERVAL_BLOCKS 4096

static void gather_seed_locked(void)
{
    u64 w[8] = {0};
    for (int i = 0; i < 6; i++) {
        u64 v = 0;
        if (!rdseed64(&v) && !rdrand64(&v)) v = 0;
        w[i] = v ^ rdtsc64();
    }
    /* Address-space layout + timing entropy as a backstop when no HW RNG. */
    w[6] = rdtsc64() ^ (u64)(uintptr_t)&w[0] ^ ((u64)(uintptr_t)g_pool << 17);
    w[7] = rdtsc64() ^ ((u64)g_reseed_ctr << 32) ^ (u64)(uintptr_t)&g_rng_lock;

    /* Fold into the key rather than overwrite, so an existing pool is only
     * ever improved by a reseed. */
    const u32 *sw = (const u32 *)w;
    for (int i = 0; i < 8; i++) g_key[i] ^= sw[i];
    g_nonce ^= w[4] ^ (w[5] << 1);
    g_reseed_ctr++;
    g_blocks_since_reseed = 0;
}

static void refill_locked(void)
{
    if (!g_seeded) {
        static const u32 kat[8] = {
            0x9e3779b9, 0x243f6a88, 0xb7e15162, 0x8aed2a6a,
            0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f
        };
        for (int i = 0; i < 8; i++) g_key[i] = kat[i];
        g_seeded = 1;
        gather_seed_locked();
    }

    if (g_blocks_since_reseed >= RESEED_INTERVAL_BLOCKS) {
        gather_seed_locked();
    }

    u8 block[64];
    u32 nonce[3] = { (u32)g_nonce, (u32)(g_nonce >> 32), g_reseed_ctr };
    chacha20_block(g_key, 0, nonce, block);
    g_nonce++;
    g_blocks_since_reseed++;

    /* Fast key erasure. */
    memcpy(g_key, block, 32);

    /* Continuous hardware-entropy injection (only ever helps). */
    u64 hw;
    if (rdrand64(&hw)) {
        g_key[0] ^= (u32)hw;
        g_key[7] ^= (u32)(hw >> 32);
    }

    memcpy(g_pool, block + 32, 32);
    g_pool_pos = 0;
    __builtin_memset(block, 0, sizeof(block));
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void krandom_init(void)
{
    irqflags_t f = spinlock_lock_irqsave(&g_rng_lock);
    if (!g_seeded) {
        static const u32 kat[8] = {
            0x9e3779b9, 0x243f6a88, 0xb7e15162, 0x8aed2a6a,
            0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f
        };
        for (int i = 0; i < 8; i++) g_key[i] = kat[i];
        g_seeded = 1;
        gather_seed_locked();
        /* Stir: discard a few blocks so the KAT constants are gone. */
        for (int i = 0; i < 8; i++) refill_locked();
        g_pool_pos = 32;
    }
    spinlock_unlock_irqrestore(&g_rng_lock, f);
}

void krandom_bytes(void *buf, size_t n)
{
    if (!buf || n == 0) return;
    u8 *d = (u8 *)buf;

    irqflags_t f = spinlock_lock_irqsave(&g_rng_lock);
    while (n) {
        if (g_pool_pos >= 32) refill_locked();
        size_t avail = (size_t)(32 - g_pool_pos);
        size_t k = n < avail ? n : avail;
        memcpy(d, &g_pool[g_pool_pos], k);
        /* Wipe consumed pool bytes so a later state capture can't reveal them. */
        __builtin_memset(&g_pool[g_pool_pos], 0, k);
        g_pool_pos += (int)k;
        d += k;
        n -= k;
    }
    spinlock_unlock_irqrestore(&g_rng_lock, f);
}

u64 krandom_u64(void)
{
    u64 v;
    krandom_bytes(&v, sizeof(v));
    return v;
}

void krandom_add_entropy(const void *buf, size_t n)
{
    if (!buf || n == 0) return;
    const u8 *s = (const u8 *)buf;

    irqflags_t f = spinlock_lock_irqsave(&g_rng_lock);
    u8 *kb = (u8 *)g_key;
    for (size_t i = 0; i < n; i++) kb[i % 32] ^= s[i];
    g_nonce ^= rdtsc64();
    /* Force a rekey so the injected entropy propagates before the next output. */
    g_pool_pos = 32;
    refill_locked();
    spinlock_unlock_irqrestore(&g_rng_lock, f);
}
