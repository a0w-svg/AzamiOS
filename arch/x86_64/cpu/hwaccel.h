/* ============================================================================
 * AzamiOS — Hardware-Accelerated Primitives (x86_64)
 * File: arch/x86_64/cpu/hwaccel.h
 *
 * cpu.c decides which extensions the CPU *has*; this module is where the
 * kernel's own hot paths actually spend them. Every routine here dispatches
 * once through a boot-time function pointer or a cached predicate, so a call
 * site pays no CPUID cost and no per-call branch chain.
 *
 * Only GPR-operand extensions appear here — SSE4.2's CRC32, POPCNT, MOVNTI,
 * CLZERO and the WAITPKG timed pauses. The kernel is built -mno-sse and does
 * not save XMM/YMM state across its own entry paths, so a routine that touched
 * a vector register would silently corrupt whichever thread it interrupted;
 * that is why there is no AVX memcpy here, and why there must not be one.
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

/* ── Bit scanning ────────────────────────────────────────────────────────── */

/* Set by hwaccel_init() when CPUID reports POPCNT. Read directly by the inline
 * below so a bitmap loop pays one predictable branch, not a call. */
extern u8 g_popcnt_enabled;

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
        __asm__("popcnt %1, %0" : "=r"(r) : "rm"(word) : "cc");
        return (unsigned)r;
    }
    /* Classic SWAR: pairwise sums, then nibbles, then a multiply that
     * accumulates all eight byte totals into the top byte. */
    word = word - ((word >> 1) & 0x5555555555555555ULL);
    word = (word & 0x3333333333333333ULL) + ((word >> 2) & 0x3333333333333333ULL);
    word = (word + (word >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (unsigned)((word * 0x0101010101010101ULL) >> 56);
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
