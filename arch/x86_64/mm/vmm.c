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
#include "../cpu/cpu.h"
#include "../cpu/hwaccel.h"
#include "../cpu/msr.h"
#include "../cpu/spinlock.h"
#include "tlb.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"
#include "../boot/limine.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/perf/ktrace.h"

/* ── Kernel PML4 physical address ─────────────────────────────────────────── */
static vmm_space_t g_kernel_pml4 = 0;

/* ── Global VMM lock (protects kernel page table modifications) ─────────────── */
static spinlock_t g_vmm_lock = SPINLOCK_INIT;

/* ── Copy-on-Write page reference counters ────────────────────────────────── */
/*
 * One u16 per physical page-frame (PFN).  A frame with refcount 0 is not COW-
 * shared; it may be freed directly.  A frame with refcount ≥ 2 is shared and
 * must be copied on a write fault.  Refcount 1 means the last owner — the write
 * fault may promote in-place without copying.
 *
 * Sizing: the array covers every frame the buddy allocator can possibly hand
 * out.  pmm_get_total_pages() returns that count during vmm_init(), which runs
 * before any userspace process exists, so the static bound below is always safe.
 *
 * 32 GiB / 4 KiB = 8 388 608 frames × 2 bytes = 16 MiB.  That is acceptable
 * kernel BSS.  On machines with less RAM most of the array is unused but still
 * physically backed by the BSS.
 */
#define VMM_MAX_PHYS_PAGES  (32ULL * 1024 * 1024 * 1024 / 4096)
static uint16_t g_page_refcounts[VMM_MAX_PHYS_PAGES];
static spinlock_t g_cow_lock = SPINLOCK_INIT;

static inline size_t pfn(phys_addr_t p) { return (size_t)(p >> 12); }

void vmm_page_ref_inc(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return;
    irqflags_t fl = spinlock_lock_irqsave(&g_cow_lock);
    g_page_refcounts[f]++;
    spinlock_unlock_irqrestore(&g_cow_lock, fl);
}

uint16_t vmm_page_ref_dec(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return 0;
    irqflags_t fl = spinlock_lock_irqsave(&g_cow_lock);
    if (g_page_refcounts[f] > 0) g_page_refcounts[f]--;
    uint16_t v = g_page_refcounts[f];
    spinlock_unlock_irqrestore(&g_cow_lock, fl);
    return v;
}

