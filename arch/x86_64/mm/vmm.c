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

/* One past the highest physical address the bootloader's memory map describes,
 * which is also the extent of the HHDM window: every entry in that map, RAM or
 * not, is aliased at PHYS_TO_VIRT(base). Recorded during vmm_init() because
 * the memmap response is not kept anywhere else, and kprotect_seal() needs to
 * know how much address space "the HHDM" actually means before it can blanket
 * it with NX. */
u64 g_hhdm_phys_top = 0;

/* ── Global VMM lock (protects kernel page table modifications) ─────────────── */
#define VMM_LOCK_STRIPES 64
static spinlock_t g_vmm_locks[VMM_LOCK_STRIPES];

static inline spinlock_t *vmm_get_lock(vmm_space_t space) {
    if (!space) space = g_kernel_pml4;
    return &g_vmm_locks[(space >> 12) % VMM_LOCK_STRIPES];
}

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

static inline size_t pfn(phys_addr_t p) { return (size_t)(p >> 12); }

void vmm_page_ref_inc(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return;
    __atomic_add_fetch(&g_page_refcounts[f], 1, __ATOMIC_RELAXED);
}

uint16_t vmm_page_ref_dec(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return 0;
    return __atomic_sub_fetch(&g_page_refcounts[f], 1, __ATOMIC_ACQ_REL);
}

/* Pull @p's refcount line toward this core ahead of an increment that will
 * need it. A no-op for a frame outside the tracked range, and harmless for one
 * that turns out not to need the increment after all — a prefetch has no
 * architectural effect. */
static inline void vmm_page_ref_prefetch(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f < VMM_MAX_PHYS_PAGES) hw_prefetch_write(&g_page_refcounts[f]);
}

static inline uint16_t vmm_page_refcount(phys_addr_t p)
{
    size_t f = pfn(p);
    if (f >= VMM_MAX_PHYS_PAGES) return 0;
    return __atomic_load_n(&g_page_refcounts[f], __ATOMIC_ACQUIRE);
}

/* ── Internal helpers ─────────────────────────────────────────────────────── */


/* Return a pointer to a PTE table given its physical address via HHDM. */
static inline u64 *phys_to_table(phys_addr_t p) {
    if (p >= 0xffff800000000000ULL) p = VIRT_TO_PHYS(p);
    return (u64 *)PHYS_TO_VIRT(p & VMM_PHYS_MASK);
}

/*
 * Allocate a 4 KB page table level without clearing it (returns physical
 * address).
 *
 * Only for a caller that writes all 512 entries before anything can fail. That
 * is a hard requirement, not a preference: the OOM path of the one caller that
 * uses this tears the half-built address space down with vmm_destroy_space(),
 * which walks every entry and frees the frames it finds — so a single entry
 * left holding whatever the page last contained would hand a live frame back
 * to the allocator.
 */
static phys_addr_t alloc_table_raw(void)
{
    return pmm_alloc_page();
}

/* Allocate a zeroed 4 KB page table level (returns physical address). */
static phys_addr_t alloc_table(void)
{
    phys_addr_t p = pmm_alloc_page();
    if (!p) return 0;
    /* hw_clear_page_hot(), not hw_clear_page(): a page table is cleared only
     * so it can be filled in, and the caller's very next act is to write
     * entries into it. Clearing it non-temporally would write around the cache
     * and leave the fill to pull every line back with a read-for-ownership. */
    hw_clear_page_hot(phys_to_table(p));
    return p;
}

/*
 * The leaf page table covering one 2 MB region, remembered across the pages of
 * a loop.
 *
 * vmm_unmap_range() and vmm_set_flags() both resolve PML4 -> PDPT -> PD -> PT
 * for every single page they touch. Those are four dependent loads — each
 * one's address comes out of the previous one's result, so nothing overlaps
 * and the hardware prefetcher cannot predict them — and for any range inside
 * one 2 MB region the first three produce the same answer every time. A
 * 128-page munmap does 512 of them where 131 would do, and tearing down an
 * address space at exit does it for every page the process ever mapped.
 *
 * Two things make this safe. Both loops hold g_vmm_lock for their whole
 * duration, so no other CPU can install or remove a level underneath them; and
 * the only write either loop makes is to a leaf entry, never to a level the
 * cache is keyed on. The key is the 2 MB region the leaf table covers, so a
 * stride that leaves the region simply misses and re-walks, and a huge mapping
 * never populates the cache at all because it has no leaf table.
 */
