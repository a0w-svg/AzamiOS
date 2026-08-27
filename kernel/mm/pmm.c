/* ============================================================================
 * AzamiOS — Physical Memory Manager: Buddy Allocator
 * File: kernel/mm/pmm.c
 *
 * Algorithm overview (binary buddy system):
 *
 *   Physical memory is divided into "blocks" whose sizes are powers of two
 *   times the page size (4 KB).  Blocks at order N have size 2^N pages.
 *
 *   Free blocks at each order are kept in a singly-linked intrusive list.
 *   The link pointer is stored IN the physical page itself (via the HHDM
 *   virtual mapping) so no external node storage is required.
 *
 *   Allocation:
 *     1. Find the smallest order ≥ requested order with a free block.
 *     2. Split the block in half repeatedly until we reach the target order.
 *        Each lower half ("buddy") is pushed onto its order's free list.
 *     3. Mark the block allocated in the bitmap.
 *     4. Return the physical address.
 *
 *   Freeing:
 *     1. Check if the buddy (block at the same order with address XOR size)
 *        is also free (bitmap check).
 *     2. If yes: remove buddy from its free list, merge, and recurse upward.
 *     3. Push the final merged block onto the free list for that order.
 *
 *   Complexity: O(log N) allocation and free, O(1) per merge step.
 *
 * Bitmap:
 *   A single bit per page frame tracks whether the frame is allocated.
 *   Bits are stored in a statically allocated array (covers up to 64 GB with
 *   a 2 MB bitmap at 4 KB granularity: 64 GB / 4 KB / 8 = 2 MB).
 *
 * SMP safety:
 *   A single spinlock protects the entire allocator.  This is acceptable for
 *   the boot phase and moderate kernel allocation rates.  A per-order lock
 *   or per-NUMA-node allocator can be added later.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "pmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"


/* Limine memory map types (from limine.h — we use the numeric values directly
 * to avoid pulling the full Limine header into every PMM consumer). */
#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

/* Limine memmap structures (layout-compatible, no limine.h required). */
typedef struct {
    u64  base;
    u64  length;
    u64  type;
} limine_memmap_entry_t;

typedef struct {
    u64                    revision;
    u64                    entry_count;
    limine_memmap_entry_t **entries;
} limine_memmap_response_t;

/* ── Bitmap ──────────────────────────────────────────────────────────────── */

/* 2 MB bitmap = 64 GB addressable at 4 KB granularity */
#define PMM_MAX_FRAMES  (64ULL * 1024 * 1024 * 1024 / PAGE_SIZE)
#define BITMAP_WORDS    (PMM_MAX_FRAMES / 64)

static u64 g_bitmap[BITMAP_WORDS];   /* 1 bit per frame, 1=allocated */
static u64 g_total_frames;
static u64 g_free_frames;

static __always_inline void bitmap_set(u64 frame) {
    if (unlikely(frame >= PMM_MAX_FRAMES)) return;
    g_bitmap[frame / 64] |=  (1ULL << (frame % 64));
}
static __always_inline void bitmap_clear(u64 frame) {
    if (unlikely(frame >= PMM_MAX_FRAMES)) return;
    g_bitmap[frame / 64] &= ~(1ULL << (frame % 64));
}
static __always_inline bool bitmap_test(u64 frame) {
    if (unlikely(frame >= PMM_MAX_FRAMES)) return true;
    return (g_bitmap[frame / 64] >> (frame % 64)) & 1ULL;
}

/* ── Free lists ──────────────────────────────────────────────────────────── */

#define PMM_BLOCK_MAGIC 0x504D4D31ULL

/* Each free block stores intrusive next/prev pointers, order, and magic
 * in physical memory via HHDM. */
typedef struct free_block {
    struct free_block *next;
    struct free_block *prev;
    u32 order;
    u64 magic;
} free_block_t;

static free_block_t *g_free_list[PMM_ORDER_COUNT];

