/* ============================================================================
 * AzamiOS — Hardware-Accelerated Primitives (x86_64)
 * File: arch/x86_64/cpu/hwaccel.c
 *
 * See hwaccel.h for the contract. The rule this file lives by: every
 * instruction emitted here operates on general-purpose registers only. The
 * kernel is compiled -mno-sse/-mno-mmx and its interrupt and syscall entry
 * paths do not save XMM/YMM/ZMM, so a vector instruction executed on behalf of
 * the kernel would destroy the interrupted thread's live SIMD state with no
 * fault to point at afterwards.
 *
 * CPUID bits are treated as claims to be checked, not facts — the same lesson
 * cpu.c already learnt from XSAVEC and MWAIT. Anything whose absence is only
 * detectable as a #UD is probed once at boot behind the .extable fixup that
 * arch/x86_64/lib/uaccess.asm's handler already provides.
 * ============================================================================ */

#include "hwaccel.h"
#include "cpu.h"
#include "msr.h"
#include "../../../include/azami/defs.h"
#include "../../../kernel/lib/string.h"

extern void kprintf(const char *fmt, ...);
extern int  scnprintf(char *buf, size_t size, const char *fmt, ...);
extern u8   g_erms_enabled;

/* Probe helper shared with cpu.c: runs one candidate instruction under an
 * exception fixup and reports whether it faulted. */
extern int hwaccel_probe_asm(void *scratch, u32 which);

/* Selector values for hwaccel_probe_asm(). Matches cases in hwprobe.asm. */
#define PROBE_CLZERO     0
#define PROBE_TPAUSE     3
#define PROBE_RDPMC      4
#define PROBE_CLFLUSHOPT 5
#define PROBE_CLWB       6
#define PROBE_SERIALIZE  7
#define PROBE_RDRAND     8
#define PROBE_RDSEED     9

/* ── Chosen strategies ───────────────────────────────────────────────────── */
static u8  s_clear_variant = HW_CLEAR_REP_STOSB;
static u8  s_have_crc32    = 0;
static u8  s_have_tpause   = 0;
u8         g_popcnt_enabled     = 0;
u8         g_lzcnt_enabled      = 0;
u8         g_bmi1_enabled       = 0;
u8         g_bmi2_enabled       = 0;
u8         g_clflushopt_enabled = 0;
u8         g_clwb_enabled       = 0;
u8         g_serialize_enabled  = 0;
u8         g_rdrand_enabled     = 0;
u8         g_rdseed_enabled     = 0;
static u32 s_line_size          = 64;

/* Scratch for the boot probes. A whole line so CLZERO, whose operand is
 * rounded down to the enclosing cache line, cannot scribble past it. */
static u8 s_probe_area[128] __attribute__((aligned(64)));

/* ── CRC-32C ─────────────────────────────────────────────────────────────── */

/* Castagnoli polynomial 0x1EDC6F41, reflected as 0x82F63B78 — the one SSE4.2's
 * CRC32 instruction implements, so the table path and the instruction path
 * agree bit for bit. Built at boot rather than stored: 1 KiB of .data for
 * something a few hundred cycles can generate is a poor trade in a kernel
 * image, and on any CPU from the last fifteen years it is never consulted. */
static u32 s_crc32c_table[256];

static void crc32c_build_table(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        s_crc32c_table[i] = c;
    }
}

