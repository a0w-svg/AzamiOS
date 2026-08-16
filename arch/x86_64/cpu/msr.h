/* ============================================================================
 * AzamiOS — MSR & CPUID Helpers (x86_64)
 * File: arch/x86_64/cpu/msr.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/* ── Well-known MSR addresses ─────────────────────────────────────────────── */
#define MSR_EFER          0xC0000080UL  /* Extended Feature Enable Register */
#define MSR_STAR          0xC0000081UL  /* Syscall target CS/SS selectors */
#define MSR_LSTAR         0xC0000082UL  /* Syscall handler RIP (64-bit mode) */
#define MSR_CSTAR         0xC0000083UL  /* Syscall handler RIP (compat mode) */
#define MSR_SFMASK        0xC0000084UL  /* RFLAGS bits to clear on SYSCALL */
#define MSR_FS_BASE       0xC0000100UL  /* FS segment base (user TLS) */
#define MSR_GS_BASE       0xC0000101UL  /* GS segment base (kernel per-CPU) */
#define MSR_KERNEL_GS_BASE 0xC0000102UL /* GS base after SWAPGS */
#define MSR_APIC_BASE     0x0000001BUL  /* APIC base address */

/* EFER bit definitions */
#define EFER_SCE  (1ULL << 0)   /* SYSCALL Enable */
#define EFER_LME  (1ULL << 8)   /* Long Mode Enable */
#define EFER_LMA  (1ULL << 10)  /* Long Mode Active (read-only) */
#define EFER_NXE  (1ULL << 11)  /* No-Execute Enable */

/* ── Inline RDMSR / WRMSR ─────────────────────────────────────────────────── */

static __attribute__((always_inline)) inline u64 rdmsr(u32 msr)
{
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

static __attribute__((always_inline)) inline void wrmsr(u32 msr, u64 val)
{
    u32 lo = (u32)(val & 0xFFFFFFFFUL);
    u32 hi = (u32)(val >> 32);
    __asm__ volatile("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
}

/* ── CPUID wrapper ─────────────────────────────────────────────────────────── */

static inline void cpuid(u32 leaf, u32 subleaf,
                          u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    __asm__ volatile("cpuid"
                     : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf),  "c"(subleaf));
}

/* Convenience: check if CPUID leaf is available */
static inline bool cpuid_has_leaf(u32 leaf)
{
    u32 max_leaf, b, c, d;
    cpuid(0, 0, &max_leaf, &b, &c, &d);
    return leaf <= max_leaf;
}

/* ── CR register accessors ─────────────────────────────────────────────────── */

static __attribute__((always_inline)) inline u64 read_cr0(void) {
    u64 v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr0(u64 v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static __attribute__((always_inline)) inline u64 read_cr2(void) {
    u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline u64 read_cr3(void) {
    u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr3(u64 v) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}
static __attribute__((always_inline)) inline u64 read_cr4(void) {
    u64 v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v;
}
static __attribute__((always_inline)) inline void write_cr4(u64 v) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

/* ── TLB invalidation ─────────────────────────────────────────────────────── */
static __attribute__((always_inline)) inline void invlpg(uintptr_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}
static __attribute__((always_inline)) inline void tlb_flush_all(void) {
    write_cr3(read_cr3());  /* Reload CR3 to flush entire TLB (except global pages) */
}