/* ── Spinlock ─────────────────────────────────────────────────────────────── */
static spinlock_t g_pmm_lock = SPINLOCK_INIT;

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Frame index → physical address */
static inline phys_addr_t frame_to_phys(u64 frame) {
    return (phys_addr_t)(frame * PAGE_SIZE);
}
/* Physical address → frame index */
static inline u64 phys_to_frame(phys_addr_t p) {
    return p / PAGE_SIZE;
}
/* Physical address → kernel virtual address via HHDM */
static inline free_block_t *phys_to_block(phys_addr_t p) {
    return (free_block_t *)PHYS_TO_VIRT(p);
}

/* Push a block onto a free list and mark it free in the bitmap. */
static void free_list_push(u32 order, phys_addr_t phys)
{
    u64 frame = phys_to_frame(phys);
    u64 count = (u64)1 << order;
    for (u64 i = 0; i < count; i++) bitmap_clear(frame + i);

    free_block_t *blk  = phys_to_block(phys);
    blk->order         = order;
    blk->magic         = PMM_BLOCK_MAGIC;
    blk->next          = g_free_list[order];
    blk->prev          = NULL;

    if (g_free_list[order]) {
        g_free_list[order]->prev = blk;
    }
    g_free_list[order] = blk;
    g_free_frames     += count;
}

/* Pop a block from a free list and mark it allocated in the bitmap. */
static phys_addr_t free_list_pop(u32 order)
{
    free_block_t *blk = g_free_list[order];
    if (!blk) return 0;
    
    g_free_list[order] = blk->next;
    if (g_free_list[order]) {
        g_free_list[order]->prev = NULL;
    }

    blk->magic = 0;
    blk->next  = NULL;
    blk->prev  = NULL;

    phys_addr_t phys = VIRT_TO_PHYS((uintptr_t)blk);
    u64 frame  = phys_to_frame(phys);
    u64 count  = (u64)1 << order;
    for (u64 i = 0; i < count; i++) bitmap_set(frame + i);
    g_free_frames -= count;
    return phys;
}

/* Remove a specific block from a free list in O(1) during coalescing. */
static bool free_list_remove(u32 order, phys_addr_t phys)
{
    free_block_t *blk = phys_to_block(phys);
    if (blk->magic != PMM_BLOCK_MAGIC || blk->order != order) {
        return false;
    }

    if (blk->prev) {
        blk->prev->next = blk->next;
    } else {
        g_free_list[order] = blk->next;
    }
    if (blk->next) {
        blk->next->prev = blk->prev;
    }

    blk->magic = 0;
    blk->next  = NULL;
    blk->prev  = NULL;
    return true;
}