static u32 crc32c_sw(u32 crc, const u8 *p, size_t len)
{
    while (len--) crc = s_crc32c_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

u32 crc32c(u32 crc, const void *buf, size_t len)
{
    const u8 *p = (const u8 *)buf;
    if (!p || len == 0) return crc;

    if (!s_have_crc32) return crc32c_sw(crc, p, len);

    /* Reach an 8-byte boundary a byte at a time, then consume qwords: the
     * 64-bit form retires one per cycle where the byte form retires one byte,
     * and CRC32 has no alignment requirement beyond what the load itself has. */
    while (len && ((uintptr_t)p & 7)) {
        __asm__("crc32b %1, %0" : "+r"(crc) : "rm"(*p));
        p++; len--;
    }

    u64 c64 = crc;
    while (len >= 8) {
        __asm__("crc32q %1, %0" : "+r"(c64) : "rm"(*(const u64 *)p));
        p += 8; len -= 8;
    }
    crc = (u32)c64;

    while (len--) {
        __asm__("crc32b %1, %0" : "+r"(crc) : "rm"(*p));
        p++;
    }
    return crc;
}

/* ── Page clear / copy ───────────────────────────────────────────────────── *
 *
 * Everything here is written as a *run* over an arbitrary byte count rather
 * than as a one-page routine, and the fence lives at the end of the run rather
 * than inside the strategy.
 *
 * Both choices exist for the same reason. hw_clear_pages() used to be a loop
 * calling hw_clear_page(), so clearing the 512 pages behind a 2 MB huge page
 * paid 512 separate SFENCEs — a full store-buffer drain each, for an operation
 * that only needs to be ordered once, at the end, before the caller publishes
 * the memory. On the ERMS path it was worse: `rep stosb`'s fast path only
 * engages after a setup cost of a few dozen cycles, and restarting the string
 * operation every 4 KB throws away most of what ERMS is for. Issued as one run
 * it is a single instruction for the whole 2 MB.
 *
 * Contract for the non-temporal paths: @p is 64-byte aligned and @bytes is a
 * multiple of 64. Every caller passes page-aligned addresses and page-multiple
 * sizes, which satisfies both with room to spare. CLZERO in particular rounds
 * its operand down to the enclosing line, so a short tail would zero past the
 * end of the region rather than stopping inside it.
 */

/* CLZERO zeroes the cache line containing the address in RAX and, unlike a
 * store, never fetches it first. Zeroing this way costs no read bandwidth at
 * all and leaves the caches holding whatever they held before. */
static inline void clear_run_clzero(u8 *p, size_t bytes)
{
    for (size_t off = 0; off < bytes; off += s_line_size)
        __asm__ volatile("clzero" : : "a"(p + off) : "memory");
}

/* MOVNTI: a GPR non-temporal store. Same cache-pollution argument as CLZERO,
 * available on everything back to SSE2, and — critically for this kernel —
 * needs no XMM register. */
static inline void clear_run_movnti(u8 *p, size_t bytes)
{
    u64 zero = 0;
    for (size_t off = 0; off < bytes; off += 64) {
        u64 *q = (u64 *)(p + off);
        __asm__ volatile(
            "movnti %1, 0(%0)  \n"
            "movnti %1, 8(%0)  \n"
            "movnti %1, 16(%0) \n"
            "movnti %1, 24(%0) \n"
            "movnti %1, 32(%0) \n"
            "movnti %1, 40(%0) \n"
            "movnti %1, 48(%0) \n"
            "movnti %1, 56(%0) \n"
            : : "r"(q), "r"(zero) : "memory");
    }
}

/*
 * Zero @bytes at @p with ordinary cached stores, for memory the caller is
 * about to write into.
 *
 * The non-temporal strategies above are the right default because a freshly
 * allocated page is normally written once and then left alone, so keeping it
 * out of the cache is free. A page *table* is the opposite case: it is zeroed
 * and then immediately filled in entry by entry. Zeroing it non-temporally
 * writes around the cache and the fill then pulls every line back in with a
 * read-for-ownership — the clear buys nothing and costs a second pass over the
 * page. Cached stores leave the lines in this core's L1 already owned, which
 * is exactly where the fill wants them.
 *
 * `rep stosq` rather than `rep stosb` on the non-ERMS path: without ERMS the
 * byte form is genuinely a byte at a time, while the quadword form has always
 * moved eight.
 */
static void clear_run_cached(u8 *p, size_t bytes)
{
    if (g_erms_enabled) {
        __asm__ volatile("rep stosb" : "+D"(p), "+c"(bytes) : "a"(0) : "memory");
        return;
    }
    size_t q = bytes / 8;
    u64 *d = (u64 *)p;
    __asm__ volatile("rep stosq" : "+D"(d), "+c"(q) : "a"(0ULL) : "memory");
    size_t rem = bytes & 7;
    u8 *t = (u8 *)d;
    while (rem--) *t++ = 0;
}

/* Zero @bytes at @p with whichever strategy hwaccel_init() picked, fencing
 * once at the end if that strategy needs it. */
static void clear_run(u8 *p, size_t bytes)
{
    switch (s_clear_variant) {
    case HW_CLEAR_CLZERO:
        clear_run_clzero(p, bytes);
        break;
    case HW_CLEAR_MOVNTI:
        clear_run_movnti(p, bytes);
        break;
    default:
        /* Ordinary stores: already ordered with respect to what follows, so
         * this path wants no fence at all. */
        __asm__ volatile("rep stosb" : "+D"(p), "+c"(bytes) : "a"(0) : "memory");
        return;
    }
    /* CLZERO and MOVNTI are both weakly ordered with respect to younger
     * stores. One fence covers the whole run. */
    __asm__ volatile("sfence" ::: "memory");
}

/* Copy @bytes from @s to @d. With ERMS a single `rep movsb` already runs at
 * line width and picks its own non-temporal hint for large copies; hand-rolled
 * stores can only lose. Without it, the non-temporal path keeps the copy from
 * flushing the L1 of whatever is about to run — which for a 1 GB huge-page
 * fork is the difference between evicting the working set once and evicting it
 * two hundred and fifty thousand times. */
static void copy_run(u8 *d, const u8 *s, size_t bytes)
{
    if (g_erms_enabled) {
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(bytes) : : "memory");
        return;
    }

    const u64 *sq = (const u64 *)s;
    u64 *dq = (u64 *)d;
    for (size_t i = 0; i < bytes / 8; i += 8) {
        u64 v0 = sq[i + 0], v1 = sq[i + 1], v2 = sq[i + 2], v3 = sq[i + 3];
        u64 v4 = sq[i + 4], v5 = sq[i + 5], v6 = sq[i + 6], v7 = sq[i + 7];
        __asm__ volatile(
            "movnti %1, 0(%0)  \n"
            "movnti %2, 8(%0)  \n"
            "movnti %3, 16(%0) \n"
            "movnti %4, 24(%0) \n"
            : : "r"(&dq[i]), "r"(v0), "r"(v1), "r"(v2), "r"(v3) : "memory");
        __asm__ volatile(
            "movnti %1, 32(%0) \n"
            "movnti %2, 40(%0) \n"
            "movnti %3, 48(%0) \n"
            "movnti %4, 56(%0) \n"
            : : "r"(&dq[i]), "r"(v4), "r"(v5), "r"(v6), "r"(v7) : "memory");
    }
    __asm__ volatile("sfence" ::: "memory");
}