typedef struct {
    virt_addr_t region;   /* base of the 2 MB region `pt` covers */
    u64        *pt;       /* NULL when nothing is cached */
} pt_cache_t;

#define PT_REGION_MASK  (~(virt_addr_t)((1ULL << 21) - 1))

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

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));

    u64 *pml4 = phys_to_table(space);
    u64 *pdpt = get_or_create(&pml4[VMM_PML4_IDX(virt)], table_flags);
    if (!pdpt) { spinlock_unlock_irqrestore(vmm_get_lock(space), irqf); return -1; }

    u64 *pd   = get_or_create(&pdpt[VMM_PDPT_IDX(virt)], table_flags);
    if (!pd)   { spinlock_unlock_irqrestore(vmm_get_lock(space), irqf); return -1; }

    u64 *pt   = get_or_create(&pd[VMM_PD_IDX(virt)], table_flags);
    if (!pt)   { spinlock_unlock_irqrestore(vmm_get_lock(space), irqf); return -1; }

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

    spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
    /* One user page replaced: the other cores need only drop this address
     * space, not everything they have cached. */
    if (replaced) tlb_shootdown_user(space, virt, 1);
    return 0;
}

int vmm_set_flags(vmm_space_t space, virt_addr_t virt, size_t count, u64 flags)
{
    if (!space) space = g_kernel_pml4;
    if (count == 0) return 0;

    bool changed = false;

    /*
     * Chunked, like vmm_unmap_range() above and for the same reason.
     *
     * This used to hold g_vmm_lock — with interrupts disabled — across the
     * entire range in one go, and @count is whatever userspace asked
     * mprotect(2) for: a process can hand it a multi-gigabyte mapping and pin
     * this core with interrupts off for the whole walk. A core in that state
     * cannot service a TLB shootdown IPI, so every other core doing a
     * shootdown blocks behind it until it finishes — which is precisely the
     * starvation the "[TLB] shootdown to CPUn stuck" warning reports.
     *
     * Releasing between chunks bounds that window to a fixed amount of work
     * regardless of the request size. It widens nothing: the lock was already
     * dropped and retaken between chunks by the unmap path next door, the
     * range is serialised by the caller's own mmap lock a layer up, and the
     * permission change was never atomic across the range anyway — remote
     * cores only see it after the single shootdown at the end.
     */
    #define SETFLAGS_CHUNK 128

    for (size_t base = 0; base < count; base += SETFLAGS_CHUNK) {
        size_t chunk = count - base;
        if (chunk > SETFLAGS_CHUNK) chunk = SETFLAGS_CHUNK;

        irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));

        /* Scoped to this chunk's critical section: the cache is only valid while
         * g_vmm_lock is held. */
        pt_cache_t ptc = { 0, NULL };

        for (size_t i = 0; i < chunk; i++) {
            virt_addr_t va = virt + ((base + i) * PAGE_SIZE);
            u64 *pt;

            if (ptc.pt && ptc.region == (va & PT_REGION_MASK)) {
                /* Same 2 MB region as the previous page: the first three lookups
                 * would only repeat themselves. */
                pt = ptc.pt;
            } else {
                u64 *pml4 = phys_to_table(space);
                if (!(pml4[VMM_PML4_IDX(va)] & VMM_F_PRESENT)) continue;

                u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(va)] & VMM_PHYS_MASK);
                u64 pdpte = pdpt[VMM_PDPT_IDX(va)];
                if (!(pdpte & VMM_F_PRESENT)) continue;
                if (pdpte & VMM_F_HUGE) {
                    /* 1 GB huge mapping (e.g. Limine's HHDM). Rewrite the huge
                     * entry in place rather than dereferencing its data as a page
                     * table. Whole-page granularity; a sub-range request
                     * under-flushes, but no path here creates 1 GB user mappings. */
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

                pt = phys_to_table(pde & VMM_PHYS_MASK);
                ptc.region = va & PT_REGION_MASK;
                ptc.pt     = pt;
            }

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

        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
    }
    #undef SETFLAGS_CHUNK

    /* mprotect(2) narrowing a range is only enforced once every CPU has
     * dropped the old, more permissive translation — but only of this address
     * space, which is all mprotect(2) can have touched. */
    if (changed) tlb_shootdown_user(space, virt, count);
    return 0;
}