/* Compute the address of a block's buddy at the same order. */
static inline phys_addr_t buddy_of(phys_addr_t phys, u32 order)
{
    return phys ^ (phys_addr_t)(PAGE_SIZE << order);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void pmm_init(void *memmap_raw)
{
    limine_memmap_response_t *mm = (limine_memmap_response_t *)memmap_raw;
    if (!mm) {
        pr_debug("[PMM] CRITICAL: No memory map from bootloader!\n");
        return;
    }

    /* Mark the entire bitmap as allocated initially. */
    for (u64 i = 0; i < BITMAP_WORDS; i++) g_bitmap[i] = ~0ULL;

    /* Clear all free lists. */
    for (u32 o = 0; o < PMM_ORDER_COUNT; o++) g_free_list[o] = NULL;

    g_total_frames = 0;
    g_free_frames  = 0;

    /* Walk the Limine memory map and free usable regions. */
    for (u64 i = 0; i < mm->entry_count; i++) {
        limine_memmap_entry_t *e = mm->entries[i];

        /* Only add USABLE memory to the free pool.
         * BOOTLOADER_RECLAIMABLE can be freed later after we no longer need
         * boot structures (that's a future enhancement). */
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        phys_addr_t base = PAGE_ALIGN_UP(e->base);
        phys_addr_t end  = PAGE_ALIGN_DOWN(e->base + e->length);
        if (base >= end) continue;

        u64 frames = (end - base) / PAGE_SIZE;
        g_total_frames += frames;

        /* Free each aligned block from largest to smallest order. */
        phys_addr_t cur = base;
        while (cur < end) {
            /* Find the largest order block that fits here and is aligned. */
            u32 order = PMM_MAX_ORDER;
            while (order > 0) {
                phys_addr_t block_size = (phys_addr_t)(PAGE_SIZE << order);
                if ((cur % block_size) == 0 && (cur + block_size) <= end)
                    break;
                order--;
            }
            phys_addr_t block_size = (phys_addr_t)(PAGE_SIZE << order);
            free_list_push(order, cur);
            cur += block_size;
        }
    }

    pr_debug("[PMM] Buddy allocator ready: %llu MB free / %llu MB total\n",
            (unsigned long long)(g_free_frames  * PAGE_SIZE / (1024 * 1024)),
            (unsigned long long)(g_total_frames * PAGE_SIZE / (1024 * 1024)));
}

phys_addr_t pmm_alloc(u32 order)
{
    if (unlikely(order > PMM_MAX_ORDER)) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);

    /* Find the smallest order with a free block. */
    u32 found = order;
    while (found <= PMM_MAX_ORDER && !g_free_list[found]) found++;

    if (found > PMM_MAX_ORDER) {
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        pr_debug("[PMM] Out of memory (order=%u requested)\n", order);
        return 0;
    }

    /* Pop the block from its free list. */
    phys_addr_t blk = free_list_pop(found);

    /* Split down to the requested order, freeing the upper halves. */
    while (found > order) {
        found--;
        phys_addr_t buddy_phys = blk + (phys_addr_t)(PAGE_SIZE << found);
        free_list_push(found, buddy_phys);
    }

    spinlock_unlock_irqrestore(&g_pmm_lock, flags);
    return blk;
}

void pmm_free(phys_addr_t phys, u32 order)
{
    if (unlikely(!phys || order > PMM_MAX_ORDER)) return;
    BUG_ON(!IS_ALIGNED(phys, PAGE_SIZE << order));

    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);

    while (order < PMM_MAX_ORDER) {
        phys_addr_t buddy = buddy_of(phys, order);
        u64 buddy_frame   = phys_to_frame(buddy);

        /* Is the buddy free? (all frames in buddy block must be unallocated) */
        bool buddy_free = true;
        u64  count      = (u64)1 << order;
        for (u64 i = 0; i < count && buddy_free; i++) {
            if (bitmap_test(buddy_frame + i)) buddy_free = false;
        }

        if (!buddy_free) break;  /* Can't coalesce further */
        if (!free_list_remove(order, buddy)) break; /* Buddy already gone? */

        /* Account for the buddy's frames being removed from the free pool.
         * free_list_remove() does NOT touch g_free_frames, but the buddy was
         * previously counted as free.  Subtract now; free_list_push() below
         * will re-add the whole merged block's frame count. */
        g_free_frames -= count;

        /* Merge: the lower address is the new coalesced block. */
        phys  = MIN(phys, buddy);
        order++;
    }

    free_list_push(order, phys);
    spinlock_unlock_irqrestore(&g_pmm_lock, flags);
}

static u32 pages_to_order(size_t count)
{
    if (count > (1UL << PMM_MAX_ORDER)) {
        return (u32)-1;
    }
    u32 order = 0;
    while ((1UL << order) < count && order < PMM_MAX_ORDER) {
        order++;
    }
    return order;
}

phys_addr_t pmm_alloc_pages(size_t page_count)
{
    if (page_count == 0) return 0;
    u32 order = pages_to_order(page_count);
    if (order > PMM_MAX_ORDER) return 0;
    return pmm_alloc(order);
}