void hw_clear_page(void *va)
{
    if (!va) return;
    clear_run((u8 *)va, PAGE_SIZE);
}

void hw_clear_page_hot(void *va)
{
    if (!va) return;
    clear_run_cached((u8 *)va, PAGE_SIZE);
}

void hw_clear_pages(void *va, size_t npages)
{
    if (!va || npages == 0) return;
    clear_run((u8 *)va, npages * PAGE_SIZE);
}

void hw_copy_page(void *dst, const void *src)
{
    if (!dst || !src) return;
    copy_run((u8 *)dst, (const u8 *)src, PAGE_SIZE);
}

void hw_copy_pages(void *dst, const void *src, size_t npages)
{
    if (!dst || !src || npages == 0) return;
    copy_run((u8 *)dst, (const u8 *)src, npages * PAGE_SIZE);
}

void hw_copy_to_vram(void *dst, const void *src, size_t len)
{
    if (!dst || !src || len == 0) return;

    if (g_erms_enabled) {
        u8 *d = (u8 *)dst;
        const u8 *s = (const u8 *)src;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(len) : : "memory");
        return;
    }

    u8 *d8 = (u8 *)dst;
    const u8 *src8 = (const u8 *)src;
    while (len && ((uintptr_t)d8 & 7)) {
        *d8++ = *src8++;
        len--;
    }

    u64 *d64 = (u64 *)d8;
    const u64 *src64 = (const u64 *)src8;
    size_t qwords = len / 8;
    size_t blocks = qwords / 8;

    for (size_t b = 0; b < blocks; b++) {
        u64 v0 = src64[0], v1 = src64[1], v2 = src64[2], v3 = src64[3];
        u64 v4 = src64[4], v5 = src64[5], v6 = src64[6], v7 = src64[7];
        __asm__ volatile(
            "movnti %1, 0(%0)  \n"
            "movnti %2, 8(%0)  \n"
            "movnti %3, 16(%0) \n"
            "movnti %4, 24(%0) \n"
            "movnti %5, 32(%0) \n"
            "movnti %6, 40(%0) \n"
            "movnti %7, 48(%0) \n"
            "movnti %8, 56(%0) \n"
            : : "r"(d64), "r"(v0), "r"(v1), "r"(v2), "r"(v3),
                "r"(v4), "r"(v5), "r"(v6), "r"(v7) : "memory");
        d64 += 8;
        src64 += 8;
    }

    size_t rem_q = qwords % 8;
    for (size_t i = 0; i < rem_q; i++) {
        u64 v = *src64++;
        __asm__ volatile("movnti %1, (%0)" : : "r"(d64++), "r"(v) : "memory");
    }

    __asm__ volatile("sfence" ::: "memory");

    d8 = (u8 *)d64;
    src8 = (const u8 *)src64;
    size_t rem_bytes = len & 7;
    while (rem_bytes--) {
        *d8++ = *src8++;
    }
}