/* ── Attribute rewriting over an arbitrary kernel range ───────────────────── *
 *
 * vmm_set_flags() above is mprotect(2)'s primitive: it *replaces* a leaf's
 * attributes wholesale, one 4 KiB page at a time, and treats a huge mapping as
 * an all-or-nothing special case. Neither property suits kernel self-
 * protection, which has to say "add NX to everything in the HHDM" over a range
 * measured in gigabytes, without disturbing the caching bits the HHDM fix-up
 * already set and without shattering the 1 GiB and 2 MiB pages the bootloader
 * used to map it (the whole point of which is TLB reach — splitting them would
 * trade a security win for a measurable slowdown on every kernel memory
 * access).
 *
 * So this is the complementary operation: a read-modify-write of selected
 * attribute bits, applied at whatever granularity each mapping already uses.
 * A huge entry the range covers completely is rewritten in place and skipped
 * over in one step. A huge entry the range only clips is split down to the
 * next level first when @allow_split says the caller needs exactness — which
 * the kernel-image passes do, since .text and .rodata are neighbours inside
 * one 2 MiB region — and left alone otherwise.
 */

/* Bits that are attributes rather than addresses, at any level. Bit 12 is the
 * PAT bit of a huge entry (it is bit 7 on a 4 KiB PTE, where bit 12 is part of
 * the frame number), so it only travels with the huge cases below. */
#define KPROT_ATTR_COMMON  (VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | \
                            VMM_F_PWT | VMM_F_PCD | VMM_F_ACCESSED |  \
                            VMM_F_DIRTY | VMM_F_GLOBAL | VMM_F_SHARED | \
                            VMM_F_COW | VMM_F_NX | VMM_PKEY_MASK)
#define KPROT_HUGE_PAT     (1ULL << 12)

#define SZ_1G  (1ULL << 30)
#define SZ_2M  (1ULL << 21)

/* Split one 1 GiB entry into a page directory of 512 2 MiB entries, or one
 * 2 MiB entry into a page table of 512 4 KiB entries. @entry is the slot to
 * replace; it must currently hold a present huge mapping. Returns the child
 * table, or NULL if no page was available (in which case @entry is untouched
 * and the caller must leave the mapping as it found it). */
static u64 *split_huge_entry(u64 *entry, bool one_gig)
{
    u64 orig = *entry;
    phys_addr_t child_phys = pmm_alloc_page();
    if (!child_phys) return NULL;

    u64 *child = phys_to_table(child_phys);
    u64  base  = orig & VMM_PHYS_MASK & ~((one_gig ? SZ_1G : SZ_2M) - 1);

    if (one_gig) {
        /* 1 GiB → 2 MiB: both are huge entries, so every attribute including
         * the PAT bit sits in the same place and carries over unchanged. */
        u64 attrs = (orig & (KPROT_ATTR_COMMON | KPROT_HUGE_PAT)) | VMM_F_HUGE;
        for (u32 i = 0; i < 512; i++)
            child[i] = (base + (u64)i * SZ_2M) | attrs;
    } else {
        /* 2 MiB → 4 KiB: the PAT bit moves from bit 12 to bit 7, because on a
         * leaf PTE bit 12 is the bottom of the frame number. Copying the raw
         * entry here would silently reassign the page's memory type. */
        u64 attrs = orig & KPROT_ATTR_COMMON;
        if (orig & KPROT_HUGE_PAT) attrs |= VMM_F_HUGE;   /* = PAT at this level */
        for (u32 i = 0; i < 512; i++)
            child[i] = (base + (u64)i * PAGE_SIZE) | attrs;
    }

    /* The parent must stay traversable by everyone the children are visible
     * to: USER on the parent is a permission ceiling, not a grant. */
    *entry = child_phys | VMM_F_PRESENT | VMM_F_WRITE | (orig & VMM_F_USER);
    return child;
}

