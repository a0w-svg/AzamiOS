/* ============================================================================
 * AzamiOS — 4-Level VMM Implementation (x86_64)
 * File: arch/x86_64/mm/vmm.c
 *
 * Design decisions vs old paging.c:
 *   1. HHDM: all physical pages are accessed through PHYS_TO_VIRT().
 *      pmm_alloc returns a physical address; we convert it to a virtual
 *      pointer immediately. No "physical == virtual" assumption anywhere.
 *   2. FULL 4-level walk: vmm_map/vmm_translate/vmm_clone all traverse
 *      PML4 → PDPT → PD → PT properly, supporting the full 256 TB user VA space.
 *   3. vmm_clone_space(): replaces the old hard-coded 4-PDPT hack.
 *      It walks all 256 user-half PML4 entries instead of assuming 4 entries.
 *   4. Kernel half sharing: the kernel PML4[256..511] entries are shared
 *      (not copied) in child spaces, so kernel mappings are always visible.
 *   5. NX bit: data/stack pages mapped NX by default.
 *   6. TLB shootdown: after any unmap on the kernel space, invlpg is called.
 *      Cross-CPU shootdown hooks are provided for the SMP layer.
 * ============================================================================ */

#include "vmm.h"
#include "../cpu/msr.h"
#include "../cpu/spinlock.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"
#include "../boot/limine.h"
#include "../../../kernel/lib/string.h"

/* ── Kernel PML4 physical address ─────────────────────────────────────────── */
static vmm_space_t g_kernel_pml4 = 0;

/* Global hardware protection flags */
u8 g_smep_enabled = 0;
u8 g_smap_enabled = 0;

/* ── Global VMM lock (protects kernel page table modifications) ─────────────── */
static spinlock_t g_vmm_lock = SPINLOCK_INIT;

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Return a pointer to a PTE table given its physical address via HHDM. */
static inline u64 *phys_to_table(phys_addr_t p) {
    if (p >= 0xffff800000000000ULL) p = VIRT_TO_PHYS(p);
    return (u64 *)PHYS_TO_VIRT(p & VMM_PHYS_MASK);
}

/* Allocate a zeroed 4 KB page table level (returns physical address). */
static phys_addr_t alloc_table(void)
{
    phys_addr_t p = pmm_alloc_page();
    if (!p) return 0;
    u64 *t = phys_to_table(p);
    for (int i = 0; i < 512; i++) t[i] = 0;
    return p;
}

/* Get or create the next level table.
 * @entry    Pointer to the parent PTE.
 * @flags    Flags to apply if creating a new entry (typically PRESENT|WRITE|USER).
 * Returns pointer to the child table, or NULL on allocation failure. */
static u64 *get_or_create(u64 *entry, u64 flags)
{
    if (*entry & VMM_F_PRESENT) {
        return phys_to_table(*entry & VMM_PHYS_MASK);
    }
    phys_addr_t new_phys = alloc_table();
    if (!new_phys) return NULL;
    *entry = new_phys | flags | VMM_F_PRESENT;
    return phys_to_table(new_phys);
}

/* ── Core mapping function ────────────────────────────────────────────────── */

int vmm_map(vmm_space_t space, virt_addr_t virt, phys_addr_t phys, u64 flags)
{
    if (!space) space = g_kernel_pml4;

    /* Intermediate tables need PRESENT+WRITE+USER so user processes can
     * traverse them (actual access control is enforced at the leaf PTE). */
    const u64 table_flags = VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER;

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    u64 *pml4 = phys_to_table(space);
    u64 *pdpt = get_or_create(&pml4[VMM_PML4_IDX(virt)], table_flags);
    if (!pdpt) { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    u64 *pd   = get_or_create(&pdpt[VMM_PDPT_IDX(virt)], table_flags);
    if (!pd)   { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    u64 *pt   = get_or_create(&pd[VMM_PD_IDX(virt)], table_flags);
    if (!pt)   { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    /* Install the leaf PTE. */
    pt[VMM_PT_IDX(virt)] = (phys & VMM_PHYS_MASK) | flags;

    /* Invalidate the TLB entry for this VA on the current CPU. */
    invlpg(virt);

    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    return 0;
}

void vmm_unmap(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) goto out;

    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(virt)] & VMM_F_PRESENT)) goto out;

    u64 *pd   = phys_to_table(pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(virt)] & VMM_F_PRESENT)) goto out;

    u64 *pt   = phys_to_table(pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK);
    pt[VMM_PT_IDX(virt)] = 0;
    invlpg(virt);

out:
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
}

