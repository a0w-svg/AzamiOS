/* ============================================================================
 * AzamiOS Userspace — SIMD string/memory scanners (x86_64)
 * File: userland/libc/string_simd.c
 *
 * The plain C in string.c scans eight bytes per step with a SWAR zero-test.
 * Every x86_64 CPU has SSE2 as a baseline (the ABI guarantees it), so the
 * length- and character-search primitives — the ones a shell, grep, awk and
 * the coreutils lean on hardest — can compare sixteen bytes per step with
 * PCMPEQB/PMOVMSKB instead, and thirty-two per step on a CPU that also has
 * AVX2.
 *
 * ── Why the aligned loads never fault ──────────────────────────────────────
 * An unbounded scan (strlen, strchr, rawmemchr) cannot read a fixed 16 or 32
 * bytes from an arbitrary address: the string may end one byte before an
 * unmapped page. Each scanner therefore rounds the start down to the vector
 * width and ignores the bytes before the real start. A width-aligned load
 * lies entirely inside one 4 KiB page (16 and 32 both divide 4096), so once
 * the pointer is aligned every load touches only pages the string itself
 * already occupies. Bounded scanners (memchr, memcmp, strnlen) have an
 * explicit length and just use unaligned loads for the full chunks.
 *
 * ── Tier selection ────────────────────────────────────────────────────────
 * __libc_simd_tier defaults to 0 (SSE2), which is always correct, and
 * __libc_simd_init() — called once from __libc_init() before main() — raises
 * it to 1 when CPUID reports AVX2 *and* the OS has enabled YMM state. Because
 * the default is a working path, a call that somehow lands before init still
 * produces the right answer, just without the wider vector.
 * ============================================================================ */

#include "include/string.h"
#include <stddef.h>
#include <stdint.h>
#include <emmintrin.h>
#include <immintrin.h>

int __libc_simd_tier = 0;   /* 0 = SSE2, 1 = AVX2 */

/* ── CPU probe ───────────────────────────────────────────────────────────── */

static int cpu_has_avx2(void)
{
    unsigned a, b, c, d;

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(0), "c"(0));
    if (a < 7) return 0;                 /* leaf 7 not available */

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(1), "c"(0));
    if (!(c & (1u << 27))) return 0;     /* OSXSAVE clear: XGETBV would #UD  */

    unsigned xlo, xhi;
    __asm__ volatile("xgetbv" : "=a"(xlo), "=d"(xhi) : "c"(0));
    if ((xlo & 0x6) != 0x6) return 0;    /* OS not saving SSE + YMM state    */

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(7), "c"(0));
    return (b & (1u << 5)) != 0;         /* leaf 7 EBX bit 5 = AVX2          */
}

void __libc_simd_init(void)
{
    if (cpu_has_avx2()) __libc_simd_tier = 1;
}

/* ── strlen ──────────────────────────────────────────────────────────────── */

static size_t strlen_sse2(const char *s)
{
    const __m128i z = _mm_setzero_si128();
    uintptr_t a = (uintptr_t)s;
    const char *p = (const char *)(a & ~(uintptr_t)15);
    unsigned off = (unsigned)(a & 15);

    unsigned m = (unsigned)_mm_movemask_epi8(
                     _mm_cmpeq_epi8(_mm_load_si128((const __m128i *)p), z));
    m >>= off;                                   /* drop bytes before s */
    if (m) return (size_t)__builtin_ctz(m);

    for (;;) {
        p += 16;
        m = (unsigned)_mm_movemask_epi8(
                _mm_cmpeq_epi8(_mm_load_si128((const __m128i *)p), z));
        if (m) return (size_t)(p - s) + (size_t)__builtin_ctz(m);
    }
}

__attribute__((target("avx2")))
static size_t strlen_avx2(const char *s)
{
    const __m256i z = _mm256_setzero_si256();
    uintptr_t a = (uintptr_t)s;
    const char *p = (const char *)(a & ~(uintptr_t)31);
    unsigned off = (unsigned)(a & 31);

    unsigned m = (unsigned)_mm256_movemask_epi8(
                     _mm256_cmpeq_epi8(_mm256_load_si256((const __m256i *)p), z));
    m >>= off;
    if (m) return (size_t)__builtin_ctz(m);

    for (;;) {
        p += 32;
        m = (unsigned)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(_mm256_load_si256((const __m256i *)p), z));
        if (m) return (size_t)(p - s) + (size_t)__builtin_ctz(m);
    }
}