s64 vmm_protect_range(vmm_space_t space, virt_addr_t start, virt_addr_t end,
                      u64 set, u64 clear, u32 prot_flags)
{
    const bool allow_split = (prot_flags & VMM_PROT_SPLIT) != 0;

    if (!space) space = g_kernel_pml4;
    start = ALIGN_DOWN(start, PAGE_SIZE);
    end   = ALIGN_UP(end, PAGE_SIZE);
    if (end <= start) return 0;

    /* Clearing an address bit would relocate the mapping, and clearing HUGE
     * would reinterpret a 2 MiB data page as a page table. Neither is ever
     * what a protection change means. */
    clear &= ~(VMM_PHYS_MASK | VMM_F_HUGE);

    s64  changed = 0;
    bool oom     = false;

    /* Chunked for the same reason vmm_set_flags() is: a core holding the
     * address space's lock with interrupts off cannot answer anyone else's
     * shootdown IPI, and a range this walks can be gigabytes wide.
     *
     * The budget counts *entries touched*, not address space covered, and
     * that distinction is the whole point. Bounding by VA would make the hold
     * time depend on how the range happens to be mapped: the same 64 MiB
     * window is 32 entries where the HHDM uses 2 MiB pages and 16384 entries
     * where the kernel image uses 4 KiB ones — a 512x spread in how long
     * interrupts stay off, and the wide end of it is long enough to make
     * another core's shootdown wait time out and start resending. Counting
     * entries makes every chunk the same amount of work, in the same order as
     * the 128-page chunk vmm_set_flags() settled on for the same reason. */
    #define PROTECT_CHUNK_ENTRIES  256

    virt_addr_t va = start;
    while (va < end && !oom) {
        u32 budget = PROTECT_CHUNK_ENTRIES;

        irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));
        u64 *pml4 = phys_to_table(space);

        while (va < end && budget) {
            u64 *pml4e = &pml4[VMM_PML4_IDX(va)];
            if (!(*pml4e & VMM_F_PRESENT)) {
                /* Skip the whole 512 GiB this PML4 slot covers. The wrap
                 * check matters: the kernel image lives in the topmost slot,
                 * so "one past the end" of it is 0, and without this the
                 * outer loop would restart at the bottom of the address
                 * space and never terminate. */
                virt_addr_t nxt = ALIGN_DOWN(va, 512ULL * SZ_1G) + 512ULL * SZ_1G;
                if (nxt <= va) { va = end; break; }
                va = nxt;
                continue;
            }

            u64 *pdpt  = phys_to_table(*pml4e & VMM_PHYS_MASK);
            u64 *pdpte = &pdpt[VMM_PDPT_IDX(va)];
            if (!(*pdpte & VMM_F_PRESENT)) {
                va = ALIGN_DOWN(va, SZ_1G) + SZ_1G;
                continue;
            }
            if (*pdpte & VMM_F_HUGE) {
                if ((va & (SZ_1G - 1)) == 0 && va + SZ_1G <= end) {
                    u64 nv = (*pdpte | set) & ~clear;
                    if (nv != *pdpte) { *pdpte = nv; changed++; }
                    va += SZ_1G;
                    budget--;
                    continue;
                }
                if (!allow_split) { va = ALIGN_DOWN(va, SZ_1G) + SZ_1G; continue; }
                if (!split_huge_entry(pdpte, true)) { oom = true; break; }
            }

            u64 *pd  = phys_to_table(*pdpte & VMM_PHYS_MASK);
            u64 *pde = &pd[VMM_PD_IDX(va)];
            if (!(*pde & VMM_F_PRESENT)) {
                va = ALIGN_DOWN(va, SZ_2M) + SZ_2M;
                continue;
            }
            if (*pde & VMM_F_HUGE) {
                if ((va & (SZ_2M - 1)) == 0 && va + SZ_2M <= end) {
                    u64 nv = (*pde | set) & ~clear;
                    if (nv != *pde) { *pde = nv; changed++; }
                    va += SZ_2M;
                    budget--;
                    continue;
                }
                if (!allow_split) { va = ALIGN_DOWN(va, SZ_2M) + SZ_2M; continue; }
                if (!split_huge_entry(pde, false)) { oom = true; break; }
            }

            /* 4 KiB leaves: walk the rest of this page table without redoing
             * the three upper levels, which all resolve to the same entries. */
            u64 *pt = phys_to_table(*pde & VMM_PHYS_MASK);
            do {
                u64 *pte = &pt[VMM_PT_IDX(va)];
                if (*pte & (VMM_F_PRESENT | VMM_PHYS_MASK)) {
                    u64 nv = (*pte | set) & ~clear;
                    if (nv != *pte) { *pte = nv; changed++; }
                }
                va += PAGE_SIZE;
                budget--;
            } while (va < end && budget && (va & (SZ_2M - 1)) != 0);
        }

        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
    }
    #undef PROTECT_CHUNK_ENTRIES

    /* Kernel-half mappings are global and shared by every address space, so
     * nothing narrower than "drop everything, globals included" is correct
     * here. A caller making a series of related edits passes VMM_PROT_NOFLUSH
     * on all of them and calls vmm_protect_flush() once at the end: each
     * broadcast makes every other core stop and acknowledge, and a core that
     * is spinning for g_vmm_lock at the time cannot answer until it gets the
     * lock, which is how a run of back-to-back shootdowns turns into the
     * "[TLB] shootdown to CPUn stuck" warning next door. */
    if (changed && !(prot_flags & VMM_PROT_NOFLUSH)) vmm_protect_flush();
    return oom ? -1 : changed;
}