void hw_fill_vram(void *dst, u32 val, size_t count)
{
    if (!dst || count == 0) return;

    u64 val64 = ((u64)val << 32) | (u64)val;
    u32 *d32 = (u32 *)dst;

    if (((uintptr_t)d32 & 4) && count > 0) {
        *d32++ = val;
        count--;
    }

    size_t qwords = count / 2;
    u64 *d64 = (u64 *)d32;

    if (g_erms_enabled) {
        __asm__ volatile("rep stosq" : "+D"(d64), "+c"(qwords) : "a"(val64) : "memory");
    } else {
        size_t blocks = qwords / 8;
        for (size_t b = 0; b < blocks; b++) {
            __asm__ volatile(
                "movnti %1, 0(%0)  \n"
                "movnti %1, 8(%0)  \n"
                "movnti %1, 16(%0) \n"
                "movnti %1, 24(%0) \n"
                "movnti %1, 32(%0) \n"
                "movnti %1, 40(%0) \n"
                "movnti %1, 48(%0) \n"
                "movnti %1, 56(%0) \n"
                : : "r"(d64), "r"(val64) : "memory");
            d64 += 8;
        }
        size_t rem_q = qwords % 8;
        for (size_t i = 0; i < rem_q; i++) {
            __asm__ volatile("movnti %1, (%0)" : : "r"(d64++), "r"(val64) : "memory");
        }
        __asm__ volatile("sfence" ::: "memory");
    }

    if (count & 1) {
        d32 = (u32 *)((u8 *)d64 + qwords * 8);
        *d32 = val;
    }
}

/* ── Contention backoff ──────────────────────────────────────────────────── */

/* Iterations of plain PAUSE before it is worth entering a power state, and the
 * TSC window to stay there. 2048 ticks is a few hundred nanoseconds on any
 * current part: long enough to matter for power and for the sibling thread,
 * short enough that a lock released immediately after we parked costs one
 * wasted window rather than a scheduling quantum. */
#define SPIN_PAUSE_ROUNDS  64
#define TPAUSE_WINDOW      2048ULL

void hw_spin_wait(u32 spins)
{
    if (!s_have_tpause || spins < SPIN_PAUSE_ROUNDS) {
        cpu_pause();
        return;
    }

    u64 deadline = rdtsc() + TPAUSE_WINDOW;
    /* ECX bit 0 clear selects C0.2 — the deeper of the two states, which frees
     * more of the core but takes longer to leave. TPAUSE sets CF when the
     * deadline had already passed; we do not care either way, since the caller
     * re-tests its condition regardless. */
    __asm__ volatile("tpause %0"
                     : : "r"(0u), "d"((u32)(deadline >> 32)),
                         "a"((u32)deadline)
                     : "cc");
}

/* TPAUSE's maximum residency is capped by IA32_UMWAIT_CONTROL, which is a
 * per-logical-processor MSR. Its reset value leaves C0.2 disabled on some
 * parts, which silently degrades TPAUSE to the shallower C0.1; program a known
 * bound instead — bit 0 clear permits C0.2, bits 31:2 are the maximum residency
 * in TSC ticks. Every core must get the same value or the same spin loop backs
 * off differently depending on where it runs. */
static void program_umwait_control(void)
{
    wrmsr(MSR_IA32_UMWAIT_CONTROL, (TPAUSE_WINDOW * 4) & ~3ULL);
}

/* ── Boot-time selection ─────────────────────────────────────────────────── */

