/* ============================================================================
 * AzamiOS — Hardware-Accelerated Primitives (x86_64)
 * File: arch/x86_64/cpu/hwaccel.h
 *
 * cpu.c decides which extensions the CPU *has*; this module is where the
 * kernel's own hot paths actually spend them. Every routine here dispatches
 * once through a boot-time function pointer or a cached predicate, so a call
 * site pays no CPUID cost and no per-call branch chain.
 *
 * Only GPR-operand extensions appear here — SSE4.2's CRC32, POPCNT, BMI1/BMI2
 * (TZCNT/LZCNT/BZHI), MOVNTI, CLZERO and the WAITPKG timed pauses. The kernel
 * is built -mno-sse and does not save XMM/YMM state across its own entry
 * paths, so a routine that touched a vector register would silently corrupt
 * whichever thread it interrupted; that is why there is no AVX memcpy here,
 * and why there must not be one.
 *
 * The wider extensions the CPU may have — CLWB, CLFLUSHOPT, CLDEMOTE,
 * MOVDIR64B, SERIALIZE — are detected in cpu.c and reported through
 * /proc/cpuinfo, where userspace can act on them. They are not wrapped here,
 * because on this cache-coherent architecture the kernel has no path that
 * needs them, and a primitive with no correct caller is a liability.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* Which page-clearing strategy hwaccel_init() settled on. Reported in
 * /proc/cpuinfo's azami_hwaccel line so a bug report says which path ran. */
#define HW_CLEAR_REP_STOSB   0   /* ERMS `rep stosb`                        */
#define HW_CLEAR_MOVNTI      1   /* non-temporal GPR stores + SFENCE        */
#define HW_CLEAR_CLZERO      2   /* AMD CLZERO: whole line, no RFO          */

/** hwaccel_init() — pick an implementation for each primitive. Call once on
 *  the BSP, after cpu_detect_features() and cpu_enable_features_bsp(). */
void hwaccel_init(void);

/**
 * hwaccel_init_ap() — replay the BSP's per-core MSR state on an application
 * processor. The dispatch decisions themselves are global, but
 * IA32_UMWAIT_CONTROL is per-logical-processor: without this an AP would honour
 * a different TPAUSE residency cap from the BSP, so the same spin loop would
 * behave differently depending on which core happened to run it.
 */
void hwaccel_init_ap(void);

/** Format the chosen strategies as a single /proc/cpuinfo value line. */
size_t hwaccel_format(char *buf, size_t max);

/* ── CRC-32C (Castagnoli, the polynomial SSE4.2's CRC32 implements) ──────── */

/**
 * crc32c(crc, buf, len) — running CRC-32C over @buf.
 *
 * Start a fresh digest from 0xFFFFFFFF and complement the result, or chain
 * calls by feeding the previous return value back in. Uses the SSE4.2 CRC32
 * instruction where available (GPR operands, so no FPU state is involved) and
 * a byte-at-a-time table otherwise; both produce identical values, so a hash
 * computed on one core is valid on any other.
 */
u32 crc32c(u32 crc, const void *buf, size_t len);

/* ── Bulk page operations ────────────────────────────────────────────────── */

/**
 * hw_clear_page(va) — zero one 4 KiB page at @va (must be page-aligned).
 *
 * A freshly allocated page is written before it is read, so pulling it into
 * L1 first is pure waste: CLZERO and the non-temporal store path both skip
 * the read-for-ownership and leave the cache holding whatever was useful
 * before the allocation.
 */
void hw_clear_page(void *va);

/** hw_clear_pages(va, npages) — hw_clear_page() over a contiguous run. */
void hw_clear_pages(void *va, size_t npages);

/**
 * hw_copy_page(dst, src) — copy one 4 KiB page.
 *
 * Uses non-temporal stores when the CPU has no ERMS to make `rep movsb` fast,
 * which keeps a page copy (COW faults, exec image loads) from evicting the
 * working set of whatever thread is about to be resumed.
 */
void hw_copy_page(void *dst, const void *src);

/* ── Video Memory / Aperture Acceleration ────────────────────────────────── */

/**
 * hw_copy_to_vram(dst, src, len) — high-throughput copy to Write-Combining VRAM.
 *
 * Uses ERMS `rep movsb` or 64-bit non-temporal GPR stores (`movnti`) with `sfence`.
 * Avoids cache pollution of CPU caches while bursting data across the PCIe bus.
 */
