/*
 * =========================================================================================
 * Azami OS — Hardware Abstraction Layer (HAL) Master Interface
 * File: /kernel/hal/include/hal.h
 * Architecture: x86_64 (64-Bit Long Mode exclusively)
 *
 * Description:
 *   This header defines the core interfaces between the Azami OS microkernel logic
 *   and the underlying physical x86_64 hardware. The HAL is strictly decoupled from
 *   high-level kernel memory management and process scheduling, providing clean,
 *   orthogonal primitives for CPU control, segment tables (GDT/TSS), interrupts (IDT),
 *   hardware timers (PIT/HPET), and basic console I/O.
 *
 * Educational Notes on x86_64 Long Mode Descriptors:
 *   1. Standard Code/Data Segment Descriptors (8 bytes):
 *      In 64-bit Long Mode (`x86_64`), segmentation is mostly disabled by hardware. For
 *      code and data segments (`CS`, `DS`, `SS`, `ES`, `FS`, `GS`), the hardware ignores the
 *      base address and limit checks (treating base as 0x0 and limit as 2^64 - 1).
 *      However, the access byte (`DPL` privilege checks, Code/Data type bit, Long Mode `L` bit)
 *      is strictly enforced!
 *
 *   2. System Segment Descriptors — TSS & LDT (16 bytes):
 *      Unlike standard code/data segments, System Descriptors (such as the Task State
 *      Segment descriptor loaded by `ltr`) must hold a full 64-bit base pointer in Long Mode.
 *      Therefore, the x86_64 specification expands system descriptors from 8 bytes to 16 bytes.
 *      The lower 8 bytes match a normal descriptor, while the upper 8 bytes store bits [32..63]
 *      of the 64-bit base address (`base_upper`) and reserved/zero bits.
 * =========================================================================================
 */

#ifndef AZAMI_HAL_H
#define AZAMI_HAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* =========================================================================================
 * 1. ARCHITECTURAL & MSR CONSTANTS
 * ========================================================================================= */

#define HAL_MAX_CPUS        16U
#define HAL_PAGE_SIZE       4096UL

/* Standard x86_64 Model-Specific Registers (MSRs) */
#define HAL_MSR_IA32_APIC_BASE  0x0000001B
#define HAL_MSR_IA32_EFER       0xC0000080
#define HAL_MSR_IA32_STAR       0xC0000081
#define HAL_MSR_IA32_LSTAR      0xC0000082
#define HAL_MSR_IA32_FMASK      0xC0000084
#define HAL_MSR_IA32_FS_BASE    0xC0000100
#define HAL_MSR_IA32_GS_BASE    0xC0000101
#define HAL_MSR_IA32_KERNEL_GS  0xC0000102

/* EFER (Extended Feature Enable Register) Bits */
#define HAL_EFER_SCE            (1ULL << 0)  /* System Call Extensions (SYSCALL/SYSRET) */
#define HAL_EFER_LME            (1ULL << 8)  /* Long Mode Enable */
#define HAL_EFER_LMA            (1ULL << 10) /* Long Mode Active (hardware status bit) */
#define HAL_EFER_NXE            (1ULL << 11) /* No-Execute Enable (page protection bit 63) */

/* CR4 Control Register Bits */
#define HAL_CR4_PAE             (1UL << 5)   /* Physical Address Extension (required for PML4) */
#define HAL_CR4_PGE             (1UL << 7)   /* Page Global Enable (global TLB preservation) */
#define HAL_CR4_OSFXSR          (1UL << 9)   /* OS Support for FXSAVE and FXRSTOR instructions */
#define HAL_CR4_OSXMMEXCPT      (1UL << 10)  /* OS Support for Unmasked SIMD Floating-Point Exceptions */
#define HAL_CR4_FSGSBASE        (1UL << 16)  /* Enable RDFSBASE/WRFSBASE/RDGSBASE/WRGSBASE */
#define HAL_CR4_OSXSAVE         (1UL << 18)  /* XSAVE and Processor Extended States Enable */

/* GDT Segment Selector Byte Offsets */
#define HAL_GDT_SEL_NULL        0x00
#define HAL_GDT_SEL_KERNEL_CS   0x08  /* Ring 0 Code Segment (64-Bit Long Mode, DPL 0) */
#define HAL_GDT_SEL_KERNEL_DS   0x10  /* Ring 0 Data Segment (DPL 0) */
#define HAL_GDT_SEL_USER_DS     0x18  /* Ring 3 Data Segment (DPL 3, RPL 3 = 0x1B) */
#define HAL_GDT_SEL_USER_CS     0x20  /* Ring 3 Code Segment (64-Bit Long Mode, DPL 3, RPL 3 = 0x23) */
#define HAL_GDT_SEL_TSS         0x28  /* 16-Byte System TSS Descriptor Base Selector */

/* =========================================================================================
 * 2. GLOBAL DESCRIPTOR TABLE (GDT) & TSS STRUCTURES
 * ========================================================================================= */