phys_addr_t vmm_translate(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pdpt[VMM_PDPT_IDX(virt)] & VMM_F_HUGE) {
        /* 1 GB huge page */
        return (pdpt[VMM_PDPT_IDX(virt)] & ~0x3FFFFFFFUL) + (virt & 0x3FFFFFFFUL);
    }

    u64 *pd   = phys_to_table(pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pd[VMM_PD_IDX(virt)] & VMM_F_HUGE) {
        /* 2 MB huge page */
        return (pd[VMM_PD_IDX(virt)] & ~0x1FFFFFUL) + (virt & 0x1FFFFFUL);
    }

    u64 *pt   = phys_to_table(pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pt[VMM_PT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    return (pt[VMM_PT_IDX(virt)] & VMM_PHYS_MASK) + (virt & 0xFFFUL);
}

/* ── Address space management ─────────────────────────────────────────────── */

vmm_space_t vmm_create_space(void)
{
    phys_addr_t new_pml4_phys = alloc_table();
    if (!new_pml4_phys) return 0;

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);
    u64 *new_pml4 = phys_to_table(new_pml4_phys);
    u64 *krn_pml4 = phys_to_table(g_kernel_pml4);

    /* Share the kernel half: PML4 entries 256–511 point to the same PDPT
     * tables as the kernel's PML4. User entries (0–255) start as zero.    */
    for (int i = 0; i < 256; i++)  new_pml4[i] = 0;
    for (int i = 256; i < 512; i++) new_pml4[i] = krn_pml4[i];
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);

    return new_pml4_phys;
}