static inline uint16_t vmm_page_refcount(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return 0;
    irqflags_t fl = spinlock_lock_irqsave(&g_cow_lock);
    uint16_t v = g_page_refcounts[f];
    spinlock_unlock_irqrestore(&g_cow_lock, fl);
    return v;
}

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
    /* A page table is written before it is ever read, so the non-temporal /
     * CLZERO path hw_clear_page() picks costs no read-for-ownership traffic
     * and leaves the caches holding the mappings we are walking. */
    hw_clear_page(phys_to_table(p));
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

    /* Security check: User-accessible pages cannot be placed in kernel space */
    if (virt >= 0x0000800000000000ULL && (flags & VMM_F_USER)) {
        return -1;
    }

    /* Intermediate tables need PRESENT+WRITE. If user space, add USER so ring 3 can traverse */
    u64 table_flags = VMM_F_PRESENT | VMM_F_WRITE;
    if (virt < 0x0000800000000000ULL) {
        table_flags |= VMM_F_USER;
    }

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    u64 *pml4 = phys_to_table(space);
    u64 *pdpt = get_or_create(&pml4[VMM_PML4_IDX(virt)], table_flags);
    if (!pdpt) { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    u64 *pd   = get_or_create(&pdpt[VMM_PDPT_IDX(virt)], table_flags);
    if (!pd)   { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    u64 *pt   = get_or_create(&pd[VMM_PD_IDX(virt)], table_flags);
    if (!pt)   { spinlock_unlock_irqrestore(&g_vmm_lock, irqf); return -1; }

    /* Install the leaf PTE. Replacing a *present* translation is the only case
     * that can leave another CPU holding a stale one — going from not-present
     * to present needs no shootdown, since the architecture never caches an
     * absent entry. Demand paging takes that path, which matters: it runs from
     * the #PF handler with interrupts disabled, where waiting on remote CPUs
     * would not be safe. */
    u64 old_pte = pt[VMM_PT_IDX(virt)];
    u64 new_pte = (phys & VMM_PHYS_MASK) | flags;
    bool replaced = (old_pte & VMM_F_PRESENT) && old_pte != new_pte;
    pt[VMM_PT_IDX(virt)] = new_pte;

    /* Invalidate the TLB entry for this VA on the current CPU. */
    invlpg(virt);

    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    if (replaced) tlb_shootdown_all();
    return 0;
}

int vmm_set_flags(vmm_space_t space, virt_addr_t virt, size_t count, u64 flags)
{
    if (!space) space = g_kernel_pml4;
    if (count == 0) return 0;

    bool changed = false;
    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    for (size_t i = 0; i < count; i++) {
        virt_addr_t va = virt + (i * PAGE_SIZE);
        u64 *pml4 = phys_to_table(space);
        if (!(pml4[VMM_PML4_IDX(va)] & VMM_F_PRESENT)) continue;

        u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(va)] & VMM_PHYS_MASK);
        u64 pdpte = pdpt[VMM_PDPT_IDX(va)];
        if (!(pdpte & VMM_F_PRESENT)) continue;
        if (pdpte & VMM_F_HUGE) {
            /* 1 GB huge mapping (e.g. Limine's HHDM). Rewrite the huge entry in
             * place rather than dereferencing its data as a page table. Whole-
             * page granularity; a sub-range request under-flushes, but no path
             * here creates 1 GB user mappings. */
            u64 keep = pdpte & (VMM_PHYS_MASK | VMM_F_HUGE | VMM_F_GLOBAL | VMM_F_SHARED);
            if ((keep | flags) != pdpte) changed = true;
            pdpt[VMM_PDPT_IDX(va)] = keep | flags;
            invlpg(va);
            continue;
        }

        u64 *pd = phys_to_table(pdpte & VMM_PHYS_MASK);
        u64 pde = pd[VMM_PD_IDX(va)];
        if (!(pde & VMM_F_PRESENT)) continue;
        if (pde & VMM_F_HUGE) {
            /* 2 MB huge mapping — same treatment as the 1 GB case above. */
            u64 keep = pde & (VMM_PHYS_MASK | VMM_F_HUGE | VMM_F_GLOBAL | VMM_F_SHARED);
            if ((keep | flags) != pde) changed = true;
            pd[VMM_PD_IDX(va)] = keep | flags;
            invlpg(va);
            continue;
        }

        u64 *pt = phys_to_table(pde & VMM_PHYS_MASK);
        u64 pte = pt[VMM_PT_IDX(va)];
        /* Skip only genuinely empty slots. A PROT_NONE page retains its phys
         * bits with PRESENT cleared — mprotect(PROT_READ) afterwards must be
         * able to bring it back, so fall through when phys != 0. */
        if (!(pte & VMM_F_PRESENT) && (pte & VMM_PHYS_MASK) == 0) continue;

        phys_addr_t phys = pte & VMM_PHYS_MASK;
        u64 preserved = pte & (VMM_F_GLOBAL | VMM_F_SHARED);
        /* mprotect(2) must not silently drop a page's protection key; only
         * pkey_mprotect(2) sets one, and it says so with VMM_F_PKEY_SET. */
        if (!(flags & VMM_F_PKEY_SET)) preserved |= pte & VMM_PKEY_MASK;
        u64 npte = (phys | flags | preserved) & ~VMM_F_PKEY_SET;
        if (npte != pte) changed = true;
        pt[VMM_PT_IDX(va)] = npte;
        invlpg(va);
    }

    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    /* mprotect(2) narrowing a range is only enforced once every CPU has
     * dropped the old, more permissive translation. */
    if (changed) tlb_shootdown_all();
    return 0;
}

/* Clear one leaf PTE and invalidate it locally. Caller holds g_vmm_lock and is
 * responsible for the cross-CPU shootdown; returning the old entry lets it
 * decide whether one is needed and whether a frame has to be released. */
static u64 pte_clear_locked(vmm_space_t space, virt_addr_t virt)
{
    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pd = phys_to_table(pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pt = phys_to_table(pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK);
    u64 old_pte = pt[VMM_PT_IDX(virt)];
    pt[VMM_PT_IDX(virt)] = 0;
    invlpg(virt);
    return old_pte;
}

u64 vmm_unmap_get(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);
    u64 old_pte = pte_clear_locked(space, virt);
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);

    /* Callers free the frame this returns, so no other CPU may still be able to
     * reach it through a cached translation once we are back. */
    if (old_pte & VMM_PHYS_MASK) tlb_shootdown_all();
    return old_pte;
}