void vmm_protect_flush(void)
{
    tlb_flush_local_global();
    tlb_shootdown_all();
}

#undef SZ_1G
#undef SZ_2M

/* Clear one leaf PTE and invalidate it locally. Caller holds g_vmm_lock and is
 * responsible for the cross-CPU shootdown; returning the old entry lets it
 * decide whether one is needed and whether a frame has to be released. */
static u64 pte_clear_locked(vmm_space_t space, virt_addr_t virt, pt_cache_t *c)
{
    u64 *pt;

    if (c && c->pt && c->region == (virt & PT_REGION_MASK)) {
        pt = c->pt;
    } else {
        u64 *pml4 = phys_to_table(space);
        if (!(pml4[VMM_PML4_IDX(virt)] & VMM_F_PRESENT)) return 0;

        u64 pdpte = phys_to_table(pml4[VMM_PML4_IDX(virt)] & VMM_PHYS_MASK)[VMM_PDPT_IDX(virt)];
        if (!(pdpte & VMM_F_PRESENT)) return 0;

        u64 pde = phys_to_table(pdpte & VMM_PHYS_MASK)[VMM_PD_IDX(virt)];
        if (!(pde & VMM_F_PRESENT)) return 0;

        pt = phys_to_table(pde & VMM_PHYS_MASK);

        /* Only a walk that really went through page tables is cacheable: every
         * caller of this today maps its range with 4 KB entries (user address
         * spaces, and the kernel-stack window, which sits under a PDPT slot
         * vmm_map() built itself), but a walk that descended through a huge
         * entry is reading mapped data as a table and nothing about it is
         * reproducible from a cached pointer. */
        if (c && !(pdpte & VMM_F_HUGE) && !(pde & VMM_F_HUGE)) {
            c->region = virt & PT_REGION_MASK;
            c->pt     = pt;
        }
    }

    u64 old_pte = pt[VMM_PT_IDX(virt)];
    pt[VMM_PT_IDX(virt)] = 0;
    invlpg(virt);
    return old_pte;
}

u64 vmm_unmap_get(vmm_space_t space, virt_addr_t virt)
{
    if (!space) space = g_kernel_pml4;

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));
    u64 old_pte = pte_clear_locked(space, virt, NULL);
    spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);

    /* Callers free the frame this returns, so no other CPU may still be able to
     * reach it through a cached translation once we are back. */
    if (old_pte & VMM_PHYS_MASK) tlb_shootdown_user(space, virt, 1);
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

        irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));
        /* Scoped to this chunk's critical section: the cache is only valid
         * while g_vmm_lock is held, and the lock is dropped between chunks. */
        pt_cache_t ptc = { 0, NULL };
        for (size_t i = 0; i < n; i++) {
            u64 old = pte_clear_locked(space, virt + (base + i) * PAGE_SIZE, &ptc);
            phys_addr_t phys = old & VMM_PHYS_MASK;
            if (!phys) continue;
            any_live = true;
            /* A SHARED frame belongs to a shmem object or a peer mapping, and a
             * kernel frame is not ours to reclaim here. */
            if (free_frames && (old & VMM_F_USER) && !(old & VMM_F_SHARED)) {
                /* COW-shared frame: only free when last reference drops. */
                if (old & VMM_F_COW) {
                    uint16_t rc = vmm_page_ref_dec(phys);
                    if (rc == 0)
                        batch[nbatch++] = phys;
                } else {
                    batch[nbatch++] = phys;
                }
            }
        }
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);

        if (any_live) tlb_shootdown_user(space, virt + base * PAGE_SIZE, n);
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

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(g_kernel_pml4));
    u64 *new_pml4 = phys_to_table(new_pml4_phys);
    u64 *krn_pml4 = phys_to_table(g_kernel_pml4);

    /* Share the kernel half: PML4 entries 256–511 point to the same PDPT
     * tables as the kernel's PML4. User entries (0–255) start as zero.    */
    for (int i = 0; i < 256; i++)  new_pml4[i] = 0;
    for (int i = 256; i < 512; i++) new_pml4[i] = krn_pml4[i];
    spinlock_unlock_irqrestore(vmm_get_lock(g_kernel_pml4), irqf);

    return new_pml4_phys;
}

