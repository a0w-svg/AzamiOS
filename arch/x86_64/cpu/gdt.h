/* ============================================================================
 * AzamiOS — Global Descriptor Table & Task State Segment (x86_64)
 * File: arch/x86_64/cpu/gdt.h
 *
 * Per-CPU GDT layout (7 slots = 56 bytes, plus a 16-byte TSS descriptor):
 *
 *  Selector  Slot  Contents
 *  ────────  ────  ──────────────────────────────────────────────────────────
 *   0x00      0    Null descriptor (mandatory)
 *   0x08      1    Kernel 64-bit code  (DPL=0, L=1)
 *   0x10      2    Kernel 64-bit data  (DPL=0, W=1)
 *   0x18      3    User   64-bit data  (DPL=3, W=1)   ← SYSRET order: data first
 *   0x20      4    User   64-bit code  (DPL=3, L=1)
 *   0x28      5    TSS descriptor low  (64-bit system, 16 bytes total)
 *   0x30      6    TSS descriptor high (base_upper + reserved)
 *
 * Note: User data (0x18) before user code (0x20) matches the selector layout
 *       required by SYSRET on Intel CPUs (CS = STAR.SYSRET_CS + 16,
 *       SS = STAR.SYSRET_CS + 8 → so STAR[63:48] = 0x10 giving SS=0x18, CS=0x20).
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"

/* ── Segment selectors ───────────────────────────────────────────────────── */
#define SEL_NULL        0x00
#define SEL_KERNEL_CODE 0x08
#define SEL_KERNEL_DATA 0x10
#define SEL_USER_DATA   0x18    /* RPL 3: 0x1B */
#define SEL_USER_CODE   0x20    /* RPL 3: 0x23 */
#define SEL_TSS         0x28

#define RPL3(sel)       ((sel) | 0x3)   /* Add Ring-3 RPL bits */

/* ── Maximum supported CPU cores ─────────────────────────────────────────── */
#ifndef HAL_MAX_CPUS
#define HAL_MAX_CPUS    256
#endif

/* ── 8-byte GDT entry ────────────────────────────────────────────────────── */
typedef struct __packed {
    u16 limit_low;     /* Bits  0–15  of segment limit */
    u16 base_low;      /* Bits  0–15  of base address  */
    u8  base_mid;      /* Bits 16–23  of base address  */
    u8  access;        /* Access byte (P, DPL, S, type) */
    u8  granularity;   /* Limit high [19:16] | flags (G, DB, L, AVL) */
    u8  base_high;     /* Bits 24–31  of base address  */
} gdt_entry_t;

/* ── 16-byte system descriptor (TSS / LDT in 64-bit mode) ───────────────── */
typedef struct __packed {
    gdt_entry_t low;       /* Standard 8-byte lower half */
    u32         base_upper;/* Bits 32–63 of base address */
    u32         reserved;  /* Must be zero */
} gdt_system_entry_t;

BUILD_ASSERT(sizeof(gdt_entry_t)        == 8,  "gdt_entry_t must be 8 bytes");
BUILD_ASSERT(sizeof(gdt_system_entry_t) == 16, "gdt_system_entry_t must be 16 bytes");

/* ── GDTR pointer structure (10 bytes) ───────────────────────────────────── */
typedef struct __packed {
    u16      limit;    /* Size of GDT in bytes − 1 */
    u64      base;     /* Linear address of GDT */
} gdt_ptr_t;

/* ── x86_64 Task State Segment (104 bytes + IOPB) ───────────────────────── */
typedef struct __packed {
    u32 reserved0;
    u64 rsp0;          /* Ring-0 stack pointer (loaded on interrupt/syscall) */
    u64 rsp1;          /* Ring-1 stack pointer (unused in AzamiOS) */
    u64 rsp2;          /* Ring-2 stack pointer (unused in AzamiOS) */
    u64 reserved1;
    u64 ist[7];        /* IST1–IST7: dedicated stacks for #DF, #NMI, #MC … */
    u64 reserved2;
    u16 reserved3;
    u16 iopb_offset;   /* Offset to I/O Permission Bitmap (= sizeof(tss_t)
                          means "no IOPB" → all port access denied in ring 3) */
} tss_t;

BUILD_ASSERT(sizeof(tss_t) == 104, "tss_t must be 104 bytes per Intel SDM");

/* ── Per-CPU GDT storage (exported for SMP AP use) ──────────────────────── */
typedef struct {
    gdt_entry_t        entries[7];  /* Slots 0–4 (8-byte descriptors) + Slots 5–6 (16-byte TSS) */
    gdt_ptr_t          ptr;
    tss_t              tss          __aligned(16);
} __aligned(64) cpu_gdt_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * gdt_init_bsp() — Initialise the GDT and TSS for the bootstrap processor.
 *
 * Called once from kernel_main() before interrupts are enabled.
 * Automatically reads the current RSP and uses it as the initial RSP0.
 */
void gdt_init_bsp(void);

/**
 * gdt_init_ap(core_id, kernel_stack_top) — Initialise GDT/TSS for an AP core.
 *
 * Called from the AP trampoline (smp.c) for each application processor.
 * @core_id          APIC ID of this core (0 < core_id < HAL_MAX_CPUS).
 * @kernel_stack_top Top of the AP's dedicated kernel stack (RSP0).
 */
void gdt_init_ap(u32 core_id, uintptr_t kernel_stack_top);

/**
 * gdt_set_rsp0(core_id, stack_top) — Update TSS.RSP0 for the given core.
 *
 * Must be called on every context switch to ensure the next ring-0 entry
 * (interrupt, syscall) uses the correct kernel stack for the new thread.
 */
void gdt_set_rsp0(u32 core_id, uintptr_t stack_top);

/**
 * gdt_set_ist(core_id, index, stack_top) — Set an IST stack pointer.
 *
 * @index  1–7 (IST1 through IST7).
 */
void gdt_set_ist(u32 core_id, int index, uintptr_t stack_top);

/* ── Assembly stubs (defined in gdt_flush.asm) ───────────────────────────── */
extern void gdt_flush(gdt_ptr_t *ptr);   /* lgdt + far-return to reload CS */
extern void tss_flush(u16 selector);     /* ltr selector */