size_t strlen(const char *s)
{
    if (!s) return 0;
    return __libc_simd_tier ? strlen_avx2(s) : strlen_sse2(s);
}

size_t strnlen(const char *s, size_t maxlen)
{
    if (!s) return 0;
    const void *n = memchr(s, 0, maxlen);
    return n ? (size_t)((const char *)n - s) : maxlen;
}

/* ── memchr (bounded) ────────────────────────────────────────────────────── */

static void *memchr_sse2(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char cc = (unsigned char)c;
    __m128i v = _mm_set1_epi8((char)cc);

    while (n >= 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(
                _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), v));
        if (m) return (void *)(p + __builtin_ctz(m));
        p += 16;
        n -= 16;
    }
    while (n--) {
        if (*p == cc) return (void *)p;
        p++;
    }
    return NULL;
}

__attribute__((target("avx2")))
static void *memchr_avx2(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char cc = (unsigned char)c;
    __m256i v = _mm256_set1_epi8((char)cc);

    while (n >= 32) {
        unsigned m = (unsigned)_mm256_movemask_epi8(
                _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), v));
        if (m) return (void *)(p + __builtin_ctz(m));
        p += 32;
        n -= 32;
    }
    while (n >= 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(
                _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p),
                               _mm256_castsi256_si128(v)));
        if (m) return (void *)(p + __builtin_ctz(m));
        p += 16;
        n -= 16;
    }
    while (n--) {
        if (*p == cc) return (void *)p;
        p++;
    }
    return NULL;
}

void *memchr(const void *s, int c, size_t n)
{
    if (!s) return NULL;
    return __libc_simd_tier ? memchr_avx2(s, c, n) : memchr_sse2(s, c, n);
}

/* ── rawmemchr (unbounded, always finds) ─────────────────────────────────── */

static void *rawmemchr_sse2(const void *s, int c)
{
    __m128i v = _mm_set1_epi8((char)(unsigned char)c);
    uintptr_t a = (uintptr_t)s;
    const unsigned char *p = (const unsigned char *)(a & ~(uintptr_t)15);
    unsigned off = (unsigned)(a & 15);

    unsigned m = (unsigned)_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_load_si128((const __m128i *)p), v));
    m >>= off;
    if (m) return (void *)((const unsigned char *)s + __builtin_ctz(m));

    for (;;) {
        p += 16;
        m = (unsigned)_mm_movemask_epi8(
                _mm_cmpeq_epi8(_mm_load_si128((const __m128i *)p), v));
        if (m) return (void *)(p + __builtin_ctz(m));
    }
}

void *rawmemchr(const void *s, int c)
{
    return rawmemchr_sse2(s, c);
}

/* ── strchr / strchrnul ─────────────────────────────────────────────────── */

static char *strchrnul_sse2(const char *s, int c)
{
    const __m128i vc = _mm_set1_epi8((char)c);
    const __m128i z  = _mm_setzero_si128();
    uintptr_t a = (uintptr_t)s;
    const char *p = (const char *)(a & ~(uintptr_t)15);
    unsigned off = (unsigned)(a & 15);

    __m128i x = _mm_load_si128((const __m128i *)p);
    unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, vc)) |
                 (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, z));
    m >>= off;
    if (m) return (char *)(s + __builtin_ctz(m));

    for (;;) {
        p += 16;
        x = _mm_load_si128((const __m128i *)p);
        m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, vc)) |
            (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, z));
        if (m) return (char *)(p + __builtin_ctz(m));
    }
}