vmm_space_t vmm_clone_space(vmm_space_t src)
{
    if (!src) src = g_kernel_pml4;
    if (src >= 0xffff800000000000ULL) src = VIRT_TO_PHYS(src);

    phys_addr_t dst_phys = alloc_table();
    if (!dst_phys) return 0;

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(src));

    u64 *src_pml4 = phys_to_table(src);
    u64 *dst_pml4 = phys_to_table(dst_phys);

    /* Share kernel half. One 2 KB move rather than 256 separate stores — this
     * lowers to the same ERMS `rep movsb` the rest of the kernel's bulk copies
     * use, and it runs on every fork. */
    u64 *krn_pml4 = phys_to_table(g_kernel_pml4);
    __builtin_memcpy(&dst_pml4[256], &krn_pml4[256], 256 * sizeof(u64));

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
                 * PMM_MAX_ORDER is 18 (1 GB), so pmm_alloc_pages(512 * 512)
                 * really can return a naturally aligned 1 GB frame — which is
                 * exactly what a PDPT huge entry needs — and this path now
                 * completes instead of always failing into `goto oom`. It still
                 * fails cleanly when no 1 GB block is free, which on a
                 * fragmented system is the common outcome. */
                if ((src_pdpt[pdpti] & VMM_F_WRITE) && (src_pdpt[pdpti] & VMM_F_USER) && !(src_pdpt[pdpti] & VMM_F_SHARED)) {
                    phys_addr_t new_phys = pmm_alloc_pages(512 * 512); /* 1 GB = 2^18 pages */
                    if (!new_phys) goto oom;
                    /* BUG-09: release lock before 1 GB memcpy; dst pages are private */
                    spinlock_unlock_irqrestore(vmm_get_lock(src), irqf);
                    u8 *src_pg = (u8 *)PHYS_TO_VIRT(src_pdpt[pdpti] & VMM_PHYS_MASK & ~0x3FFFFFFFUL);
                    u8 *dst_pg = (u8 *)PHYS_TO_VIRT(new_phys);
                    /* Not memcpy(): a gigabyte pulled through the cache
                     * hierarchy evicts everything the resumed thread was about
                     * to touch, and on a CPU without ERMS it does so one
                     * read-for-ownership at a time. hw_copy_pages() issues the
                     * whole run once and fences once. */
                    hw_copy_pages(dst_pg, src_pg, 512 * 512);
                    irqf = spinlock_lock_irqsave(vmm_get_lock(src));
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
                        spinlock_unlock_irqrestore(vmm_get_lock(src), irqf);
                        u8 *src_pg = (u8 *)PHYS_TO_VIRT(src_pd[pdi] & VMM_PHYS_MASK & ~0x1FFFFFUL);
                        u8 *dst_pg = (u8 *)PHYS_TO_VIRT(new_phys);
                        hw_copy_pages(dst_pg, src_pg, 512);
                        irqf = spinlock_lock_irqsave(vmm_get_lock(src));
                        dst_pd[pdi] = new_phys | (src_pd[pdi] & ~VMM_PHYS_MASK & ~VMM_F_HUGE);
                        dst_pd[pdi] |= VMM_F_HUGE;
                    } else {
                        src_pd[pdi] |= VMM_F_SHARED;
                        dst_pd[pdi] = src_pd[pdi];
                    }
                    continue;
                }

                /* Clone PT.
                 *
                 * alloc_table_raw(): the loop below writes every one of the
                 * 512 entries — including an explicit 0 for an empty slot —
                 * and nothing in it can fail or jump to oom, so zeroing the
                 * page first would only be overwritten immediately. Page
                 * tables are also the level there are most of (one per 2 MB of
                 * address space, against one PD per gigabyte), so this is
                 * nearly all of the zeroing fork was doing, and it was doing
                 * it under g_vmm_lock with interrupts off. */
                phys_addr_t dst_pt_phys = alloc_table_raw();
                if (!dst_pt_phys) goto oom;
                dst_pd[pdi] = dst_pt_phys | (src_pd[pdi] & ~VMM_PHYS_MASK);

                u64 *src_pt = phys_to_table(src_pd[pdi] & VMM_PHYS_MASK);
                u64 *dst_pt = phys_to_table(dst_pt_phys);

                for (int pti = 0; pti < 512; pti++) {
                    /* Start pulling the next entry's refcount line in now.
                     * g_page_refcounts is indexed by physical frame number, so
                     * a page table's worth of COW increments walks it in
                     * whatever order those frames happen to sit in physical
                     * memory — scattered, and invisible to the hardware
                     * prefetcher, which sees only the linear scan of the page
                     * table itself. One entry of lookahead overlaps that miss
                     * with the work this iteration is about to do. */
                    if (pti + 1 < 512) {
                        u64 nxt = src_pt[pti + 1];
                        if ((nxt & VMM_F_USER) && !(nxt & VMM_F_SHARED))
                            vmm_page_ref_prefetch(nxt & VMM_PHYS_MASK);
                    }

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
                            if (src_pt[pti] & VMM_F_COW) {
                                /* Already COW (grandchild fork): share and bump refcount. */
                                dst_pt[pti] = src_pt[pti];
                                vmm_page_ref_inc(fp);
                            } else {
                                /* First fork for this page (whether writable or read-only):
                                 * Strip WRITE if present, mark both parent and child COW,
                                 * and initialize refcount to 2 (parent + child).
                                 * A write fault will verify VMA permissions before breaking COW. */
                                u64 cow_pte = (src_pt[pti] & ~VMM_F_WRITE) | VMM_F_COW;
                                src_pt[pti] = cow_pte;
                                dst_pt[pti] = cow_pte;
                                vmm_page_ref_inc(fp);
                                vmm_page_ref_inc(fp);
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
    spinlock_unlock_irqrestore(vmm_get_lock(src), irqf);
    /* Reload CR3 verbatim (PCID bits and all) to drop this core's non-global
     * entries the deep-copy above may have created through scratch mappings;
     * the shootdown covers the others. Targeted at `src`, the space actually
     * mutated (its writable pages just got marked COW) — dst_phys is the
     * fresh child copy, which by definition has never run anywhere yet and
     * needs no shootdown of its own. */
    write_cr3(read_cr3());
    tlb_shootdown_space(src);
    return dst_phys;

oom:
    spinlock_unlock_irqrestore(vmm_get_lock(src), irqf);
    vmm_destroy_space(dst_phys);  /* clean up partial allocation */
    return 0;
}

void vmm_destroy_space(vmm_space_t space)
{
    if (!space || space == g_kernel_pml4) return;
    if (space >= 0xffff800000000000ULL) space = VIRT_TO_PHYS(space);

    /* CORRECTNESS FIX: this function used to free every frame in the space
     * (below) with no shootdown at all — a stale translation on some other
     * core that had run this address space earlier (PCID mode does not
     * flush those just because a core later switched away; see
     * vmm_switch_proc()'s doc comment in vmm.h) could keep pointing at a
     * frame straight through whatever the allocator hands it out to next.
     *
     * Unlike every other shootdown in this file, this one does not need to
     * happen *after* the edit: a shootdown for a whole PCID/address space
     * invalidates by identity, not by walking current PTE content, so it is
     * just as effective run first as last. Doing it first — before a single
     * frame is freed — gets the property every other call site has to work
     * for (no frame reused before the flush that must precede it) for free,
     * with no need to restructure this whole walk into gather-then-free like
     * vmm_unmap_range does. By the time the loop below runs, no other core
     * can still be holding a translation into this space at all. */
    tlb_shootdown_space(space);

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));
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
                                    uint16_t rc = vmm_page_ref_dec(fp);
                                    if (rc == 0)
                                        pmm_free_page(fp);
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
    spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
}