size_t vmm_unmap_range(vmm_space_t space, virt_addr_t virt, size_t count, bool free_frames)
{
    if (!space) space = g_kernel_pml4;
    if (count == 0) return 0;

    /* One shootdown per page would make munmap() of a large region a storm of
     * IPI round-trips, so unmap in chunks and pay for one shootdown per chunk.
     * The frames are released only *after* that shootdown: handing a frame back
     * to the allocator while another CPU can still reach it through a stale
     * translation is exactly the bug the shootdown exists to prevent. The chunk
     * size is bounded by what the batch array may take from a 16 KB ring-0
     * stack. */
    #define UNMAP_CHUNK 128
    phys_addr_t batch[UNMAP_CHUNK];
    size_t freed = 0;

    for (size_t base = 0; base < count; base += UNMAP_CHUNK) {
        size_t n = count - base;
        if (n > UNMAP_CHUNK) n = UNMAP_CHUNK;

        size_t nbatch = 0;
        bool any_live = false;

        irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);
        for (size_t i = 0; i < n; i++) {
            u64 old = pte_clear_locked(space, virt + (base + i) * PAGE_SIZE);
            phys_addr_t phys = old & VMM_PHYS_MASK;
            if (!phys) continue;
            any_live = true;
            /* A SHARED frame belongs to a shmem object or a peer mapping, and a
             * kernel frame is not ours to reclaim here. */
            if (free_frames && (old & VMM_F_USER) && !(old & VMM_F_SHARED)) {
                /* COW-shared frame: only free when last reference drops. */
                if (old & VMM_F_COW) {
                    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                    uint16_t rc = vmm_page_ref_dec(phys);
                    irqf = spinlock_lock_irqsave(&g_vmm_lock);
                    if (rc == 0)
                        batch[nbatch++] = phys;
                } else {
                    batch[nbatch++] = phys;
                }
            }
        }
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);

        if (any_live) tlb_shootdown_all();
        for (size_t i = 0; i < nbatch; i++) {
            pmm_free_page(batch[i]);
            freed++;
        }
    }
    #undef UNMAP_CHUNK
    return freed;
}

void vmm_unmap(vmm_space_t space, virt_addr_t virt)
{
    vmm_unmap_get(space, virt);
}