__attribute__((target("avx2")))
static char *strchrnul_avx2(const char *s, int c)
{
    const __m256i vc = _mm256_set1_epi8((char)c);
    const __m256i z  = _mm256_setzero_si256();
    uintptr_t a = (uintptr_t)s;
    const char *p = (const char *)(a & ~(uintptr_t)31);
    unsigned off = (unsigned)(a & 31);

    __m256i x = _mm256_load_si256((const __m256i *)p);
    unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, vc)) |
                 (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, z));
    m >>= off;
    if (m) return (char *)(s + __builtin_ctz(m));

    for (;;) {
        p += 32;
        x = _mm256_load_si256((const __m256i *)p);
        m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, vc)) |
            (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, z));
        if (m) return (char *)(p + __builtin_ctz(m));
    }
}

char *strchrnul(const char *s, int c)
{
    return __libc_simd_tier ? strchrnul_avx2(s, c) : strchrnul_sse2(s, c);
}

char *strchr(const char *s, int c)
{
    if (!s) return NULL;
    char *r = __libc_simd_tier ? strchrnul_avx2(s, c) : strchrnul_sse2(s, c);
    return (*(unsigned char *)r == (unsigned char)c) ? r : NULL;
}

/* ── strrchr ────────────────────────────────────────────────────────────── */

static char *strrchr_sse2(const char *s, int c)
{
    const __m128i vc = _mm_set1_epi8((char)c);
    const __m128i z  = _mm_setzero_si128();
    uintptr_t a = (uintptr_t)s;
    const char *p = (const char *)(a & ~(uintptr_t)15);
    unsigned off = (unsigned)(a & 15);
    const char *last = NULL;

    __m128i x = _mm_load_si128((const __m128i *)p);
    unsigned cm = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, vc)) >> off;
    unsigned zm = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, z)) >> off;
    const char *base = s;

    for (;;) {
        if (zm) {
            unsigned nul = (unsigned)__builtin_ctz(zm);
            unsigned keep = (nul >= 31) ? ~0u : ((1u << (nul + 1)) - 1);
            cm &= keep;
            if (cm) last = base + (31 - __builtin_clz(cm));
            return (char *)last;
        }
        if (cm) last = base + (31 - __builtin_clz(cm));
        p += 16;
        base = p;
        x = _mm_load_si128((const __m128i *)p);
        cm = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, vc));
        zm = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, z));
    }
}

char *strrchr(const char *s, int c)
{
    if (!s) return NULL;
    return strrchr_sse2(s, c);
}

/* ── memcmp (bounded) ───────────────────────────────────────────────────── */

static int memcmp_sse2(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    while (n >= 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)p);
        __m128i y = _mm_loadu_si128((const __m128i *)q);
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, y));
        if (m != 0xffffu) {
            unsigned i = (unsigned)__builtin_ctz(~m);
            return (int)p[i] - (int)q[i];
        }
        p += 16;
        q += 16;
        n -= 16;
    }
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

__attribute__((target("avx2")))
static int memcmp_avx2(const void *a, const void *b, size_t n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    while (n >= 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)p);
        __m256i y = _mm256_loadu_si256((const __m256i *)q);
        unsigned m = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(x, y));
        if (m != 0xFFFFFFFFu) {
            unsigned i = (unsigned)__builtin_ctz(~m);
            return (int)p[i] - (int)q[i];
        }
        p += 32;
        q += 32;
        n -= 32;
    }
    while (n >= 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)p);
        __m128i y = _mm_loadu_si128((const __m128i *)q);
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, y));
        if (m != 0xffffu) {
            unsigned i = (unsigned)__builtin_ctz(~m);
            return (int)p[i] - (int)q[i];
        }
        p += 16;
        q += 16;
        n -= 16;
    }
    while (n--) {
        if (*p != *q) return (int)*p - (int)*q;
        p++;
        q++;
    }
    return 0;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    if (s1 == s2 || n == 0) return 0;
    return __libc_simd_tier ? memcmp_avx2(s1, s2, n) : memcmp_sse2(s1, s2, n);
}

int bcmp(const void *s1, const void *s2, size_t n)
{
    return memcmp(s1, s2, n);
}

/* ── memcpy (unaligned vector chunks + 64/128-byte unrolled) ─────────────── */