void hw_copy_to_vram(void *dst, const void *src, size_t len);

/**
 * hw_fill_vram(dst, val, count) — high-throughput 32-bit pixel fill in VRAM.
 *
 * Fills @count 32-bit pixels with @val using 64-bit packed stores.
 */
void hw_fill_vram(void *dst, u32 val, size_t count);

/* ── Bit scanning & manipulation ─────────────────────────────────────────── */

/* Set by hwaccel_init() when CPUID reports POPCNT. Read directly by the inline
 * below so a bitmap loop pays one predictable branch, not a call. */
extern u8 g_popcnt_enabled;
extern u8 g_lzcnt_enabled;
extern u8 g_bmi1_enabled;
extern u8 g_bmi2_enabled;
extern u8 g_clflushopt_enabled;
extern u8 g_clwb_enabled;
extern u8 g_serialize_enabled;
extern u8 g_rdrand_enabled;
extern u8 g_rdseed_enabled;

/**
 * hw_popcnt64(word) — count set bits.
 *
 * POPCNT where the CPU has it, and a SWAR reduction otherwise. Deliberately
 * *not* __builtin_popcountll(): the kernel is compiled for a baseline without
 * POPCNT, so GCC lowers that builtin to a call into libgcc — which a
 * freestanding kernel does not link, and which would be a function call per
 * bitmap word even if it did.
 */
