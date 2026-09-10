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
#include "../cpu/mitigations.h"

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
/* Software-only COW marker: this PTE shares a physical frame with at least one
 * other mapping. On a write fault the page-fault handler copies the frame and
 * promotes the copy to writable. Sits in available PTE bit 10 — the hardware
 * ignores all of bits 9–11 on leaf 4-KiB PTEs. */
#define VMM_F_COW       (1ULL << 10)
#define VMM_F_NX        (1ULL << 63)

/* Memory-protection keys (CR4.PKE). Bits 62:59 of a leaf PTE name one of 16
 * keys; PKRU then gates read/write access per key for user-accessible pages.
 * VMM_F_PKEY_SET is an internal marker on the *flags argument* of
 * vmm_set_flags(), not a hardware bit: it distinguishes "apply the key in bits
 * 62:59" (pkey_mprotect) from "leave whatever key the page already has"
 * (plain mprotect), including the case of assigning key 0. It is stripped
 * before the PTE is written. */
#define VMM_PKEY_SHIFT  59
#define VMM_PKEY_MASK   (0xFULL << VMM_PKEY_SHIFT)
#define VMM_F_PKEY(k)   (((u64)(k) & 0xF) << VMM_PKEY_SHIFT)
#define VMM_F_PKEY_SET  (1ULL << 58)

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
 * vmm_unmap_range(space, virt, count, free_frames) — Tear down `count`
 * consecutive 4 KB mappings starting at `virt`.
 *
 * Prefer this over a loop of vmm_unmap_get(): each unmap that removes a live
 * translation owes the other CPUs a TLB shootdown, and doing that once per page
 * turns unmapping a large region into thousands of IPI round-trips. This
 * batches the page-table edits and pays for one shootdown per chunk.
 *
 * With `free_frames` set, a page whose old PTE was user-owned and not
 * VMM_F_SHARED is returned to the PMM — after the shootdown, never before.
 * Returns the number of frames freed.
 */
size_t vmm_unmap_range(vmm_space_t space, virt_addr_t virt, size_t count, bool free_frames);

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
 * vmm_unmap_get(space, virt) — Clear the leaf PTE, flush TLB, and return the
 * previous PTE value (containing physical address and flags). Returns 0 if unmapped.
 */
u64 vmm_unmap_get(vmm_space_t space, virt_addr_t virt);

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
 *
 * Where the Spectre-v2 policy calls for it (no enhanced IBRS on this part), an
 * address-space change is also the boundary at which the indirect-branch
 * predictor must be flushed, so the outgoing process cannot steer the
 * incoming one's indirect branches. The check is a byte load against a global
 * the branch predictor learns immediately, so a machine that does not need the
 * barrier pays nothing for the test — which is why the CR3 read that the
 * barrier needs sits *inside* the guarded path.
 */
static inline void vmm_switch(vmm_space_t space)
{
    if (__builtin_expect(g_ibpb_on_switch != 0, 0)) {
        u64 prev;
        __asm__ volatile("mov %%cr3, %0" : "=r"(prev));
        mitigations_switch_mm(prev & ~0xFFFULL, (u64)space & ~0xFFFULL);
    }
    /* Legacy callers load a bare PML4: PCID 0, and the write flushes that
     * context's non-global entries. Used for the kernel address space, which
     * lives in PCID 0 and whose pages are global anyway. */
    __asm__ volatile("mov %0, %%cr3" : : "r"((u64)space & VMM_PHYS_MASK) : "memory");
}

extern u8 g_pcid_enabled;   /* cpu.c — CR4.PCIDE is live */

/**
 * vmm_switch_proc(pml4, pcid, cpu, primed_mask) — switch to a process address
 * space, tagged with its PCID so the CPU keeps that space's TLB entries across
 * the switch instead of flushing the whole non-global TLB every time.
 *
 * @primed_mask is a per-address-space bitmask, one bit per CPU. The first
 * switch to this space on a given core is a *flushing* load (bit clear ->
 * clears any entries a previous owner of the same PCID number left behind),
 * every switch after that sets CR3[63] so the entries are kept. A cross-CPU
 * TLB shootdown flushes every PCID (INVPCID all-contexts), so a stale mapping
 * can never outlive an unmap.
 */
static inline void vmm_switch_proc(phys_addr_t pml4, u32 pcid, u32 cpu,
                                   u64 *primed_mask)
{
    u64 phys = (u64)pml4 & VMM_PHYS_MASK;

    if (__builtin_expect(g_ibpb_on_switch != 0, 0)) {
        u64 prev;
        __asm__ volatile("mov %%cr3, %0" : "=r"(prev));
        mitigations_switch_mm(prev & ~0xFFFULL, phys);
    }

    if (!g_pcid_enabled) {
        __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
        return;
    }

    u64 cr3 = phys | (pcid & 0xFFFu);
    if (cpu < 64 && primed_mask) {
        u64 bit = 1ULL << cpu;
        if (*primed_mask & bit) {
            cr3 |= (1ULL << 63);                       /* keep this PCID's TLB */
        } else {
            __atomic_or_fetch(primed_mask, bit, __ATOMIC_RELAXED);  /* flush once */
        }
    }
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
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

/**
 * vmm_cow_fault(space, virt) — Handle a copy-on-write write fault.
 *
 * Called from the #PF handler when: (a) the fault is a write to a non-writable
 * page, (b) the page has VMM_F_COW set, and (c) vma_probe() confirms the VMA
 * is writable. If the physical frame's refcount drops to 1 the page is promoted
 * writable in-place; otherwise a private copy is made.
 *
 * Returns 0 on success, -ENOMEM if no frame could be allocated.
 */
int vmm_cow_fault(vmm_space_t space, virt_addr_t virt);

/**
 * vmm_page_ref_inc(phys) / vmm_page_ref_dec(phys) — Manage the per-frame
 * reference counter used by the COW implementation.
 *
 * Both are exported so vmm_clone_space() can bump counts from vmm.c and
 * higher-level code can inspect them if needed.
 */
void vmm_page_ref_inc(phys_addr_t phys);
uint16_t vmm_page_ref_dec(phys_addr_t phys);
