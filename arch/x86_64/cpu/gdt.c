/* ============================================================================
 * AzamiOS — GDT / TSS Implementation (x86_64)
 * File: arch/x86_64/cpu/gdt.c
 *
 * Changes from the old hal/gdt/gdt.c:
 *   1. FIXED: gdt_set_rsp0() no longer hard-codes core_id = 0.
 *             It takes an explicit core_id parameter (callers obtain this
 *             from the per-CPU GS base, not a global variable).
 *   2. FIXED: gdt_set_ist() likewise takes core_id explicitly.
 *   3. ADDED:  gdt_init_bsp() / gdt_init_ap() split: BSP can call gdt_init_bsp()
 *             early (before per-CPU GS is set up); APs call gdt_init_ap(id, stack).
 *   4. FIXED:  TSS descriptor built with the full 64-bit base address in one
 *             operation — no risk of the old partial-cast truncation bug.
 *   5. ADDED:  Readable kprintf output with "[GDT]" prefix for traceability.
 * ============================================================================ */

#include "gdt.h"

/* Early console (uart_puts / kprintf). Available before APIC / scheduler. */
#include "../../../drivers/char/uart.h"
#include "../../../drivers/char/console.h"

/* ── Per-CPU GDT + TSS storage ───────────────────────────────────────────── */
static cpu_gdt_t g_cpu_gdt[HAL_MAX_CPUS] __aligned(64);
static u8 g_df_stack[HAL_MAX_CPUS][4096] __aligned(16);
static u8 g_nmi_stack[HAL_MAX_CPUS][4096] __aligned(16);
static u8 g_mc_stack[HAL_MAX_CPUS][4096] __aligned(16);
static u8 g_dbg_stack[HAL_MAX_CPUS][4096] __aligned(16);

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/**
 * encode_entry() — Write an 8-byte GDT segment descriptor.
 *
 * @e           Pointer to the gdt_entry_t slot to fill.
 * @base        32-bit segment base (in 64-bit mode, ignored for CS/DS/SS).
 * @limit       20-bit segment limit.
 * @access      Access byte (P, DPL, S, type bits).
 * @flags       Upper nibble of granularity byte (G, DB, L, AVL bits).
 */
static void encode_entry(gdt_entry_t *e,
                          u32 base, u32 limit,
                          u8 access, u8 flags)
{
    e->limit_low   = (u16)(limit & 0xFFFFU);
    e->base_low    = (u16)(base  & 0xFFFFU);
    e->base_mid    = (u8)((base  >> 16) & 0xFFU);
    e->access      = access;
    /* Granularity byte: upper nibble = flags, lower nibble = limit[19:16] */
    e->granularity = (u8)(((limit >> 16) & 0x0FU) | (flags & 0xF0U));
    e->base_high   = (u8)((base  >> 24) & 0xFFU);
}

/**
 * encode_tss_descriptor() — Write the 16-byte TSS system descriptor.
 *
 * The TSS descriptor occupies two consecutive 8-byte GDT slots (slots 5 & 6).
 * We overlay a gdt_system_entry_t on top of slots 5–6 and fill all fields.
 *
 * @slots   Pointer to GDT slot[5] (the first of the two system slots).
 * @base    Full 64-bit linear address of the TSS.
 * @limit   Byte size of the TSS − 1.
 */
static void encode_tss_descriptor(gdt_entry_t *slots, uintptr_t base, u32 limit)
{
    gdt_system_entry_t *sys = (gdt_system_entry_t *)slots;

    /* Lower 8 bytes: standard segment descriptor format.
     * Access = 0x89: P=1, DPL=0, S=0 (system), Type=9 (64-bit Available TSS). */
    encode_entry(&sys->low,
                 (u32)(base & 0xFFFFFFFFUL), limit,
                 0x89,  /* Present | DPL=0 | Type 9 = 64-bit available TSS */
                 0x00); /* Granularity: byte-granular, no L/DB/G flags */

    /* Upper 8 bytes: bits 32–63 of base address + 4 bytes of reserved=0. */
    sys->base_upper = (u32)((base >> 32) & 0xFFFFFFFFUL);
    sys->reserved   = 0;
}

