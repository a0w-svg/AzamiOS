#include "include/paging.h"
#include "include/pmm.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "../arch/include/spinlock.h"
#include "../arch/include/smp.h"
#include <stdbool.h>


extern void switch_page_dir(void *page);

extern bool g_is_uefi;

/* ISR-safe spinlock protecting all page table entries and directory manipulations */
static volatile int paging_lock = 0;

extern uint64_t pd0[];

int paging_map_page(uintptr_t phys_addr, uintptr_t virt_addr, uint8_t is_kernel, uint8_t is_writable) {
    unsigned long flags;
    spinlock_acquire_irqsave(&paging_lock, &flags);

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FFUL;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FFUL;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FFUL;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FFUL;

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t*)(cr3 & ~0xFFFUL);

    if (!(pml4[pml4_idx] & 0x01UL)) {
        uint64_t *new_pdpt = (uint64_t*)pmm_alloc_block();
        if (!new_pdpt) {
            spinlock_release_irqrestore(&paging_lock, flags);
            return -1;
        }
        memset(new_pdpt, 0, PAGE_SIZE);
        pml4[pml4_idx] = ((uintptr_t)new_pdpt) | 0x07UL;
    } else if (!is_kernel) {
        pml4[pml4_idx] |= 0x04UL;
    }

    uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & 0x01UL)) {
        uint64_t *new_pd = (uint64_t*)pmm_alloc_block();
        if (!new_pd) {
            spinlock_release_irqrestore(&paging_lock, flags);
            return -1;
        }
        memset(new_pd, 0, PAGE_SIZE);
        pdpt[pdpt_idx] = ((uintptr_t)new_pd) | 0x07UL;
    } else {
        if (pdpt[pdpt_idx] & 0x80UL) {
            if ((phys_addr & ~0xFFFUL) == (virt_addr & ~0xFFFUL)) {
                spinlock_release_irqrestore(&paging_lock, flags);
                return 0;
            }
        }
        if (!is_kernel) {
            pdpt[pdpt_idx] |= 0x04UL;
        }
    }

    uint64_t *pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFUL);
    if (pd[pd_idx] & 0x80UL) {
        if ((phys_addr & ~0xFFFUL) == (virt_addr & ~0xFFFUL)) {
            spinlock_release_irqrestore(&paging_lock, flags);
            return 0;
        }
        uint64_t *pt = (uint64_t*)pmm_alloc_block();
        if (!pt) {
            spinlock_release_irqrestore(&paging_lock, flags);
            return -1;
        }
        memset(pt, 0, PAGE_SIZE);
        uint64_t base_phys = pd[pd_idx] & ~0x1FFFFFUL;
        for (int i = 0; i < 512; i++) {
            pt[i] = (base_phys + (uint64_t)i * 4096UL) | (pd[pd_idx] & 0x7FUL);
        }
        pd[pd_idx] = ((uintptr_t)pt) | 0x07UL;
    } else if (!(pd[pd_idx] & 0x01UL)) {
        uint64_t *new_pt = (uint64_t*)pmm_alloc_block();
        if (!new_pt) {
            spinlock_release_irqrestore(&paging_lock, flags);
            return -1;
        }
        memset(new_pt, 0, PAGE_SIZE);
        pd[pd_idx] = ((uintptr_t)new_pt) | 0x07UL;
    }

    if (!is_kernel) {
        pd[pd_idx] |= 0x04UL;
    }

    uint64_t *pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ~0xFFFUL);
    pt[pt_idx] = (phys_addr & ~0xFFFUL)
        | (is_writable ? 0x02UL : 0x00UL)
        | 0x01UL
        | (is_kernel ? 0x00UL : 0x04UL);

    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    spinlock_release_irqrestore(&paging_lock, flags);
    /* Broadcast TLB invalidation to all other cores */
    if (smp_is_active()) smp_tlb_shootdown(virt_addr);
    return 0;

}