/* ── Initialisation ───────────────────────────────────────────────────────── */

void vmm_init(u64 hhdm_base, u64 phys_base, u64 virt_base, void *memmap_raw)
{
    for (int i = 0; i < VMM_LOCK_STRIPES; i++) {
        g_vmm_locks[i] = (spinlock_t)SPINLOCK_INIT;
    }

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

            if (end > g_hhdm_phys_top) g_hhdm_phys_top = end;

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

    irqflags_t irqf = spinlock_lock_irqsave(vmm_get_lock(space));

    /* Walk to the leaf PTE. */
    u64 *pml4 = phys_to_table(space);
    if (!(pml4[VMM_PML4_IDX(page_va)] & VMM_F_PRESENT)) {
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
        return -(int)EFAULT;
    }
    u64 *pdpt = phys_to_table(pml4[VMM_PML4_IDX(page_va)] & VMM_PHYS_MASK);
    if (!(pdpt[VMM_PDPT_IDX(page_va)] & VMM_F_PRESENT) || (pdpt[VMM_PDPT_IDX(page_va)] & VMM_F_HUGE)) {
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
        return -(int)EFAULT;
    }
    u64 *pd = phys_to_table(pdpt[VMM_PDPT_IDX(page_va)] & VMM_PHYS_MASK);
    if (!(pd[VMM_PD_IDX(page_va)] & VMM_F_PRESENT) || (pd[VMM_PD_IDX(page_va)] & VMM_F_HUGE)) {
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
        return -(int)EFAULT;
    }
    u64 *pt = phys_to_table(pd[VMM_PD_IDX(page_va)] & VMM_PHYS_MASK);
    u64 old_pte = pt[VMM_PT_IDX(page_va)];

    if (!(old_pte & VMM_F_COW)) {
        /* Not a COW page — either a genuine protection fault or already resolved
         * by another CPU; let the caller decide. */
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
        return -(int)EFAULT;
    }

    phys_addr_t old_phys = old_pte & VMM_PHYS_MASK;
    spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);

    /* Check refcount outside the vmm lock (cow_lock is separate). */
    uint16_t rc = vmm_page_refcount(old_phys);

    if (rc <= 1) {
        /* Last owner: promote in-place — add WRITE, clear COW.
         * Since this page is private to this address space and permissions are only
         * being relaxed (read-only -> writable), local invlpg is sufficient.
         * Omitting tlb_shootdown_all() avoids broadcast IPI storms on write faults. */
        irqf = spinlock_lock_irqsave(vmm_get_lock(space));
        /* Re-read: another CPU might have already broken it. */
        u64 cur = pt[VMM_PT_IDX(page_va)];
        if (cur & VMM_F_COW) {
            pt[VMM_PT_IDX(page_va)] = (cur | VMM_F_WRITE) & ~VMM_F_COW;
            invlpg(page_va);
            /* Reset refcount to 0 now that this page is private and unshared. */
            vmm_page_ref_dec(old_phys);
        }
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf);
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
    irqflags_t irqf2 = spinlock_lock_irqsave(vmm_get_lock(space));
    u64 cur = pt[VMM_PT_IDX(page_va)];
    if (cur & VMM_F_COW) {
        /* Still COW — install our copy. */
        pt[VMM_PT_IDX(page_va)] = (new_phys & VMM_PHYS_MASK)
                                  | (cur & ~VMM_PHYS_MASK & ~VMM_F_COW)
                                  | VMM_F_WRITE;
        invlpg(page_va);
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf2);
        tlb_shootdown_user(space, page_va, 1);
        /* Decrement the old frame's refcount; free if it hits zero. */
        uint16_t remaining = vmm_page_ref_dec(old_phys);
        if (remaining == 0)
            pmm_free_page(old_phys);
    } else {
        /* Another CPU already broke COW while we were copying — discard ours. */
        spinlock_unlock_irqrestore(vmm_get_lock(space), irqf2);
        pmm_free_page(new_phys);
    }
    return 0;
}

