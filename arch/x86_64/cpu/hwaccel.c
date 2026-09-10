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

/* Selector values for hwaccel_probe_asm(). They are sparse because the probe
 * stub numbers its cases and these are the two the kernel still needs. */
#define PROBE_CLZERO     0
#define PROBE_TPAUSE     3

/* ── Chosen strategies ───────────────────────────────────────────────────── */
static u8  s_clear_variant = HW_CLEAR_REP_STOSB;
static u8  s_have_crc32    = 0;
static u8  s_have_tpause   = 0;
u8         g_popcnt_enabled = 0;
static u32 s_line_size     = 64;

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

/* ── Page clear / copy ───────────────────────────────────────────────────── */

/* CLZERO zeroes the cache line containing the address in RAX and, unlike a
 * store, never fetches it first. Zeroing a page this way costs no read
 * bandwidth at all and leaves the caches holding whatever they held before. */
static inline void clear_page_clzero(u8 *p)
{
    for (u32 off = 0; off < PAGE_SIZE; off += s_line_size)
        __asm__ volatile("clzero" : : "a"(p + off) : "memory");
    /* CLZERO is weakly ordered with respect to younger stores. */
    __asm__ volatile("sfence" ::: "memory");
}

/* MOVNTI: a GPR non-temporal store. Same cache-pollution argument as CLZERO,
 * available on everything back to SSE2, and — critically for this kernel —
 * needs no XMM register. */
static inline void clear_page_movnti(u8 *p)
{
    u64 zero = 0;
    for (u32 off = 0; off < PAGE_SIZE; off += 64) {
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
    __asm__ volatile("sfence" ::: "memory");
}

void hw_clear_page(void *va)
{
    u8 *p = (u8 *)va;
    if (!p) return;

    switch (s_clear_variant) {
    case HW_CLEAR_CLZERO: clear_page_clzero(p); return;
    case HW_CLEAR_MOVNTI: clear_page_movnti(p); return;
    default: {
        size_t n = PAGE_SIZE;
        __asm__ volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(0) : "memory");
        return;
    }
    }
}

void hw_clear_pages(void *va, size_t npages)
{
    u8 *p = (u8 *)va;
    for (size_t i = 0; i < npages; i++) hw_clear_page(p + i * PAGE_SIZE);
}

void hw_copy_page(void *dst, const void *src)
{
    if (!dst || !src) return;

    /* With ERMS a single `rep movsb` already runs at line width and picks its
     * own non-temporal hint for large copies; hand-rolling stores can only
     * lose. Without it, the non-temporal path keeps a page copy from flushing
     * the L1 of whatever is about to run. */
    if (g_erms_enabled) {
        u8 *d = (u8 *)dst;
        const u8 *s = (const u8 *)src;
        size_t n = PAGE_SIZE;
        __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
        return;
    }

    const u64 *s = (const u64 *)src;
    u64 *d = (u64 *)dst;
    for (u32 i = 0; i < PAGE_SIZE / 8; i += 8) {
        u64 v0 = s[i + 0], v1 = s[i + 1], v2 = s[i + 2], v3 = s[i + 3];
        u64 v4 = s[i + 4], v5 = s[i + 5], v6 = s[i + 6], v7 = s[i + 7];
        __asm__ volatile(
            "movnti %1, 0(%0)  \n"
            "movnti %2, 8(%0)  \n"
            "movnti %3, 16(%0) \n"
            "movnti %4, 24(%0) \n"
            : : "r"(&d[i]), "r"(v0), "r"(v1), "r"(v2), "r"(v3) : "memory");
        __asm__ volatile(
            "movnti %1, 32(%0) \n"
            "movnti %2, 40(%0) \n"
            "movnti %3, 48(%0) \n"
            "movnti %4, 56(%0) \n"
            : : "r"(&d[i]), "r"(v4), "r"(v5), "r"(v6), "r"(v7) : "memory");
    }
    __asm__ volatile("sfence" ::: "memory");
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

    kprintf("[CPU] hwaccel: crc32c=%s popcnt=%s clear=%s spin=%s (line %u B)\n",
            s_have_crc32 ? "sse4.2" : "table",
            g_popcnt_enabled ? "hw" : "swar",
            s_clear_variant == HW_CLEAR_CLZERO ? "clzero" :
            s_clear_variant == HW_CLEAR_MOVNTI ? "movnti" : "rep-stosb",
            s_have_tpause ? "tpause" : "pause",
            s_line_size);
}

size_t hwaccel_format(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max, "crc32c=%s popcnt=%s clear=%s spin=%s",
            s_have_crc32 ? "sse4.2" : "table",
            g_popcnt_enabled ? "hw" : "swar",
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
