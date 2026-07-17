/*
 * =========================================================================================
 * Azami OS — Hardware Abstraction Layer (HAL) Global Descriptor Table & TSS Module
 * File: /kernel/hal/gdt/gdt.c
 * Architecture: x86_64 (64-Bit Long Mode exclusively)
 *
 * Description:
 *   Constructs and manages the Global Descriptor Table (GDT) and Task State Segment (TSS)
 *   for every processor core in the system (`core_id` 0 through `HAL_MAX_CPUS - 1`).
 *
 * Educational Step-by-Step Overview:
 *   1. Why Per-CPU GDT and TSS?
 *      In a modern Symmetric Multiprocessing (SMP) operating system, each CPU core
 *      can independently transition between Ring 3 (Userspace) and Ring 0 (Kernelspace).
 *      When an interrupt or `syscall` happens on Core N, the CPU automatically reads the
 *      `rsp0` stack pointer from Core N's active TSS (`TR` register) to allocate the
 *      new Ring 0 stack frame. If multiple cores shared one single TSS structure, concurrent
 *      interrupts on different cores would overwrite each other's stack pointers, leading to
 *      instant kernel stack corruption and triple faults (`#DF`)!
 *
 *   2. The 64-Bit System Descriptor Split (`0x28` and `0x30`):
 *      Standard code/data segment descriptors (`CS`, `DS`) take up exactly 1 GDT slot (8 bytes).
 *      However, the x86_64 architecture mandates that System Segment Descriptors (like the TSS)
 *      are 16 bytes wide (`gdt_system_entry_t`) so they can encode a full 64-bit base pointer.
 *      Therefore, our TSS descriptor occupies two consecutive 8-byte slots (`0x28` and `0x30`).
 *      When loading the Task Register with `ltr ax`, we pass selector `0x28` (`Slot 5 * 8`),
 *      and the CPU automatically reads both Slot 5 and Slot 6!
 * =========================================================================================
 */

#include "../include/hal.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"

/* =========================================================================================
 * PER-CPU DESCRIPTOR TABLE ARRAYS
 * ========================================================================================= */

/*
 * Each CPU core receives 7 GDT slots (56 bytes total):
 *   Slot 0 (0x00): Null Descriptor
 *   Slot 1 (0x08): Kernel 64-Bit Code (`DPL=0`, `L=1`)
 *   Slot 2 (0x10): Kernel 64-Bit Data (`DPL=0`, `W=1`)
 *   Slot 3 (0x18): User 64-Bit Data   (`DPL=3`, `W=1`)
 *   Slot 4 (0x20): User 64-Bit Code   (`DPL=3`, `L=1`)
 *   Slot 5 (0x28): TSS System Descriptor Low 8 Bytes (`Type=0x9`)
 *   Slot 6 (0x30): TSS System Descriptor High 8 Bytes (`base_upper`)
 */
static hal_gdt_entry_t g_cpu_gdt[HAL_MAX_CPUS][7] __attribute__((aligned(16)));
static hal_gdt_ptr_t   g_cpu_gdt_ptr[HAL_MAX_CPUS];
static hal_tss_t       g_cpu_tss[HAL_MAX_CPUS] __attribute__((aligned(16)));

/* Track the currently active core's ID via simple lookup or APIC (defaulting Core 0 during early boot) */
static volatile uint32_t g_active_core_count = 0;

/* =========================================================================================
 * INTERNAL HELPER: ENCODE AN 8-BYTE GDT ENTRY
 * ========================================================================================= */
static void hal_gdt_set_entry(hal_gdt_entry_t *entry, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity)
{
    entry->base_low    = (uint16_t)(base & 0xFFFF);
    entry->base_middle = (uint8_t)((base >> 16) & 0xFF);
    entry->base_high   = (uint8_t)((base >> 24) & 0xFF);

    entry->limit_low   = (uint16_t)(limit & 0xFFFF);
    entry->granularity = (uint8_t)((limit >> 16) & 0x0F);
    entry->granularity |= (granularity & 0xF0);

    entry->access      = access;
}

/* =========================================================================================
 * INTERNAL HELPER: ENCODE A 16-BYTE TSS SYSTEM DESCRIPTOR
 * ========================================================================================= */
static void hal_gdt_set_tss_descriptor(hal_gdt_entry_t *gdt_slots, uintptr_t tss_base, uint32_t tss_limit)
{
    /* Slot 5 (0x28): Lower 8 bytes (matches standard segment descriptor layout) */
    hal_gdt_set_entry(&gdt_slots[5], (uint32_t)(tss_base & 0xFFFFFFFFUL), tss_limit,
                      0x89, /* Present (bit 7), DPL=0, Type 0x9 (64-Bit Available TSS) */
                      0x00  /* Byte granularity */);

    /* Slot 6 (0x30): Upper 8 bytes (`base_upper` and reserved zeroes) */
    hal_gdt_system_entry_t *sys_entry = (hal_gdt_system_entry_t *)&gdt_slots[5];
    sys_entry->base_upper = (uint32_t)((tss_base >> 32) & 0xFFFFFFFFUL);
    sys_entry->reserved   = 0x0;
}

/* =========================================================================================
 * CORE GDT & TSS INITIALIZATION
 * ========================================================================================= */