void hwaccel_init(void)
{
    crc32c_build_table();

    if (g_cpu_info.clflush_size) s_line_size = g_cpu_info.clflush_size;
    if (s_line_size < 32 || s_line_size > 128) s_line_size = 64;

    s_have_crc32     = g_cpu_info.has_sse4_2 ? 1 : 0;
    g_popcnt_enabled = g_cpu_info.has_popcnt ? 1 : 0;
    g_lzcnt_enabled  = g_cpu_info.has_lzcnt  ? 1 : 0;
    g_bmi1_enabled   = g_cpu_info.has_bmi1   ? 1 : 0;
    g_bmi2_enabled   = g_cpu_info.has_bmi2   ? 1 : 0;

    if (g_cpu_info.has_clflushopt &&
        hwaccel_probe_asm(s_probe_area, PROBE_CLFLUSHOPT) == 0) {
        g_clflushopt_enabled = 1;
    }
    if (g_cpu_info.has_clwb &&
        hwaccel_probe_asm(s_probe_area, PROBE_CLWB) == 0) {
        g_clwb_enabled = 1;
    }
    if (g_cpu_info.has_serialize &&
        hwaccel_probe_asm(s_probe_area, PROBE_SERIALIZE) == 0) {
        g_serialize_enabled = 1;
    }
    if (g_cpu_info.has_rdrand &&
        hwaccel_probe_asm(s_probe_area, PROBE_RDRAND) == 0) {
        g_rdrand_enabled = 1;
    }
    if (g_cpu_info.has_rdseed &&
        hwaccel_probe_asm(s_probe_area, PROBE_RDSEED) == 0) {
        g_rdseed_enabled = 1;
    }

    /* Page clear. CLZERO first (no read-for-ownership at all), then MOVNTI,
     * then ERMS. A CPU with ERMS but no CLZERO keeps `rep stosb`: on those
     * parts the microcoded path already streams and beats an explicit loop. */
    if (g_cpu_info.has_clzero &&
        hwaccel_probe_asm(s_probe_area, PROBE_CLZERO) == 0) {
        s_clear_variant = HW_CLEAR_CLZERO;
    } else if (!g_erms_enabled && (g_cpu_info.features & CPU_FEAT_SSE2)) {
        /* MOVNTI is SSE2-era but writes through a GPR, so it is usable even
         * though the kernel never enables SSE for its own code. */
        s_clear_variant = HW_CLEAR_MOVNTI;
    } else {
        s_clear_variant = HW_CLEAR_REP_STOSB;
    }

    if (g_cpu_info.has_waitpkg &&
        hwaccel_probe_asm(s_probe_area, PROBE_TPAUSE) == 0) {
        program_umwait_control();
        s_have_tpause = 1;
    }

    kprintf("[CPU] hwaccel: crc32c=%s popcnt=%s bmi=%s%s%s cache=%s%s%s ser=%s rng=%s%s clear=%s spin=%s (line %u B)\n",
            s_have_crc32 ? "sse4.2" : "table",
            g_popcnt_enabled ? "hw" : "swar",
            g_cpu_info.has_bmi1 ? "tzcnt" : "bsf",
            g_lzcnt_enabled ? "+lzcnt" : "",
            g_bmi2_enabled ? "+bmi2" : "",
            "clflush",
            g_clflushopt_enabled ? "+opt" : "",
            g_clwb_enabled ? "+clwb" : "",
            g_serialize_enabled ? "hw" : "cpuid",
            g_rdrand_enabled ? "rdrand" : "none",
            g_rdseed_enabled ? "+rdseed" : "",
            s_clear_variant == HW_CLEAR_CLZERO ? "clzero" :
            s_clear_variant == HW_CLEAR_MOVNTI ? "movnti" : "rep-stosb",
            s_have_tpause ? "tpause" : "pause",
            s_line_size);
}

size_t hwaccel_format(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
            "crc32c=%s popcnt=%s bmi=%s%s%s cache=%s%s%s ser=%s rng=%s%s clear=%s spin=%s",
            s_have_crc32 ? "sse4.2" : "table",
            g_popcnt_enabled ? "hw" : "swar",
            g_cpu_info.has_bmi1 ? "tzcnt" : "bsf",
            g_lzcnt_enabled ? "+lzcnt" : "",
            g_bmi2_enabled ? "+bmi2" : "",
            "clflush",
            g_clflushopt_enabled ? "+opt" : "",
            g_clwb_enabled ? "+clwb" : "",
            g_serialize_enabled ? "hw" : "cpuid",
            g_rdrand_enabled ? "rdrand" : "none",
            g_rdseed_enabled ? "+rdseed" : "",
            s_clear_variant == HW_CLEAR_CLZERO ? "clzero" :
            s_clear_variant == HW_CLEAR_MOVNTI ? "movnti" : "rep-stosb",
            s_have_tpause ? "tpause" : "pause");
}

void hwaccel_init_ap(void)
{
    /* Only the per-core MSR needs replaying: every other decision this module
     * made lives in a global the AP reads directly. */
    if (s_have_tpause) program_umwait_control();
}