static void *memcpy_sse2(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    while (n >= 64) {
        __m128i v0 = _mm_loadu_si128((const __m128i *)(s + 0));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(s + 32));
        __m128i v3 = _mm_loadu_si128((const __m128i *)(s + 48));
        _mm_storeu_si128((__m128i *)(d + 0), v0);
        _mm_storeu_si128((__m128i *)(d + 16), v1);
        _mm_storeu_si128((__m128i *)(d + 32), v2);
        _mm_storeu_si128((__m128i *)(d + 48), v3);
        d += 64;
        s += 64;
        n -= 64;
    }
    while (n >= 16) {
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16;
        s += 16;
        n -= 16;
    }
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

__attribute__((target("avx2")))
static void *memcpy_avx2(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    while (n >= 128) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + 0));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + 32));
        __m256i v2 = _mm256_loadu_si256((const __m256i *)(s + 64));
        __m256i v3 = _mm256_loadu_si256((const __m256i *)(s + 96));
        _mm256_storeu_si256((__m256i *)(d + 0), v0);
        _mm256_storeu_si256((__m256i *)(d + 32), v1);
        _mm256_storeu_si256((__m256i *)(d + 64), v2);
        _mm256_storeu_si256((__m256i *)(d + 96), v3);
        d += 128;
        s += 128;
        n -= 128;
    }
    while (n >= 32) {
        _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
        d += 32;
        s += 32;
        n -= 32;
    }
    if (n >= 16) {
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        d += 16;
        s += 16;
        n -= 16;
    }
    while (n >= 8) {
        *(uint64_t *)d = *(const uint64_t *)s;
        d += 8;
        s += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    if (!dest || !src || dest == src || n == 0) return dest;
    return __libc_simd_tier ? memcpy_avx2(dest, src, n) : memcpy_sse2(dest, src, n);
}

/* ── memset (vectorized broadcast + 64/128-byte unrolled) ────────────────── */