void hal_gdt_init_cpu(uint32_t core_id, uintptr_t kernel_stack_top)
{
    if (core_id >= HAL_MAX_CPUS) {
        kprintf("[HAL-GDT] CRITICAL: Core ID %u exceeds HAL_MAX_CPUS (%u)!\n", core_id, HAL_MAX_CPUS);
        return;
    }

    hal_gdt_entry_t *gdt = g_cpu_gdt[core_id];
    hal_tss_t       *tss = &g_cpu_tss[core_id];

    /* Zero out the entire TSS structure to ensure clear initialization */
    memset(tss, 0, sizeof(hal_tss_t));

    /*
     * Configure `rsp0` inside the TSS:
     * When any Ring 3 process triggers an interrupt/syscall on this CPU core, the processor
     * loads `%rsp` directly from `tss->rsp0`.
     */
    tss->rsp0 = kernel_stack_top;

    /*
     * Configure I/O Permission Bitmap (IOPB) offset:
     * Setting `iopb_offset` equal to `sizeof(hal_tss_t)` indicates that no I/O bitmap
     * exists, disabling direct userspace `inb`/`outb` instructions (enforcing strict ring 0 control).
     */
    tss->iopb_offset = sizeof(hal_tss_t);

    /* Slot 0 (0x00): Mandatory Null Descriptor */
    hal_gdt_set_entry(&gdt[0], 0, 0, 0, 0);

    /*
     * Slot 1 (0x08): Kernel 64-Bit Code Segment
     *   Access: `0x9A` -> Present (`P=1`), Ring 0 (`DPL=00`), Executable Code (`E=1`), Readable (`R=1`)
     *   Granularity: `0x20` -> Long Mode Active (`L=1`), Default 32-Bit Size Cleared (`D=0`)
     */
    hal_gdt_set_entry(&gdt[1], 0, 0xFFFFF, 0x9A, 0x20);

    /*
     * Slot 2 (0x10): Kernel 64-Bit Data Segment
     *   Access: `0x92` -> Present (`P=1`), Ring 0 (`DPL=00`), Writable Data (`W=1`)
     *   Granularity: `0x00` -> Data segments in 64-bit Long Mode ignore limits/L-bits
     */
    hal_gdt_set_entry(&gdt[2], 0, 0xFFFFF, 0x92, 0x00);

    /*
     * Slot 3 (0x18): User 64-Bit Data Segment
     *   Access: `0xF2` -> Present (`P=1`), Ring 3 (`DPL=11` = 3), Writable Data (`W=1`)
     */
    hal_gdt_set_entry(&gdt[3], 0, 0xFFFFF, 0xF2, 0x00);

    /*
     * Slot 4 (0x20): User 64-Bit Code Segment
     *   Access: `0xFA` -> Present (`P=1`), Ring 3 (`DPL=11` = 3), Executable Code (`E=1`), Readable (`R=1`)
     *   Granularity: `0x20` -> Long Mode Active (`L=1`), Default 32-Bit Size Cleared (`D=0`)
     */
    hal_gdt_set_entry(&gdt[4], 0, 0xFFFFF, 0xFA, 0x20);

    /* Slot 5 & 6 (0x28 & 0x30): 16-Byte System TSS Descriptor */
    uintptr_t tss_addr  = (uintptr_t)tss;
    uint32_t  tss_limit = sizeof(hal_tss_t) - 1;
    hal_gdt_set_tss_descriptor(gdt, tss_addr, tss_limit);

    /* Configure GDTR pointer structure for this core */
    g_cpu_gdt_ptr[core_id].limit = (sizeof(hal_gdt_entry_t) * 7) - 1;
    g_cpu_gdt_ptr[core_id].base  = (uintptr_t)gdt;

    /*
     * Execute assembly stub `hal_gdt_flush(&ptr)`:
     * Loads GDTR register, performs a far jump to reload `%cs` with selector `0x08`,
     * reloads `%ds/%es/%fs/%gs/%ss` with selector `0x10`, and executes `ltr 0x28`.
     */
    hal_gdt_flush(&g_cpu_gdt_ptr[core_id]);

    if (core_id + 1 > g_active_core_count) {
        g_active_core_count = core_id + 1;
    }

    kprintf("[HAL-GDT] Core %u GDT & 64-bit TSS initialized (RSP0=0x%lx, TSS Base=0x%lx)\n",
            core_id, kernel_stack_top, tss_addr);
}

void hal_gdt_init(void)
{
    /*
     * Bootstrap Processor (Core 0) Initialization:
     * Read the current stack pointer (`RSP`) as the early kernel stack base.
     */
    uintptr_t current_rsp;
    asm volatile("mov %%rsp, %0" : "=r"(current_rsp));

    /* Align down to 16-byte boundary to strictly satisfy System V AMD64 ABI */
    current_rsp &= ~0xFUL;

    hal_gdt_init_cpu(0, current_rsp);
}

/* =========================================================================================
 * RUNTIME STACK & IST MANAGEMENT FUNCTIONS
 * ========================================================================================= */

void hal_gdt_set_kernel_stack(uintptr_t stack_top)
{
    /* For now, update Core 0 (or read core ID if APIC is initialized) */
    uint32_t core_id = 0; /* Can be expanded to `apic_get_id()` once IRQ/APIC module is linked */
    if (core_id < HAL_MAX_CPUS) {
        g_cpu_tss[core_id].rsp0 = stack_top;
    }
}

void hal_gdt_set_ist(int index, uintptr_t stack_top)
{
    if (index < 1 || index > 7) {
        kprintf("[HAL-GDT] ERROR: IST index %d out of bounds (must be 1..7)!\n", index);
        return;
    }
    uint32_t core_id = 0; /* Core 0 default */
    if (core_id < HAL_MAX_CPUS) {
        g_cpu_tss[core_id].ist[index - 1] = stack_top;
        kprintf("[HAL-GDT] Core %u IST[%d] set to 0x%lx\n", core_id, index, stack_top);
    }
}