phys_addr_t vmm_translate(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pdpt[VMM_PDPT_IDX(virt)] & VMM_F_HUGE) {
        /* 1 GB huge page.
         *
         * Masking off only the low 30 bits leaves every flag *above* the
         * address field in the result — NX at bit 63 above all, which Limine
         * sets on the HHDM. Callers that hand this value to hardware (every
         * virtio/DMA driver translates a kernel pointer through here) would
         * programme a descriptor with bit 63 set. Take the architectural
         * address field and nothing else. */
        return (pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK & ~0x3FFFFFFFULL) + (virt & 0x3FFFFFFFULL);
    }

    u64 *pd   = phys_to_table(pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pd[VMM_PD_IDX(virt)] & VMM_F_HUGE) {
        /* 2 MB huge page — same reasoning as the 1 GB case above. */
        return (pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK & ~0x1FFFFFULL) + (virt & 0x1FFFFFULL);
    }

    u64 *pt   = phys_to_table(pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pt[VMM_PT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    return (pt[VMM_PT_IDX(virt)] & VMM_PHYS_MASK) + (virt & 0xFFFUL);
}

u64 vmm_query_flags(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) return 0;

    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pdpt[VMM_PDPT_IDX(virt)] & VMM_F_HUGE) return pdpt[VMM_PDPT_IDX(virt)] & ~VMM_PHYS_MASK;

    u64 *pd = phys_to_table(pdpt[VMM_PDPT_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(virt)] & VMM_F_PRESENT)) return 0;
    if (pd[VMM_PD_IDX(virt)] & VMM_F_HUGE) return pd[VMM_PD_IDX(virt)] & ~VMM_PHYS_MASK;

    u64 *pt = phys_to_table(pd[VMM_PD_IDX(virt)] & VMM_PHYS_MASK);
    if (!(pt[VMM_PT_IDX(virt)] & VMM_F_PRESENT)) return 0;
    return pt[VMM_PT_IDX(virt)] & ~VMM_PHYS_MASK;
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
                /* 1 GB user huge page — deep copy if writable, share r/o pages.
                 * NOTE: the buddy allocator maxes out at PMM_MAX_ORDER (4 MB),
                 * so a private 1 GB copy cannot be satisfied and the fork fails
                 * cleanly (goto oom). Userspace 1 GB huge pages are not expected;
                 * if they become real, promote them to a PT walk here. */
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
                    /* Read-only or SHARED: share frame; mark VMM_F_SHARED on both so destroy doesn't free it.
                     * TODO(F16): VMM_F_SHARED is sticky and unrefcounted, so a
                     * formerly-private RO huge page shared here is never freed
                     * once both parent and child exit. Needs a PMM page
                     * refcount (deferred follow-up). Rare: no path maps user
                     * huge pages today. */
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
                    /* BUG-AE fix: PROT_NONE pages retain their physical frame and USER flag
                     * with PRESENT cleared. Don't skip them or the child loses PROT_NONE mappings. */
                    if (!(src_pt[pti] & VMM_F_PRESENT) && !(src_pt[pti] & VMM_PHYS_MASK)) {
                        dst_pt[pti] = 0;
                        continue;
                    }

                    /* COW fork: for private writable user pages, mark both
                     * parent and child read-only + VMM_F_COW and share the
                     * frame.  A write fault on either side will break COW by
                     * allocating a private copy.  Pages that are already
                     * VMM_F_SHARED (shmem/mmap SHARED) or VMM_F_COW (already
                     * COW from a previous fork) keep their existing bits.
                     * Read-only and kernel-mapped pages are shared unchanged. */
                    if ((src_pt[pti] & VMM_F_USER) && !(src_pt[pti] & VMM_F_SHARED)) {
                        phys_addr_t fp = src_pt[pti] & VMM_PHYS_MASK;
                        if (fp) {
                            if (src_pt[pti] & VMM_F_WRITE) {
                                /* Strip WRITE, set COW in both parent and child. */
                                u64 cow_pte = (src_pt[pti] & ~VMM_F_WRITE) | VMM_F_COW;
                                src_pt[pti] = cow_pte;
                                dst_pt[pti] = cow_pte;
                                /* Bump refcount to 2 (one existing owner + new child). */
                                spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                                vmm_page_ref_inc(fp);
                                vmm_page_ref_inc(fp);
                                irqf = spinlock_lock_irqsave(&g_vmm_lock);
                                /* Refresh all table pointers (HHDM-stable). */
                                src_pml4 = phys_to_table(src);
                                dst_pml4 = phys_to_table(dst_phys);
                                src_pdpt = phys_to_table(src_pml4[pml4i] & VMM_PHYS_MASK);
                                dst_pdpt = phys_to_table(dst_pml4[pml4i] & VMM_PHYS_MASK);
                                src_pd   = phys_to_table(src_pdpt[pdpti] & VMM_PHYS_MASK);
                                dst_pd   = phys_to_table(dst_pdpt[pdpti] & VMM_PHYS_MASK);
                                src_pt   = phys_to_table(src_pd[pdi] & VMM_PHYS_MASK);
                                dst_pt   = phys_to_table(dst_pt_phys);
                            } else if (src_pt[pti] & VMM_F_COW) {
                                /* Already COW (grandchild fork): share and bump refcount. */
                                dst_pt[pti] = src_pt[pti];
                                spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                                vmm_page_ref_inc(fp);
                                irqf = spinlock_lock_irqsave(&g_vmm_lock);
                                src_pml4 = phys_to_table(src);
                                dst_pml4 = phys_to_table(dst_phys);
                                src_pdpt = phys_to_table(src_pml4[pml4i] & VMM_PHYS_MASK);
                                dst_pdpt = phys_to_table(dst_pml4[pml4i] & VMM_PHYS_MASK);
                                src_pd   = phys_to_table(src_pdpt[pdpti] & VMM_PHYS_MASK);
                                dst_pd   = phys_to_table(dst_pdpt[pdpti] & VMM_PHYS_MASK);
                                src_pt   = phys_to_table(src_pd[pdi] & VMM_PHYS_MASK);
                                dst_pt   = phys_to_table(dst_pt_phys);
                            } else {
                                /* Read-only private page (e.g. code, rodata): allocate a private
                                 * frame and deep-copy it so child has its own copy. When either
                                 * process exits or execs, vmm_destroy_space() frees only its own
                                 * frames and never frees the other process's live code. */
                                spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                                phys_addr_t new_page = pmm_alloc_page();
                                if (!new_page) {
                                    irqf = spinlock_lock_irqsave(&g_vmm_lock);
                                    goto oom;
                                }
                                void *src_pg = (void *)PHYS_TO_VIRT(fp);
                                void *dst_pg = (void *)PHYS_TO_VIRT(new_page);
                                memcpy(dst_pg, src_pg, PAGE_SIZE);
                                irqf = spinlock_lock_irqsave(&g_vmm_lock);
                                src_pml4 = phys_to_table(src);
                                dst_pml4 = phys_to_table(dst_phys);
                                src_pdpt = phys_to_table(src_pml4[pml4i] & VMM_PHYS_MASK);
                                dst_pdpt = phys_to_table(dst_pml4[pml4i] & VMM_PHYS_MASK);
                                src_pd   = phys_to_table(src_pdpt[pdpti] & VMM_PHYS_MASK);
                                dst_pd   = phys_to_table(dst_pdpt[pdpti] & VMM_PHYS_MASK);
                                src_pt   = phys_to_table(src_pd[pdi] & VMM_PHYS_MASK);
                                dst_pt   = phys_to_table(dst_pt_phys);
                                dst_pt[pti] = new_page | (src_pt[pti] & ~VMM_PHYS_MASK);
                            }
                        } else {
                            dst_pt[pti] = src_pt[pti];
                        }
                    } else {
                        /* Kernel-mapped or explicitly SHARED (e.g. shmem): share physical frame. */
                        dst_pt[pti] = src_pt[pti];
                    }
                }
            }
        }
    }
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
    vmm_switch(read_cr3());
    tlb_shootdown_all();
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
                    /* BUG-AE fix: also free PROT_NONE pages where PRESENT is 0 but
                     * physical address and USER flag are present. */
                    if (((pt[pti] & VMM_F_PRESENT) || (pt[pti] & VMM_PHYS_MASK)) && (pt[pti] & VMM_F_USER)) {
                        if (!(pt[pti] & VMM_F_SHARED)) {
                            phys_addr_t fp = pt[pti] & VMM_PHYS_MASK;
                            if (fp) {
                                if (pt[pti] & VMM_F_COW) {
                                    /* COW-shared: release lock, decrement, reacquire */
                                    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
                                    uint16_t rc = vmm_page_ref_dec(fp);
                                    irqf = spinlock_lock_irqsave(&g_vmm_lock);
                                    if (rc == 0)
                                        pmm_free_page(fp);
                                    /* Refresh table pointers (HHDM stable) */
                                    pml4 = phys_to_table(space);
                                    pdpt = phys_to_table(pml4[pml4i] & VMM_PHYS_MASK);
                                    pd   = phys_to_table(pdpt[pdpti] & VMM_PHYS_MASK);
                                    pt   = phys_to_table(pd[pdi] & VMM_PHYS_MASK);
                                } else {
                                    pmm_free_page(fp);
                                }
                            }
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

    /* Detect and activate CPU extensions on BSP (PGE, UMIP, FSGSBASE, SMEP/SMAP, OSXSAVE, NXE) */
    cpu_detect_features();
    cpu_enable_features_bsp();

    /* Bind the kernel's accelerated primitives (CRC-32C, page clear/copy,
     * cache writeback, spin backoff) to whatever this CPU actually implements.
     * Must follow cpu_enable_features_bsp(), which is what decides whether the
     * ERMS path hwaccel_init() consults is live. */
    hwaccel_init();

    /* Configure Page Attribute Table (PAT MSR 0x277):
     * PA0: WB (0x06), PA1: WC (0x01), PA2: UC- (0x07), PA3: UC (0x00),
     * PA4: WB (0x06), PA5: WC (0x01), PA6: UC- (0x07), PA7: UC (0x00)
     * Value: 0x0007010600070106ULL */
    wrmsr(MSR_PAT, 0x0007010600070106ULL);

    kprintf("[VMM] 4-level paging, HHDM=0x%016llx, NXE & PAT (WC) enabled\n",
            (unsigned long long)HHDM_BASE);

    struct limine_memmap_response *memmap = memmap_raw;
    if (memmap) {
        /* Limine already maps every RAM region into the HHDM as write-back,
         * which is exactly the caching we want for those. Re-walking all of
         * physical RAM here just to (re)confirm that costs ~1 page-table walk
         * per 4 KiB — many seconds on a big guest. So we only touch the
         * regions whose caching actually has to change:
         *   FRAMEBUFFER            → write-combining
         *   RESERVED / NVS / holes → uncacheable + NX
         * RAM-type entries are left as the bootloader mapped them. */
        u64 fixed = 0;
        for (u64 i = 0; i < memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = memmap->entries[i];

            u64 start = ALIGN_DOWN(entry->base, 4096);
            u64 end   = ALIGN_UP(entry->base + entry->length, 4096);
            u64 pages = (end - start) >> 12;

            if (entry->type == 7) {                 /* FRAMEBUFFER → WC */
                vmm_set_flags(g_kernel_pml4, (virt_addr_t)PHYS_TO_VIRT(start),
                              pages, VMM_KERNEL_RW | VMM_F_WC);
                fixed += pages;
                continue;
            }
            /* ACPI_RECLAIMABLE holds the ACPI tables themselves (RSDP/XSDT/
             * FADT/DSDT...). Limine does NOT include this type in the HHDM —
             * verified against the live memmap, where the type-2 entry backing
             * the RSDP came back unmapped — so acpi_init()'s first dereference
             * of the RSDP took an unhandled kernel #PF and panicked the boot.
             * It is ordinary write-back RAM, so map the pages Limine left out
             * with normal cacheable kernel flags rather than the UC path below;
             * pages that are already mapped are left exactly as they are. */
            if (entry->type == 2) {
                u64 added = 0;
                for (u64 pa = start; pa < end; pa += 4096) {
                    virt_addr_t va = (virt_addr_t)PHYS_TO_VIRT(pa);
                    if (!vmm_translate(g_kernel_pml4, va)) {
                        vmm_map(g_kernel_pml4, va, pa, VMM_KERNEL_RW);
                        added++;
                    }
                }
                fixed += added;
                continue;
            }

            if (entry->type == 0 || entry->type == 5 || entry->type == 6)
                continue;                           /* RAM: already WB by Limine */

            /* RESERVED / ACPI NVS / bad RAM / device holes → UC + NX.
             * All real platform MMIO on q35 (LAPIC, IOAPIC, HPET, PCIe ECAM,
             * the 32-bit PCI hole) lives below 4 GiB; 64-bit BARs above that
             * are ioremap'd explicitly by their drivers. Reserved ranges the
             * firmware reports higher up are just address-space markers with
             * nothing behind them — skipping them avoids millions of pointless
             * page-table entries.
             * PLATFORM ASSUMPTION (q35): if a port to hardware with real MMIO
             * above 4 GiB in the HHDM window is ever done, this early-out must
             * become platform-aware or those ranges will be cached. */
            if (start >= 0x100000000ULL) continue;
            if (end > 0x100000000ULL) end = 0x100000000ULL;

            for (u64 p = start; p < end; p += 4096) {
                virt_addr_t va = (virt_addr_t)PHYS_TO_VIRT(p);
                if (!vmm_translate(g_kernel_pml4, va))
                    vmm_map(g_kernel_pml4, va, p, VMM_MMIO);
            }
            fixed += (end - start) >> 12;
        }
        kprintf("[VMM] HHDM caching fix-up: %llu pages (FB=WC, MMIO=UC); "
                "RAM left write-back as mapped by Limine\n",
                (unsigned long long)fixed);
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

/* ── Copy-on-Write fault resolution ──────────────────────────────────────── */

/*
 * vmm_cow_fault(space, fault_va) — break COW sharing for one 4 KiB page.
 *
 * Called from the #PF handler when:
 *   (a) error_code has the Write bit (bit 1) set,
 *   (b) vmm_query_flags() shows VMM_F_COW on the faulting PTE,
 *   (c) vma_probe() confirmed the VMA is PROT_WRITE.
 *
 * Two cases:
 *   refcount == 1  →  last owner; strip COW, add WRITE in-place (no copy).
 *   refcount >= 2  →  shared; allocate a private copy, decrement old refcount,
 *                     install fresh PTE with WRITE set and COW cleared.
 *
 * Returns 0 on success, -(ENOMEM) on allocation failure.
 * Must NOT be called with g_vmm_lock held (it acquires it internally).
 * Must NOT be called from kernel fault paths (only ring-3 write faults).
 */
int vmm_cow_fault(vmm_space_t space, virt_addr_t fault_va)
{
    KTRACE_CALL("vmm_cow_fault", fault_va);
    if (!space) space = g_kernel_pml4;
    virt_addr_t page_va = ALIGN_DOWN(fault_va, PAGE_SIZE);

    irqflags_t irqf = spinlock_lock_irqsave(&g_vmm_lock);

    /* Walk to the leaf PTE. */
    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(page_va)] & VMM_F_PRESENT)) {
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
        return -(int)EFAULT;
    }
    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(page_va)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(page_va)] & VMM_F_PRESENT) || (pdpt[VMM_PDPT_IDX(page_va)] & VMM_F_HUGE)) {
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
        return -(int)EFAULT;
    }
    u64 *pd = phys_to_table(pdpt[VMM_PDPT_IDX(page_va)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(page_va)] & VMM_F_PRESENT) || (pd[VMM_PD_IDX(page_va)] & VMM_F_HUGE)) {
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
        return -(int)EFAULT;
    }
    u64 *pt = phys_to_table(pd[VMM_PD_IDX(page_va)] & VMM_PHYS_MASK);
    u64 old_pte = pt[VMM_PT_IDX(page_va)];

    if (!(old_pte & VMM_F_COW)) {
        /* Not a COW page — either a genuine protection fault or already resolved
         * by another CPU; let the caller decide. */
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
        return -(int)EFAULT;
    }

    phys_addr_t old_phys = old_pte & VMM_PHYS_MASK;
    spinlock_unlock_irqrestore(&g_vmm_lock, irqf);

    /* Check refcount outside the vmm lock (cow_lock is separate). */
    uint16_t rc = vmm_page_refcount(old_phys);

    if (rc <= 1) {
        /* Last owner: promote in-place — add WRITE, clear COW. */
        irqf = spinlock_lock_irqsave(&g_vmm_lock);
        /* Re-read: another CPU might have already broken it. */
        u64 cur = pt[VMM_PT_IDX(page_va)];
        if (cur & VMM_F_COW) {
            pt[VMM_PT_IDX(page_va)] = (cur | VMM_F_WRITE) & ~VMM_F_COW;
            invlpg(page_va);
            /* Reset refcount to 0 now that this page is private and unshared. */
            vmm_page_ref_dec(old_phys);
        }
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf);
        tlb_shootdown_all();
        return 0;
    }

    /* Shared: allocate a private copy. */
    phys_addr_t new_phys = pmm_alloc_page();
    if (!new_phys) return -(int)ENOMEM;

    /* Copy the frame (done outside all locks). */
    void *src_kva = (void *)PHYS_TO_VIRT(old_phys);
    void *dst_kva = (void *)PHYS_TO_VIRT(new_phys);
    hw_copy_page(dst_kva, src_kva);

    /* Install the new private PTE. */
    irqflags_t irqf2 = spinlock_lock_irqsave(&g_vmm_lock);
    u64 cur = pt[VMM_PT_IDX(page_va)];
    if (cur & VMM_F_COW) {
        /* Still COW — install our copy. */
        pt[VMM_PT_IDX(page_va)] = (new_phys & VMM_PHYS_MASK)
                                  | (cur & ~VMM_PHYS_MASK & ~VMM_F_COW)
                                  | VMM_F_WRITE;
        invlpg(page_va);
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf2);
        tlb_shootdown_all();
        /* Decrement the old frame's refcount; free if it hits zero. */
        uint16_t remaining = vmm_page_ref_dec(old_phys);
        if (remaining == 0)
            pmm_free_page(old_phys);
    } else {
        /* Another CPU already broke COW while we were copying — discard ours. */
        spinlock_unlock_irqrestore(&g_vmm_lock, irqf2);
        pmm_free_page(new_phys);
    }
    return 0;
}