/*
 * Standard 8-Byte GDT Entry (Code & Data Segments)
 *
 * Byte Layout:
 *   [0..1] limit_low     : Bits [0..15] of the segment limit
 *   [2..3] base_low      : Bits [0..15] of the segment base address
 *   [4]    base_middle   : Bits [16..23] of the segment base address
 *   [5]    access        : Segment access byte (P | DPL[1..0] | S | Type[3..0])
 *   [6]    granularity   : Limit [16..19] + flags (G | D/B | L | AVL)
 *   [7]    base_high     : Bits [24..31] of the segment base address
 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed)) hal_gdt_entry_t;

/*
 * 16-Byte System GDT Entry (Used exclusively for TSS & LDT in 64-Bit Long Mode)
 *
 * In addition to the normal 8 bytes, bytes [8..15] store the upper 32 bits of
 * the 64-bit base address, ensuring system segment bases can reside anywhere in
 * the 64-bit canonical address space.
 */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;    /* Bits [32..63] of the 64-bit segment base address */
    uint32_t reserved;      /* Must be 0x0 */
} __attribute__((packed)) hal_gdt_system_entry_t;

/*
 * GDTR Register Pointer Structure
 * Used by the `lgdt` instruction to load the GDT limit and base address into the CPU.
 */
typedef struct {
    uint16_t limit;         /* Size of the GDT in bytes minus 1 */
    uintptr_t base;         /* 64-bit linear base address of the GDT array */
} __attribute__((packed)) hal_gdt_ptr_t;

/*
 * Task State Segment (TSS) Structure for x86_64 Long Mode
 *
 * In 64-bit mode, the TSS is no longer used for hardware task switching (context switching
 * is handled entirely in software via registers). Instead, the TSS serves two vital purposes:
 *   1. Stack Privilege Transitions (`rsp0`, `rsp1`, `rsp2`): When a user process (ring 3)
 *      triggers a system call or interrupt, the CPU reads `rsp0` from this TSS to switch
 *      the stack pointer (`RSP`) to the secure ring-0 kernel stack.
 *   2. Interrupt Stack Table (`ist` [1..7]): Provides up to 7 dedicated, isolated stacks
 *      for critical exceptions (e.g., Double Fault `#DF` or Non-Maskable Interrupt `NMI`)
 *      to guarantee the CPU can handle the fault even if the normal kernel stack is corrupted or overflowed.
 */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;          /* Ring 0 kernel stack pointer (loaded on ring 3 -> ring 0 transition) */
    uint64_t rsp1;          /* Ring 1 stack pointer (unused in standard kernels) */
    uint64_t rsp2;          /* Ring 2 stack pointer (unused in standard kernels) */
    uint64_t reserved1;
    uint64_t ist[7];        /* Interrupt Stack Table (IST1 through IST7) 64-bit stack pointers */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;   /* I/O Permission Bitmap offset from TSS base (sizeof(TSS) if disabled) */
} __attribute__((packed)) hal_tss_t;

/* =========================================================================================
 * 3. HAL CPU CONTROL FUNCTION PROTOTYPES (`kernel/hal/cpu/`)
 * ========================================================================================= */

/*
 * hal_cpu_init:
 *   Verifies 64-bit CPU features (`LM` Long Mode bit), configures `CR4` to enable
 *   SSE/AVX SIMD operations, and verifies model-specific capabilities.
 */
void hal_cpu_init(void);

/* Assembly helpers for low-level processor instruction wrappers */
void hal_cpu_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx);
uint64_t hal_cpu_rdmsr(uint32_t msr);
void hal_cpu_wrmsr(uint32_t msr, uint64_t value);

uint64_t hal_cpu_get_cr0(void);
void hal_cpu_set_cr0(uint64_t val);
uint64_t hal_cpu_get_cr2(void);
uint64_t hal_cpu_get_cr3(void);
void hal_cpu_set_cr3(uint64_t val);
uint64_t hal_cpu_get_cr4(void);
void hal_cpu_set_cr4(uint64_t val);

static inline void hal_cpu_enable_interrupts(void)  { asm volatile("sti" ::: "memory"); }
static inline void hal_cpu_disable_interrupts(void) { asm volatile("cli" ::: "memory"); }
static inline void hal_cpu_halt(void)               { asm volatile("hlt" ::: "memory"); }
static inline void hal_cpu_relax(void)              { asm volatile("pause" ::: "memory"); }

/* =========================================================================================
 * 4. HAL GDT / TSS FUNCTION PROTOTYPES (`kernel/hal/gdt/`)
 * ========================================================================================= */

/* Initializes the Global Descriptor Table and Task State Segment for Core 0 (BSP) */
void hal_gdt_init(void);

/* Initializes the GDT and TSS for Application Processors (SMP Cores) */
void hal_gdt_init_cpu(uint32_t core_id, uintptr_t kernel_stack_top);

/* Updates `tss.rsp0` on the current core (`core_id`) so ring 3 -> ring 0 transitions use the given stack */
void hal_gdt_set_kernel_stack(uintptr_t stack_top);

/* Configures an Interrupt Stack Table (`ist` [1..7]) entry inside the current core's TSS */
void hal_gdt_set_ist(int index, uintptr_t stack_top);

/* Assembly stub: flushes GDTR, reloads CS via far lretq, reloads data segments, and executes ltr */
extern void hal_gdt_flush(hal_gdt_ptr_t *ptr);

#endif /* AZAMI_HAL_H */