static inline unsigned hw_popcnt64(u64 word)
{
    if (g_popcnt_enabled) {
        u64 r;
        __asm__("popcntq %1, %0" : "=r"(r) : "rm"(word) : "cc");
        return (unsigned)r;
    }
    /* Classic SWAR: pairwise sums, then nibbles, then a multiply that
     * accumulates all eight byte totals into the top byte. */
    word = word - ((word >> 1) & 0x5555555555555555ULL);
    word = (word & 0x3333333333333333ULL) + ((word >> 2) & 0x3333333333333333ULL);
    word = (word + (word >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (unsigned)((word * 0x0101010101010101ULL) >> 56);
}

static inline unsigned hw_popcnt32(u32 word)
{
    if (g_popcnt_enabled) {
        u32 r;
        __asm__("popcntl %1, %0" : "=r"(r) : "rm"(word) : "cc");
        return (unsigned)r;
    }
    word = word - ((word >> 1) & 0x55555555U);
    word = (word & 0x33333333U) + ((word >> 2) & 0x33333333U);
    word = (word + (word >> 4)) & 0x0F0F0F0FU;
    return (unsigned)((word * 0x01010101U) >> 24);
}

/**
 * hw_ctz64(word) — count trailing zero bits; 64 for word == 0.
 *
 * Emits TZCNT unconditionally, with no CPUID check needed at all: on a CPU
 * without BMI1, TZCNT's F3 0F BC encoding just decodes as plain BSF (ignoring
 * the REP prefix it's built from), and BSF's result for a *nonzero* input is
 * numerically identical to TZCNT's — both are "the position of the lowest set
 * bit". The only place they disagree is a zero input, which this handles in
 * software before the asm ever runs, so the BSF fallback is always correct,
 * not just usually.
 */
static inline unsigned hw_ctz64(u64 word)
{
    if (word == 0) return 64;
    u64 r;
    __asm__("tzcntq %1, %0" : "=r"(r) : "rm"(word) : "cc");
    return (unsigned)r;
}

static inline unsigned hw_ctz32(u32 word)
{
    if (word == 0) return 32;
    u32 r;
    __asm__("tzcntl %1, %0" : "=r"(r) : "rm"(word) : "cc");
    return (unsigned)r;
}

/**
 * hw_clz64(word) — count leading zero bits; 64 for word == 0.
 *
 * Unlike TZCNT/BSF, this genuinely needs the CPUID check: LZCNT's fallback
 * decode on a CPU without it is BSR, and BSR returns the *bit index* of the
 * highest set bit — a different number from "count of leading zeros", not
 * the same value under a different name (they're related by `63 - index`,
 * but the raw registers disagree for every nonzero input, not just zero).
 */
static inline unsigned hw_clz64(u64 word)
{
    if (word == 0) return 64;
    u64 r;
    if (g_lzcnt_enabled) {
        __asm__("lzcntq %1, %0" : "=r"(r) : "rm"(word) : "cc");
        return (unsigned)r;
    }
    __asm__("bsrq %1, %0" : "=r"(r) : "rm"(word) : "cc");
    return (unsigned)(63 - r);
}

static inline unsigned hw_clz32(u32 word)
{
    if (word == 0) return 32;
    u32 r;
    if (g_lzcnt_enabled) {
        __asm__("lzcntl %1, %0" : "=r"(r) : "rm"(word) : "cc");
        return (unsigned)r;
    }
    __asm__("bsrl %1, %0" : "=r"(r) : "rm"(word) : "cc");
    return (unsigned)(31 - r);
}

/**
 * hw_bzhi64(src, n) — the low @n bits of @src, all others cleared
 * (n >= 64 returns @src unchanged, matching BZHI's own out-of-range rule).
 */
static inline u64 hw_bzhi64(u64 src, u32 n)
{
    if (g_bmi2_enabled) {
        u64 r;
        __asm__("bzhiq %2, %1, %0" : "=r"(r) : "rm"(src), "r"((u64)n) : "cc");
        return r;
    }
    return (n >= 64) ? src : (src & ((1ULL << n) - 1));
}

static inline u32 hw_bzhi32(u32 src, u32 n)
{
    if (g_bmi2_enabled) {
        u32 r;
        __asm__("bzhil %2, %1, %0" : "=r"(r) : "rm"(src), "r"(n) : "cc");
        return r;
    }
    return (n >= 32) ? src : (src & ((1U << n) - 1));
}

/**
 * hw_bextr64(src, start, len) — extract bitfield [start, start + len)
 */
static inline u64 hw_bextr64(u64 src, u32 start, u32 len)
{
    if (g_bmi1_enabled) {
        u64 control = ((u64)(start & 0xFF)) | (((u64)(len & 0xFF)) << 8);
        u64 r;
        __asm__("bextrq %2, %1, %0" : "=r"(r) : "rm"(src), "r"(control) : "cc");
        return r;
    }
    if (start >= 64 || len == 0) return 0;
    u64 shifted = src >> start;
    if (len >= 64) return shifted;
    return shifted & ((1ULL << len) - 1);
}

static inline u32 hw_bextr32(u32 src, u32 start, u32 len)
{
    if (g_bmi1_enabled) {
        u32 control = (start & 0xFF) | ((len & 0xFF) << 8);
        u32 r;
        __asm__("bextrl %2, %1, %0" : "=r"(r) : "rm"(src), "r"(control) : "cc");
        return r;
    }
    if (start >= 32 || len == 0) return 0;
    u32 shifted = src >> start;
    if (len >= 32) return shifted;
    return shifted & ((1U << len) - 1);
}

/**
 * hw_blsr64(src) — clear lowest set bit: src & (src - 1)
 */
static inline u64 hw_blsr64(u64 src)
{
    if (g_bmi1_enabled) {
        u64 r;
        __asm__("blsrq %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src & (src - 1);
}

static inline u32 hw_blsr32(u32 src)
{
    if (g_bmi1_enabled) {
        u32 r;
        __asm__("blsrl %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src & (src - 1);
}

/**
 * hw_blsi64(src) — extract lowest set bit: src & (-src)
 */
static inline u64 hw_blsi64(u64 src)
{
    if (g_bmi1_enabled) {
        u64 r;
        __asm__("blsiq %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src & (-(int64_t)src);
}

static inline u32 hw_blsi32(u32 src)
{
    if (g_bmi1_enabled) {
        u32 r;
        __asm__("blsil %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src & (-(int32_t)src);
}

/**
 * hw_blsmsk64(src) — get mask up to lowest set bit: src ^ (src - 1)
 */
static inline u64 hw_blsmsk64(u64 src)
{
    if (g_bmi1_enabled) {
        u64 r;
        __asm__("blsmskq %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src ^ (src - 1);
}

static inline u32 hw_blsmsk32(u32 src)
{
    if (g_bmi1_enabled) {
        u32 r;
        __asm__("blsmskl %1, %0" : "=r"(r) : "rm"(src) : "cc");
        return r;
    }
    return src ^ (src - 1);
}

/**
 * hw_andn64(a, b) — bitwise NOT of @a AND @b: (~a) & b
 */
static inline u64 hw_andn64(u64 a, u64 b)
{
    if (g_bmi1_enabled) {
        u64 r;
        __asm__("andnq %1, %2, %0" : "=r"(r) : "rm"(b), "r"(a) : "cc");
        return r;
    }
    return (~a) & b;
}

static inline u32 hw_andn32(u32 a, u32 b)
{
    if (g_bmi1_enabled) {
        u32 r;
        __asm__("andnl %1, %2, %0" : "=r"(r) : "rm"(b), "r"(a) : "cc");
        return r;
    }
    return (~a) & b;
}

/**
 * hw_mulx64(a, b, hi) — unsigned 64x64 -> 128 multiply using BMI2 MULX
 * does not affect flags; returns low 64 bits and stores high 64 bits in @hi.
 */
static inline u64 hw_mulx64(u64 a, u64 b, u64 *hi)
{
    if (g_bmi2_enabled) {
        u64 lo, h;
        __asm__("mulxq %2, %0, %1" : "=r"(lo), "=r"(h) : "rm"(b), "d"(a));
        if (hi) *hi = h;
        return lo;
    }
    __uint128_t prod = (__uint128_t)a * (__uint128_t)b;
    if (hi) *hi = (u64)(prod >> 64);
    return (u64)prod;
}

/**
 * hw_rorx64(src, count) — rotate right without modifying condition codes.
 */
#define hw_rorx64(src, imm) \
    (__builtin_constant_p(imm) && ((imm) < 64) ? ({ \
        u64 _r; \
        if (g_bmi2_enabled) { \
            __asm__("rorxq %1, %2, %0" : "=r"(_r) : "i"(imm), "rm"(src)); \
        } else { \
            _r = ((u64)(src) >> (imm)) | ((u64)(src) << ((64 - (imm)) & 63)); \
        } \
        _r; \
    }) : ({ \
        unsigned _c = (imm) & 63; \
        u64 _s = (src); \
        (_c == 0) ? _s : ((_s >> _c) | (_s << (64 - _c))); \
    }))

/* ── Cache management & Memory ordering ───────────────────────────────────── */

static inline void hw_sfence(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

static inline void hw_lfence(void)
{
    __asm__ volatile("lfence" ::: "memory");
}

static inline void hw_mfence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

static inline void hw_clflush(const void *p)
{
    __asm__ volatile("clflush %0" : : "m"(*(const char *)p) : "memory");
}

static inline void hw_clflushopt(const void *p)
{
    if (g_clflushopt_enabled) {
        __asm__ volatile("clflushopt %0" : : "m"(*(const char *)p) : "memory");
    } else {
        hw_clflush(p);
    }
}

static inline void hw_clwb(const void *p)
{
    if (g_clwb_enabled) {
        __asm__ volatile("clwb %0" : : "m"(*(const char *)p) : "memory");
    } else {
        hw_clflushopt(p);
    }
}

/**
 * hw_serialize() — serialize instruction execution and memory transactions.
 * Uses SERIALIZE instruction if available, otherwise serializes via CPUID.
 */
static inline void hw_serialize(void)
{
    if (g_serialize_enabled) {
        __asm__ volatile(".byte 0x0f, 0x01, 0xe8" ::: "memory");
    } else {
        u32 eax = 0, ebx, ecx, edx;
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax) : "memory");
    }
}

/* ── Hardware Entropy Generation ─────────────────────────────────────────── */

static inline bool hw_rdrand64(u64 *val)
{
    if (!g_rdrand_enabled) return false;
    unsigned char ok;
    u64 v;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
    if (ok) {
        if (val) *val = v;
        return true;
    }
    return false;
}

static inline bool hw_rdseed64(u64 *val)
{
    if (!g_rdseed_enabled) return false;
    unsigned char ok;
    u64 v;
    __asm__ volatile("rdseed %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");
    if (ok) {
        if (val) *val = v;
        return true;
    }
    return false;
}

/* ── Contention backoff ──────────────────────────────────────────────────── */

/**
 * hw_spin_wait(spins) — back off inside a spin loop, @spins being the number
 * of iterations already burnt.
 *
 * Below a threshold this is a plain PAUSE. Past it, a CPU with WAITPKG parks
 * in TPAUSE's C0.2 state for a bounded TSC window, which frees the core's
 * execution resources for the sibling thread and draws materially less power
 * than spinning — while still waking on an interrupt, so a lock held across a
 * preemption still makes progress. Every path returns promptly; nothing here
 * changes the caller's correctness obligations.
 */
void hw_spin_wait(u32 spins);