/* ── Core initialisation ──────────────────────────────────────────────────── */

static void gdt_init_core(u32 core_id, uintptr_t kernel_stack_top)
{
    if (core_id >= HAL_MAX_CPUS) {
        kprintf("[GDT] CRITICAL: core_id %u >= HAL_MAX_CPUS (%u) — aborting\n",
                core_id, HAL_MAX_CPUS);
        return;
    }

    cpu_gdt_t *g   = &g_cpu_gdt[core_id];
    tss_t     *tss = &g->tss;

    /* ── Zero the entire TSS before setting fields ── */
    for (u32 i = 0; i < sizeof(tss_t); i++)
        ((u8 *)tss)[i] = 0;

    tss->rsp0 = kernel_stack_top;

    tss->ist[0] = (u64)(uintptr_t)g_df_stack[core_id]  + sizeof(g_df_stack[core_id]);
    tss->ist[1] = (u64)(uintptr_t)g_nmi_stack[core_id] + sizeof(g_nmi_stack[core_id]);
    tss->ist[2] = (u64)(uintptr_t)g_mc_stack[core_id]  + sizeof(g_mc_stack[core_id]);
    tss->ist[3] = (u64)(uintptr_t)g_dbg_stack[core_id] + sizeof(g_dbg_stack[core_id]);

    tss->iopb_offset = (u16)sizeof(tss_t);

    encode_entry(&g->entries[0], 0, 0, 0, 0);
    encode_entry(&g->entries[1], 0, 0xFFFFF, 0x9A, 0x20);
    encode_entry(&g->entries[2], 0, 0xFFFFF, 0x92, 0x00);
    encode_entry(&g->entries[3], 0, 0xFFFFF, 0xF2, 0x00);
    encode_entry(&g->entries[4], 0, 0xFFFFF, 0xFA, 0x20);

    encode_tss_descriptor(&g->entries[5],
                          (uintptr_t)tss,
                          (u32)(sizeof(tss_t) - 1));

    g->ptr.limit = (u16)(sizeof(g->entries) - 1);
    g->ptr.base  = (u64)(uintptr_t)g->entries;

    gdt_flush(&g->ptr);
    tss_flush(SEL_TSS);

    if (core_id == 0) {
        kprintf("[GDT] Core 0: GDTR=0x%016llx limit=%u  TSS.RSP0=0x%016llx\n",
                (unsigned long long)g->ptr.base,
                (unsigned)g->ptr.limit + 1,
                (unsigned long long)kernel_stack_top);
    }
}

void gdt_init_bsp(void)
{
    /* Use the current stack pointer as the initial ring-0 stack for core 0. */
    uintptr_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    gdt_init_core(0, rsp & ~0xFUL);  /* 16-byte aligned */
}

void gdt_init_ap(u32 core_id, uintptr_t kernel_stack_top)
{
    gdt_init_core(core_id, kernel_stack_top);
}

/* ── Runtime TSS update ───────────────────────────────────────────────────── */

void gdt_set_rsp0(u32 core_id, uintptr_t stack_top)
{
    /*
     * OLD BUG: The original code hard-coded core_id = 0 here, so on an SMP
     * system every context switch silently updated Core 0's TSS no matter
     * which core was actually running. This caused random ring-0 stack
     * corruption on any core > 0 on the first interrupt after a switch.
     *
     * FIX: The caller (scheduler context-switch path) provides the correct
     * core_id obtained from the per-CPU GS base block (cpu_data_t.cpu_id).
     */
    if (unlikely(core_id >= HAL_MAX_CPUS)) return;
    g_cpu_gdt[core_id].tss.rsp0 = stack_top;
}

void gdt_set_ist(u32 core_id, int index, uintptr_t stack_top)
{
    if (unlikely(core_id >= HAL_MAX_CPUS)) return;
    if (unlikely(index < 1 || index > 7)) {
        kprintf("[GDT] ERROR: IST index %d out of range (1–7)\n", index);
        return;
    }
    g_cpu_gdt[core_id].tss.ist[index - 1] = stack_top;
}