int paging_unmap_page(uintptr_t virt_addr) {
    unsigned long flags;
    spinlock_acquire_irqsave(&paging_lock, &flags);

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FFUL;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FFUL;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FFUL;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FFUL;

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t*)(cr3 & ~0xFFFUL);

    if (!(pml4[pml4_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return -1;
    }
    uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & 0x01UL) || (pdpt[pdpt_idx] & 0x80UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return -1;
    }
    uint64_t *pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & 0x01UL) || (pd[pd_idx] & 0x80UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return -1;
    }
    uint64_t *pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ~0xFFFUL);
    if (!(pt[pt_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return -1;
    }
    pt[pt_idx] = 0;
    asm volatile("invlpg (%0)" : : "r"(virt_addr) : "memory");
    spinlock_release_irqrestore(&paging_lock, flags);
    /* Broadcast TLB invalidation to all other cores */
    if (smp_is_active()) smp_tlb_shootdown(virt_addr);
    return 0;

}

uintptr_t paging_get_phys(uintptr_t virt_addr) {
    unsigned long flags;
    spinlock_acquire_irqsave(&paging_lock, &flags);

    uint64_t pml4_idx = (virt_addr >> 39) & 0x1FFUL;
    uint64_t pdpt_idx = (virt_addr >> 30) & 0x1FFUL;
    uint64_t pd_idx   = (virt_addr >> 21) & 0x1FFUL;
    uint64_t pt_idx   = (virt_addr >> 12) & 0x1FFUL;

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t*)(cr3 & ~0xFFFUL);

    if (!(pml4[pml4_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return 0;
    }
    uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4[pml4_idx] & ~0xFFFUL);
    if (!(pdpt[pdpt_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return 0;
    }
    if (pdpt[pdpt_idx] & 0x80UL) {
        uintptr_t base = (uintptr_t)(pdpt[pdpt_idx] & ~0x3FFFFFFFUL);
        spinlock_release_irqrestore(&paging_lock, flags);
        return base + (virt_addr & 0x3FFFFFFFUL);
    }
    uint64_t *pd = (uint64_t*)(uintptr_t)(pdpt[pdpt_idx] & ~0xFFFUL);
    if (!(pd[pd_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return 0;
    }
    if (pd[pd_idx] & 0x80UL) {
        uintptr_t base = (uintptr_t)(pd[pd_idx] & ~0x1FFFFFUL);
        spinlock_release_irqrestore(&paging_lock, flags);
        return base + (virt_addr & 0x1FFFFFUL);
    }
    uint64_t *pt = (uint64_t*)(uintptr_t)(pd[pd_idx] & ~0xFFFUL);
    if (!(pt[pt_idx] & 0x01UL)) {
        spinlock_release_irqrestore(&paging_lock, flags);
        return 0;
    }
    uintptr_t phys = (uintptr_t)(pt[pt_idx] & ~0xFFFUL) + (virt_addr & 0xFFFUL);
    spinlock_release_irqrestore(&paging_lock, flags);
    return phys;
}

uintptr_t paging_clone_directory(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&paging_lock, &flags);

    uint64_t *new_pml4 = (uint64_t*)pmm_alloc_block();
    uint64_t *new_pdpt = (uint64_t*)pmm_alloc_block();
    uint64_t *new_pd0 = (uint64_t*)pmm_alloc_block();
    uint64_t *new_pd1 = (uint64_t*)pmm_alloc_block();
    uint64_t *new_pd2 = (uint64_t*)pmm_alloc_block();
    uint64_t *new_pd3 = (uint64_t*)pmm_alloc_block();

    if (!new_pml4 || !new_pdpt || !new_pd0 || !new_pd1 || !new_pd2 || !new_pd3) {
        if (new_pml4) pmm_free_block(new_pml4);
        if (new_pdpt) pmm_free_block(new_pdpt);
        if (new_pd0) pmm_free_block(new_pd0);
        if (new_pd1) pmm_free_block(new_pd1);
        if (new_pd2) pmm_free_block(new_pd2);
        if (new_pd3) pmm_free_block(new_pd3);
        spinlock_release_irqrestore(&paging_lock, flags);
        return 0;
    }

    memset(new_pml4, 0, 4096);
    memset(new_pdpt, 0, 4096);

    new_pml4[0] = ((uintptr_t)new_pdpt) | 0x07UL;
    new_pdpt[0] = ((uintptr_t)new_pd0) | 0x07UL;
    new_pdpt[1] = ((uintptr_t)new_pd1) | 0x07UL;
    new_pdpt[2] = ((uintptr_t)new_pd2) | 0x07UL;
    new_pdpt[3] = ((uintptr_t)new_pd3) | 0x07UL;

    uintptr_t cur_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    uint64_t *cur_pml4 = (uint64_t*)cur_cr3;
    uint64_t *cur_pdpt = (uint64_t*)(uintptr_t)(cur_pml4[0] & ~0xFFFUL);
    uint64_t *cur_pd_pages[4] = {
        (uint64_t*)(uintptr_t)(cur_pdpt[0] & ~0xFFFUL),
        (uint64_t*)(uintptr_t)(cur_pdpt[1] & ~0xFFFUL),
        (uint64_t*)(uintptr_t)(cur_pdpt[2] & ~0xFFFUL),
        (uint64_t*)(uintptr_t)(cur_pdpt[3] & ~0xFFFUL)
    };

    uint64_t *pd_pages[4] = { new_pd0, new_pd1, new_pd2, new_pd3 };
    for (uint32_t pd_idx = 0; pd_idx < 2048; pd_idx++) {
        uint64_t *cur_pd_tab = cur_pd_pages[pd_idx >> 9];
        uint64_t *new_pd_tab = pd_pages[pd_idx >> 9];
        uint32_t entry_idx = pd_idx & 0x1FFu;
        uint64_t cur_entry = cur_pd_tab[entry_idx];

        if (pd_idx < 128 || pd_idx >= 1800) {
            /* Share kernel space mappings directly, clearing the USER flag for isolation */
            new_pd_tab[entry_idx] = cur_entry & ~0x04UL;
        } else if ((cur_entry & 1UL) != 0 && (cur_entry & 0x80UL) == 0) {
            /* 4KB page table in user space: allocate a new page table and copy PTEs */
            uint64_t *new_pt = (uint64_t*)pmm_alloc_block();
            if (new_pt) {
                uint64_t *old_pt = (uint64_t*)(uintptr_t)(cur_entry & ~0xFFFUL);
                memcpy(new_pt, old_pt, 4096);
                for (int i = 0; i < 512; i++) {
                    uint64_t pte = new_pt[i];
                    if ((pte & 1UL) != 0 && (pte & 4UL) != 0 && (pte & 2UL) != 0) {
                        void *new_phys = pmm_alloc_block();
                        if (new_phys) {
                            void *old_phys = (void*)(uintptr_t)(pte & ~0xFFFUL);
                            memcpy(new_phys, old_phys, 4096);
                            new_pt[i] = ((uintptr_t)new_phys) | (pte & 0xFFFUL);
                        }
                    }
                }
                new_pd_tab[entry_idx] = ((uintptr_t)new_pt) | (cur_entry & 0xFFFUL);
            } else {
                new_pd_tab[entry_idx] = cur_entry;
            }
        } else {
            new_pd_tab[entry_idx] = cur_entry;
        }
    }

    spinlock_release_irqrestore(&paging_lock, flags);
    return (uintptr_t)new_pml4;
}

void paging_map_framebuffer(uint32_t lfb_phys, uint32_t size_bytes) {
    (void)lfb_phys; (void)size_bytes;
    /* Framebuffer is below 4GB, already identity mapped by boot64.asm */
}

void paging_init(void) {
    /* In 64-bit mode, early paging is already initialized in boot.asm/UEFI */
    return;
}