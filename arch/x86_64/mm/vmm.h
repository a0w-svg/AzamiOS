/* ============================================================================
 * AzamiOS — Virtual Memory Manager (x86_64 4-level paging)
 * File: arch/x86_64/mm/vmm.h
 *
 * The VMM manages page table construction and virtual-to-physical mappings.
 * It works on top of the PMM (buddy allocator) for physical page allocation.
 *
 * Virtual memory layout enforced by this module:
 *
 *   0x0000_0000_0000_0000 – 0x0000_7FFF_FFFF_FFFF  User space (128 TB)
 *   0xFFFF_8000_0000_0000 – 0xFFFF_BFFF_FFFF_FFFF  HHDM: direct physical map
 *   0xFFFF_FFFF_8000_0000 – 0xFFFF_FFFF_FFFF_FFFF  Kernel image (-2 GB)
 *
 * Page table flags (PTE bits):
 *   VMM_F_PRESENT   (1<<0)  Page is present
 *   VMM_F_WRITE     (1<<1)  Page is writable
 *   VMM_F_USER      (1<<2)  Page is accessible from ring 3
 *   VMM_F_PWT       (1<<3)  Page write-through
 *   VMM_F_PCD       (1<<4)  Page cache disable
 *   VMM_F_ACCESSED  (1<<5)  Set by CPU on access
 *   VMM_F_DIRTY     (1<<6)  Set by CPU on write
 *   VMM_F_HUGE      (1<<7)  Huge page (2 MB at PD level, 1 GB at PDPT level)
 *   VMM_F_GLOBAL    (1<<8)  Global page (not flushed on CR3 reload)
 *   VMM_F_NX        (1<<63) No-execute (requires EFER.NXE=1)
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"

/* ── PTE flag bits ─────────────────────────────────────────────────────────── */
#define VMM_F_PRESENT   (1ULL << 0)
#define VMM_F_WRITE     (1ULL << 1)
#define VMM_F_USER      (1ULL << 2)
#define VMM_F_PWT       (1ULL << 3)
#define VMM_F_PCD       (1ULL << 4)
#define VMM_F_ACCESSED  (1ULL << 5)
#define VMM_F_DIRTY     (1ULL << 6)
#define VMM_F_HUGE      (1ULL << 7)
#define VMM_F_GLOBAL    (1ULL << 8)
#define VMM_F_SHARED    (1ULL << 9)
#define VMM_F_NX        (1ULL << 63)

/* Mask to extract the physical address from a PTE (bits 12–51). */
#define VMM_PHYS_MASK   0x000FFFFFFFFFF000ULL

/* Common flag combinations */
#define VMM_KERNEL_RX   (VMM_F_PRESENT | VMM_F_GLOBAL)
#define VMM_KERNEL_RW   (VMM_F_PRESENT | VMM_F_WRITE  | VMM_F_GLOBAL | VMM_F_NX)
#define VMM_USER_RO     (VMM_F_PRESENT | VMM_F_USER)
#define VMM_USER_RW     (VMM_F_PRESENT | VMM_F_WRITE  | VMM_F_USER   | VMM_F_NX)
#define VMM_USER_RX     (VMM_F_PRESENT | VMM_F_USER)
#define VMM_F_WC        (VMM_F_PWT) /* Write-Combining caching via PAT PA1/PA5 */
#define VMM_USER_WC     (VMM_F_PRESENT | VMM_F_WRITE  | VMM_F_USER   | VMM_F_NX | VMM_F_WC)
#define VMM_MMIO        (VMM_F_PRESENT | VMM_F_WRITE  | VMM_F_NX | VMM_F_PCD | VMM_F_PWT)