vmm_space_t vmm_clone_space(vmm_space_t src)
{
    if (!src) src = g_kernel_pml4;
    if (src >= 0xffff800000000000ULL) src = VIRT_TO_PHYS(src);

    phys_addr_t dst_phys = alloc_table();
    if (!dst_phys) return 0;

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    u64 *src_pml4 = phys_to_table(src);
    u64 *dst_pml4 = phys_to_table(dst_phys);

    /* Share kernel half. */
    u64 *krn_pml4 = phys_to_table(g_kernel_pml4);
    for (int i = 256; i < 512; i++) dst_pml4[i] = krn_pml4[i];

    /* Deep-copy user half (PML4 entries 0–255). */
    for (int pml4i = 0; pml4i < 256; pml4i++) {
        if (!(src_pml4[pml4i] & VMM_F_PRESENT)) { dst_pml4[pml4i] = 0; continue; }

        /* Clone PDPT */
        phys_addr_t dst_pdpt_phys = alloc_table();
        if (!dst_pdpt_phys) goto oom;
        dst_pml4[pml4i] = dst_pdpt_phys | (src_pml4[pml4i] & ~VMM_PHYS_MASK);

        u64 *src_pdpt = phys_to_table(src_pml4[pml4i] & VMM_PHYS_MASK);
        u64 *dst_pdpt = phys_to_table(dst_pdpt_phys);

        for (int pdpti = 0; pdpti < 512; pdpti++) {
            if (!(src_pdpt[pdpti] & VMM_F_PRESENT)) { dst_pdpt[pdpti] = 0; continue; }
            if (src_pdpt[pdpti] & VMM_F_HUGE) {
                /* 1 GB user huge page — deep copy if writable, share r/o pages */
                if ((src_pdpt[pdpti] & VMM_F_WRITE) && (src_pdpt[pdpti] & VMM_F_USER) && !(src_pdpt[pdpti] & VMM_F_SHARED)) {
                    phys_addr_t new_phys = pmm_alloc_pages(512 * 512); /* 1 GB = 2^18 pages */
                    if (!new_phys) goto oom;
                    /* BUG-09: release lock before 1 GB memcpy; dst pages are private */
                    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                    u8 *src_pg = (u8 *)PHYS_TO_VIRT(src_pdpt[pdpti] & VMM_PHYS_MASK & ~0x3FFFFFFFUL);
                    u8 *dst_pg = (u8 *)PHYS_TO_VIRT(new_phys);
                    memcpy(dst_pg, src_pg, (size_t)PAGE_SIZE * 512 * 512);
                    irqf = spinlock_lock_irqsave(&g_vmm_lock);
                    /* Refresh pointers — src_pml4/src_pdpt may point into HHDM which is stable,
                     * but dst_pml4/dst_pdpt were captured before; they are still valid (HHDM). */
                    dst_pdpt[pdpti] = new_phys | (src_pdpt[pdpti] & ~VMM_PHYS_MASK & ~VMM_F_HUGE);
                    dst_pdpt[pdpti] |= VMM_F_HUGE;
                } else {
                    /* Read-only or SHARED: share frame; mark VMM_F_SHARED on both so destroy doesn't free it */
                    src_pdpt[pdpti] |= VMM_F_SHARED;
                    dst_pdpt[pdpti] = src_pdpt[pdpti];
                }
                continue;
            }

            /* Clone PD */
            phys_addr_t dst_pd_phys = alloc_table();
            if (!dst_pd_phys) goto oom;
            dst_pdpt[pdpti] = dst_pd_phys | (src_pdpt[pdpti] & ~VMM_PHYS_MASK);

            u64 *src_pd = phys_to_table(src_pdpt[pdpti] & VMM_PHYS_MASK);
            u64 *dst_pd = phys_to_table(dst_pd_phys);

            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(src_pd[pdi] & VMM_F_PRESENT)) { dst_pd[pdi] = 0; continue; }
                if (src_pd[pdi] & VMM_F_HUGE) {
                    /* 2 MB user huge page — deep copy if writable, share r/o pages */
                    if ((src_pd[pdi] & VMM_F_WRITE) && (src_pd[pdi] & VMM_F_USER) && !(src_pd[pdi] & VMM_F_SHARED)) {
                        phys_addr_t new_phys = pmm_alloc_pages(512); /* 2 MB = 512 pages */
                        if (!new_phys) goto oom;
                        /* BUG-09: release lock before 2 MB memcpy */
                        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                        u8 *src_pg = (u8 *)PHYS_TO_VIRT(src_pd[pdi] & VMM_PHYS_MASK & ~0x1FFFFFUL);
                        u8 *dst_pg = (u8 *)PHYS_TO_VIRT(new_phys);
                        memcpy(dst_pg, src_pg, (size_t)PAGE_SIZE * 512);
                        irqf = spinlock_lock_irqsave(&g_vmm_lock);
                        dst_pd[pdi] = new_phys | (src_pd[pdi] & ~VMM_PHYS_MASK & ~VMM_F_HUGE);
                        dst_pd[pdi] |= VMM_F_HUGE;
                    } else {
                        src_pd[pdi] |= VMM_F_SHARED;
                        dst_pd[pdi] = src_pd[pdi];
                    }
                    continue;
                }

                /* Clone PT */
                phys_addr_t dst_pt_phys = alloc_table();
                if (!dst_pt_phys) goto oom;
                dst_pd[pdi] = dst_pt_phys | (src_pd[pdi] & ~VMM_PHYS_MASK);

                u64 *src_pt = phys_to_table(src_pd[pdi] & VMM_PHYS_MASK);
                u64 *dst_pt = phys_to_table(dst_pt_phys);

                for (int pti = 0; pti < 512; pti++) {
                    if (!(src_pt[pti] & VMM_F_PRESENT)) { dst_pt[pti] = 0; continue; }

                    /* Deep-copy user pages (both writable and read-only) unless explicitly VMM_F_SHARED */
                    if ((src_pt[pti] & VMM_F_USER) && !(src_pt[pti] & VMM_F_SHARED)) {
                        phys_addr_t new_page = pmm_alloc_page();
                        if (!new_page) goto oom;
                        void *src_pg = (void *)PHYS_TO_VIRT(src_pt[pti] & VMM_PHYS_MASK);
                        void *dst_pg = (void *)PHYS_TO_VIRT(new_page);
                        memcpy(dst_pg, src_pg, PAGE_SIZE);
                        dst_pt[pti] = new_page | (src_pt[pti] & ~VMM_PHYS_MASK);
                    } else {
                        /* Kernel-mapped or explicitly SHARED (e.g. shmem): share physical frame. */
                        dst_pt[pti] = src_pt[pti];
                    }
                }
            }
        }
    }
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    return dst_phys;

oom:
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    vmm_destroy_space(dst_phys);  /* clean up partial allocation */
    return 0;
}