phys_addr_t pmm_alloc_32(u32 order)
{
    if (unlikely(order > PMM_MAX_ORDER)) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);

    u32 found = order;
    free_block_t *target_blk = NULL;
    while (found <= PMM_MAX_ORDER) {
        free_block_t *curr = g_free_list[found];
        while (curr) {
            phys_addr_t phys = VIRT_TO_PHYS((uintptr_t)curr);
            if (phys + ((phys_addr_t)(PAGE_SIZE << found)) <= 0x100000000ULL) {
                target_blk = curr;
                break;
            }
            curr = curr->next;
        }
        if (target_blk) break;
        found++;
    }

    if (!target_blk || found > PMM_MAX_ORDER) {
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        pr_debug("[PMM] Out of 32-bit low memory (order=%u requested)\n", order);
        return 0;
    }

    phys_addr_t blk_phys = VIRT_TO_PHYS((uintptr_t)target_blk);
    if (!free_list_remove(found, blk_phys)) {
        /* Free list / bitmap inconsistency — refuse rather than double-account. */
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        pr_debug("[PMM] pmm_alloc_32: free_list_remove failed (order=%u)\n", found);
        return 0;
    }
    u64 frame = phys_to_frame(blk_phys);
    u64 count = (u64)1 << found;
    for (u64 i = 0; i < count; i++) bitmap_set(frame + i);
    g_free_frames -= count;

    while (found > order) {
        found--;
        phys_addr_t buddy_phys = blk_phys + (phys_addr_t)(PAGE_SIZE << found);
        free_list_push(found, buddy_phys);
    }

    spinlock_unlock_irqrestore(&g_pmm_lock, flags);
    return blk_phys;
}

phys_addr_t pmm_alloc_pages_32(size_t page_count)
{
    if (page_count == 0) return 0;
    u32 order = pages_to_order(page_count);
    if (order > PMM_MAX_ORDER) return 0;
    return pmm_alloc_32(order);
}

void pmm_free_pages(phys_addr_t phys, size_t page_count)
{
    if (phys == 0 || page_count == 0) return;

    u32 order = pages_to_order(page_count);
    if (order <= PMM_MAX_ORDER) {
        pmm_free(phys, order);
        return;
    }

    /* Region larger than one buddy block (e.g. a 2 MB/1 GB huge-page frame):
     * free it as a run of naturally-aligned power-of-two blocks so nothing
     * leaks. `phys` for such regions is always at least MAX_ORDER-aligned. */
    phys_addr_t cur = phys;
    size_t left = page_count;
    while (left) {
        u32 o = PMM_MAX_ORDER;
        while (o > 0) {
            size_t blk = (size_t)1 << o;
            if (left >= blk && IS_ALIGNED(cur, (phys_addr_t)PAGE_SIZE << o)) break;
            o--;
        }
        pmm_free(cur, o);
        cur  += (phys_addr_t)PAGE_SIZE << o;
        left -= (size_t)1 << o;
    }
}

u64 pmm_get_free_pages(void)  { return g_free_frames;  }
u64 pmm_get_total_pages(void) { return g_total_frames; }

void pmm_dump_stats(void)
{
    pr_debug("[PMM] Free: %llu pages (%llu MB)  Total: %llu pages (%llu MB)\n",
            (unsigned long long)g_free_frames,
            (unsigned long long)(g_free_frames  * PAGE_SIZE / (1024*1024)),
            (unsigned long long)g_total_frames,
            (unsigned long long)(g_total_frames * PAGE_SIZE / (1024*1024)));
    for (u32 o = 0; o < PMM_ORDER_COUNT; o++) {
        u32 cnt = 0;
        for (free_block_t *b = g_free_list[o]; b; b = b->next) cnt++;
        if (cnt) kprintf("  order %2u (%4u KB): %u blocks\n",
                         o, (u32)(PAGE_SIZE << o) / 1024, cnt);
    }
}