/* ── PML4 index extraction from a virtual address ───────────────────────────── */
#define VMM_PML4_IDX(va)  (((virt_addr_t)(va) >> 39) & 0x1FFUL)
#define VMM_PDPT_IDX(va)  (((virt_addr_t)(va) >> 30) & 0x1FFUL)
#define VMM_PD_IDX(va)    (((virt_addr_t)(va) >> 21) & 0x1FFUL)
#define VMM_PT_IDX(va)    (((virt_addr_t)(va) >> 12) & 0x1FFUL)

/* ── Address space handle (= physical address of the PML4 table) ────────────── */
typedef phys_addr_t vmm_space_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * vmm_init() — Set up the kernel's canonical page table.
 *
 * Maps:
 *   1. The kernel ELF (text RX, data/bss RW, rodata R) at -2 GB.
 *   2. The HHDM: all physical RAM at HHDM_BASE.
 *   3. Installs the new CR3 and enables SMEP/SMAP via CR4.
 *
 * @hhdm_base   HHDM virtual address reported by Limine.
 * @phys_base   Physical base address of the loaded kernel.
 * @virt_base   Virtual base address of the loaded kernel.
 * @memmap      Limine memory-map response (to enumerate RAM for HHDM).
 */
void vmm_init(u64 hhdm_base, u64 phys_base, u64 virt_base, void *memmap);

/**
 * vmm_map(space, virt, phys, flags) — Map one 4 KB page.
 *
 * Creates intermediate page table levels as needed (allocating from PMM).
 * @space   PML4 physical address (or 0 for the kernel address space).
 * @virt    Virtual address (must be 4 KB aligned).
 * @phys    Physical address (must be 4 KB aligned).
 * @flags   Combination of VMM_F_* flag bits.
 * Returns 0 on success, -1 on allocation failure.
 */
int vmm_map(vmm_space_t space, virt_addr_t virt, phys_addr_t phys, u64 flags);

/**
 * vmm_unmap(space, virt) — Unmap one 4 KB page and invalidate the TLB entry.
 */
void vmm_unmap(vmm_space_t space, virt_addr_t virt);

/**
 * vmm_translate(space, virt) → physical address, or 0 if not mapped.
 */
phys_addr_t vmm_translate(vmm_space_t space, virt_addr_t virt);

/**
 * vmm_query_flags(space, virt) → leaf PTE flag bits (VMM_F_*), or 0 if the
 * page is not present. The physical address bits are masked out.
 */
u64 vmm_query_flags(vmm_space_t space, virt_addr_t virt);

/**
 * vmm_create_space() — Allocate a new page table and copy the kernel half.
 *
 * PML4 indices 256–511 (kernel half) are shared with the kernel's PML4.
 * PML4 indices 0–255 (user half) start empty.
 *
 * Returns the physical address of the new PML4, or 0 on failure.
 */
vmm_space_t vmm_create_space(void);

/**
 * vmm_clone_space(src) — Deep-copy the user half of an address space.
 *
 * Copies all user PML4 entries (0–255) and all subordinate page tables.
 * Physical pages with the WRITE flag set are duplicated (copy-on-write is a
 * future enhancement; for now full copy).
 * Returns new PML4 physical address, or 0 on failure.
 */
vmm_space_t vmm_clone_space(vmm_space_t src);

/**
 * vmm_destroy_space(space) — Free all user-space page tables and physical pages.
 */
void vmm_destroy_space(vmm_space_t space);

/**
 * vmm_switch(space) — Load the given PML4 into CR3 (switches address space).
 */
static inline void vmm_switch(vmm_space_t space)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"((u64)space) : "memory");
}

/** vmm_kernel_space() — Returns the physical address of the kernel's PML4. */
vmm_space_t vmm_kernel_space(void);

/** vmm_map_io(phys, size) — Map physical MMIO/IO range and return virtual address. */
void *vmm_map_io(phys_addr_t phys, size_t size);

/**
 * vmm_set_flags(space, virt, count, flags) — Update PTE protection flags for a page range.
 * Used by sys_mprotect.
 */
int vmm_set_flags(vmm_space_t space, virt_addr_t virt, size_t count, u64 flags);