static void *memset_sse2(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    __m128i v = _mm_set1_epi8((char)(unsigned char)c);

    while (n >= 64) {
        _mm_storeu_si128((__m128i *)(d + 0), v);
        _mm_storeu_si128((__m128i *)(d + 16), v);
        _mm_storeu_si128((__m128i *)(d + 32), v);
        _mm_storeu_si128((__m128i *)(d + 48), v);
        d += 64;
        n -= 64;
    }
    while (n >= 16) {
        _mm_storeu_si128((__m128i *)d, v);
        d += 16;
        n -= 16;
    }
    uint64_t c8 = (unsigned char)c;
    uint64_t c64 = c8 * 0x0101010101010101ULL;
    while (n >= 8) {
        *(uint64_t *)d = c64;
        d += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dest;
}

__attribute__((target("avx2")))
static void *memset_avx2(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    __m256i v = _mm256_set1_epi8((char)(unsigned char)c);

    while (n >= 128) {
        _mm256_storeu_si256((__m256i *)(d + 0), v);
        _mm256_storeu_si256((__m256i *)(d + 32), v);
        _mm256_storeu_si256((__m256i *)(d + 64), v);
        _mm256_storeu_si256((__m256i *)(d + 96), v);
        d += 128;
        n -= 128;
    }
    while (n >= 32) {
        _mm256_storeu_si256((__m256i *)d, v);
        d += 32;
        n -= 32;
    }
    if (n >= 16) {
        _mm_storeu_si128((__m128i *)d, _mm256_castsi256_si128(v));
        d += 16;
        n -= 16;
    }
    uint64_t c8 = (unsigned char)c;
    uint64_t c64 = c8 * 0x0101010101010101ULL;
    while (n >= 8) {
        *(uint64_t *)d = c64;
        d += 8;
        n -= 8;
    }
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dest;
}

void *memset(void *dest, int c, size_t n)
{
    if (!dest || n == 0) return dest;
    return __libc_simd_tier ? memset_avx2(dest, c, n) : memset_sse2(dest, c, n);
}

/* ── memmove ─────────────────────────────────────────────────────────────── */

static void *memmove_sse2(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dest;
    if (d < s || d >= s + n) {
        return memcpy_sse2(dest, src, n);
    }
    /* Overlapping backwards */
    d += n;
    s += n;
    while (n >= 64) {
        d -= 64;
        s -= 64;
        __m128i v3 = _mm_loadu_si128((const __m128i *)(s + 48));
        __m128i v2 = _mm_loadu_si128((const __m128i *)(s + 32));
        __m128i v1 = _mm_loadu_si128((const __m128i *)(s + 16));
        __m128i v0 = _mm_loadu_si128((const __m128i *)(s + 0));
        _mm_storeu_si128((__m128i *)(d + 48), v3);
        _mm_storeu_si128((__m128i *)(d + 32), v2);
        _mm_storeu_si128((__m128i *)(d + 16), v1);
        _mm_storeu_si128((__m128i *)(d + 0), v0);
        n -= 64;
    }
    while (n >= 16) {
        d -= 16;
        s -= 16;
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        n -= 16;
    }
    while (n >= 8) {
        d -= 8;
        s -= 8;
        *(uint64_t *)d = *(const uint64_t *)s;
        n -= 8;
    }
    while (n--) {
        *--d = *--s;
    }
    return dest;
}

__attribute__((target("avx2")))
static void *memmove_avx2(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dest;
    if (d < s || d >= s + n) {
        return memcpy_avx2(dest, src, n);
    }
    d += n;
    s += n;
    while (n >= 128) {
        d -= 128;
        s -= 128;
        __m256i v3 = _mm256_loadu_si256((const __m256i *)(s + 96));
        __m256i v2 = _mm256_loadu_si256((const __m256i *)(s + 64));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + 32));
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + 0));
        _mm256_storeu_si256((__m256i *)(d + 96), v3);
        _mm256_storeu_si256((__m256i *)(d + 64), v2);
        _mm256_storeu_si256((__m256i *)(d + 32), v1);
        _mm256_storeu_si256((__m256i *)(d + 0), v0);
        n -= 128;
    }
    while (n >= 32) {
        d -= 32;
        s -= 32;
        _mm256_storeu_si256((__m256i *)d, _mm256_loadu_si256((const __m256i *)s));
        n -= 32;
    }
    if (n >= 16) {
        d -= 16;
        s -= 16;
        _mm_storeu_si128((__m128i *)d, _mm_loadu_si128((const __m128i *)s));
        n -= 16;
    }
    while (n >= 8) {
        d -= 8;
        s -= 8;
        *(uint64_t *)d = *(const uint64_t *)s;
        n -= 8;
    }
    while (n--) {
        *--d = *--s;
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    if (!dest || !src || dest == src || n == 0) return dest;
    return __libc_simd_tier ? memmove_avx2(dest, src, n) : memmove_sse2(dest, src, n);
}

/* ── strcmp ──────────────────────────────────────────────────────────────── */

static int strcmp_sse2(const char *s1, const char *s2)
{
    if (s1 == s2) return 0;
    const __m128i z = _mm_setzero_si128();

    for (;;) {
        if ((((uintptr_t)s1 & 0xFFF) > 0xFF0) || (((uintptr_t)s2 & 0xFFF) > 0xFF0)) {
            unsigned char c1 = (unsigned char)*s1;
            unsigned char c2 = (unsigned char)*s2;
            if (c1 != c2 || c1 == 0) return (int)c1 - (int)c2;
            s1++;
            s2++;
            continue;
        }

        __m128i v1 = _mm_loadu_si128((const __m128i *)s1);
        __m128i v2 = _mm_loadu_si128((const __m128i *)s2);

        unsigned eq_mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v1, v2));
        unsigned z_mask  = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v1, z));
        unsigned diff_mask = (eq_mask ^ 0xFFFFu) | z_mask;

        if (diff_mask != 0) {
            int idx = __builtin_ctz(diff_mask);
            return (int)(unsigned char)s1[idx] - (int)(unsigned char)s2[idx];
        }
        s1 += 16;
        s2 += 16;
    }
}