void vmm_destroy_space(vmm_space_t space)
{
    if (!space || space == g_kernel_pml4) return;
    if (space >= 0xffff800000000000ULL) space = VIRT_TO_PHYS(space);

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);
    u64 *pml4 = phys_to_table(space);
    for (int pml4i = 0; pml4i < 256; pml4i++) {  /* User half only */
        if (!(pml4[pml4i] & VMM_F_PRESENT)) continue;
        u64 *pdpt = phys_to_table(pml4[pml4i] & VMM_PHYS_MASK);
        for (int pdpti = 0; pdpti < 512; pdpti++) {
            if (!(pdpt[pdpti] & VMM_F_PRESENT)) continue;
            if (pdpt[pdpti] & VMM_F_HUGE) {
                /* 1 GB user huge page — free if not shared */
                if ((pdpt[pdpti] & VMM_F_USER) && !(pdpt[pdpti] & VMM_F_SHARED))
                    pmm_free_pages(pdpt[pdpti] & VMM_PHYS_MASK & ~0x3FFFFFFFUL, 512 * 512);
                continue;
            }
            u64 *pd = phys_to_table(pdpt[pdpti] & VMM_PHYS_MASK);
            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(pd[pdi] & VMM_F_PRESENT)) continue;
                if (pd[pdi] & VMM_F_HUGE) {
                    /* 2 MB user huge page — free if not shared */
                    if ((pd[pdi] & VMM_F_USER) && !(pd[pdi] & VMM_F_SHARED))
                        pmm_free_pages(pd[pdi] & VMM_PHYS_MASK & ~0x1FFFFFUL, 512);
                    continue;
                }
                u64 *pt = phys_to_table(pd[pdi] & VMM_PHYS_MASK);
                for (int pti = 0; pti < 512; pti++) {
                    if ((pt[pti] & VMM_F_PRESENT) && (pt[pti] & VMM_F_USER)) {
                        if (!(pt[pti] & VMM_F_SHARED)) {
                            pmm_free_page(pt[pti] & VMM_PHYS_MASK);
                        }
                    }
                }
                pmm_free_page(pd[pdi] & VMM_PHYS_MASK);
            }
            pmm_free_page(pdpt[pdpti] & VMM_PHYS_MASK);
        }
        pmm_free_page(pml4[pml4i] & VMM_PHYS_MASK);
    }
    pmm_free_page(space);
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
}

/* ── Initialisation ───────────────────────────────────────────────────────── */

void vmm_init(u64 hhdm_base, u64 phys_base, u64 virt_base, void *memmap_raw)
{
    (void)hhdm_base; (void)phys_base; (void)virt_base;

    /* Use the current CR3 — Limine already built a valid higher-half page table.
     * We record it as our kernel PML4 and will manage it going forward. */
    g_kernel_pml4 = (vmm_space_t)(read_cr3() & VMM_PHYS_MASK);

    kprintf("[VMM] Kernel PML4 at physical 0x%016llx\n",
            (unsigned long long)g_kernel_pml4);

    /* Enable SMEP (bit 20) and SMAP (bit 21) in CR4 if the CPU supports them. */
    u32 eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(7), "c"(0));
    u64 cr4 = read_cr4();
    if (ebx & (1U << 7))  {
        cr4 |= (1ULL << 20);
        g_smep_enabled = 1;
        kprintf("[VMM] SMEP enabled\n");
    }
    if (ebx & (1U << 20)) {
        cr4 |= (1ULL << 21);
        g_smap_enabled = 1;
        kprintf("[VMM] SMAP enabled\n");
    }
    write_cr4(cr4);

    /* Enable NXE in EFER so the NX bit in PTEs is honoured. */
    u64 efer = rdmsr(MSR_EFER);
    efer |= EFER_NXE;
    wrmsr(MSR_EFER, efer);

    kprintf("[VMM] 4-level paging, HHDM=0x%016llx, NXE enabled\n",
            (unsigned long long)HHDM_BASE);

    struct limine_memmap_response *memmap = memmap_raw;
    if (memmap) {
        for (u64 i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap->entries[i];
            u64 start = ALIGN_DOWN(entry->base, 4096);
            u64 end = ALIGN_UP(entry->base + entry->length, 4096);
            for (u64 p = start; p < end; p += 4096) {
                virt_addr_t va = (virt_addr_t)PHYS_TO_VIRT(p);
                if (!vmm_translate(g_kernel_pml4, va)) {
                    vmm_map(g_kernel_pml4, va, p, VMM_KERNEL_RW);
                }
            }
        }
        kprintf("[VMM] Mapped remaining memmap regions into HHDM\n");
    }
}

vmm_space_t vmm_kernel_space(void) { return g_kernel_pml4; }

void *vmm_map_io(phys_addr_t phys, size_t size)
{
    if (size == 0) return NULL;
    phys_addr_t phys_aligned = ALIGN_DOWN(phys, 4096);
    size_t size_aligned = ALIGN_UP((phys - phys_aligned) + size, 4096);
    for (size_t offset = 0; offset < size_aligned; offset += 4096) {
        phys_addr_t p = phys_aligned + offset;
        virt_addr_t va = (virt_addr_t)PHYS_TO_VIRT(p);
        if (!vmm_translate(g_kernel_pml4, va)) {
            vmm_map(g_kernel_pml4, va, p, VMM_MMIO);
        }
    }
    return (void *)PHYS_TO_VIRT(phys);
}