__attribute__((target("avx2")))
static int strcmp_avx2(const char *s1, const char *s2)
{
    if (s1 == s2) return 0;
    const __m256i z = _mm256_setzero_si256();

    for (;;) {
        if ((((uintptr_t)s1 & 0xFFF) > 0xFE0) || (((uintptr_t)s2 & 0xFFF) > 0xFE0)) {
            unsigned char c1 = (unsigned char)*s1;
            unsigned char c2 = (unsigned char)*s2;
            if (c1 != c2 || c1 == 0) return (int)c1 - (int)c2;
            s1++;
            s2++;
            continue;
        }

        __m256i v1 = _mm256_loadu_si256((const __m256i *)s1);
        __m256i v2 = _mm256_loadu_si256((const __m256i *)s2);

        unsigned eq_mask = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v2));
        unsigned z_mask  = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, z));
        unsigned diff_mask = (eq_mask ^ 0xFFFFFFFFu) | z_mask;

        if (diff_mask != 0) {
            int idx = __builtin_ctz(diff_mask);
            return (int)(unsigned char)s1[idx] - (int)(unsigned char)s2[idx];
        }
        s1 += 32;
        s2 += 32;
    }
}

int strcmp(const char *s1, const char *s2)
{
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    return __libc_simd_tier ? strcmp_avx2(s1, s2) : strcmp_sse2(s1, s2);
}

/* ── strncmp ─────────────────────────────────────────────────────────────── */

static int strncmp_sse2(const char *s1, const char *s2, size_t n)
{
    if (n == 0 || s1 == s2) return 0;
    const __m128i z = _mm_setzero_si128();

    while (n > 0) {
        if (n < 16 || (((uintptr_t)s1 & 0xFFF) > 0xFF0) || (((uintptr_t)s2 & 0xFFF) > 0xFF0)) {
            unsigned char c1 = (unsigned char)*s1;
            unsigned char c2 = (unsigned char)*s2;
            if (c1 != c2 || c1 == 0) return (int)c1 - (int)c2;
            s1++;
            s2++;
            n--;
            continue;
        }

        __m128i v1 = _mm_loadu_si128((const __m128i *)s1);
        __m128i v2 = _mm_loadu_si128((const __m128i *)s2);

        unsigned eq_mask = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v1, v2));
        unsigned z_mask  = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(v1, z));
        unsigned diff_mask = (eq_mask ^ 0xFFFFu) | z_mask;

        if (diff_mask != 0) {
            size_t idx = (size_t)__builtin_ctz(diff_mask);
            if (idx >= n) return 0;
            return (int)(unsigned char)s1[idx] - (int)(unsigned char)s2[idx];
        }
        s1 += 16;
        s2 += 16;
        n -= 16;
    }
    return 0;
}

__attribute__((target("avx2")))
static int strncmp_avx2(const char *s1, const char *s2, size_t n)
{
    if (n == 0 || s1 == s2) return 0;
    const __m256i z = _mm256_setzero_si256();

    while (n > 0) {
        if (n < 32 || (((uintptr_t)s1 & 0xFFF) > 0xFE0) || (((uintptr_t)s2 & 0xFFF) > 0xFE0)) {
            unsigned char c1 = (unsigned char)*s1;
            unsigned char c2 = (unsigned char)*s2;
            if (c1 != c2 || c1 == 0) return (int)c1 - (int)c2;
            s1++;
            s2++;
            n--;
            continue;
        }

        __m256i v1 = _mm256_loadu_si256((const __m256i *)s1);
        __m256i v2 = _mm256_loadu_si256((const __m256i *)s2);

        unsigned eq_mask = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v2));
        unsigned z_mask  = (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, z));
        unsigned diff_mask = (eq_mask ^ 0xFFFFFFFFu) | z_mask;

        if (diff_mask != 0) {
            size_t idx = (size_t)__builtin_ctz(diff_mask);
            if (idx >= n) return 0;
            return (int)(unsigned char)s1[idx] - (int)(unsigned char)s2[idx];
        }
        s1 += 32;
        s2 += 32;
        n -= 32;
    }
    return 0;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) return 0;
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    return __libc_simd_tier ? strncmp_avx2(s1, s2, n) : strncmp_sse2(s1, s2, n);
}

