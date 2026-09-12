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
#include "../lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
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

/* ── Word-at-a-time range operations ─────────────────────────────────────── */
/*
 * Every allocator path below touches a whole buddy block at once, and a block
 * at PMM_MAX_ORDER is 1024 frames. Walking that a bit at a time is 1024 loads,
 * shifts and stores where sixteen masked word operations do the identical job,
 * and pmm_alloc()/pmm_free() run it on every allocation and every coalescing
 * step. The loops below cost one iteration per 64 frames.
 *
 * The set and clear variants also return how many bits they actually changed.
 * That is not used for accounting — g_free_frames follows the block size, for
 * the reasons free_list_push() explains — but it is what lets free_list_pop()
 * notice a block that was on a free list while still marked allocated. POPCNT
 * makes that check cost nothing on top of the store that was happening anyway.
 */

/* Mask of @n bits starting at bit @bit within one word. Every caller here
 * keeps bit + n <= 64 (n is clamped to what's left in the word before this
 * runs), so BZHI's own "n >= 64 leaves the source unchanged" rule already
 * covers the n == 64 case without a separate branch: hw_bzhi64(~0ULL, 64)
 * degenerates to ~0ULL, which is exactly the old special case. */
static __always_inline u64 word_mask(u64 bit, u64 n)
{
    return hw_bzhi64(~0ULL, (u32)n) << bit;
}

/* Clamp a range to the bitmap, mirroring the per-bit helpers' habit of
 * silently ignoring frames past the end rather than faulting. */
static __always_inline bool range_clamp(u64 frame, u64 *count)
{
    if (unlikely(frame >= PMM_MAX_FRAMES)) return false;
    if (unlikely(*count > PMM_MAX_FRAMES - frame)) *count = PMM_MAX_FRAMES - frame;
    return *count != 0;
}

/* Mark [frame, frame+count) allocated. Returns the number that were free. */
static u64 bitmap_range_set(u64 frame, u64 count)
{
    if (!range_clamp(frame, &count)) return 0;
    u64 changed = 0;
    while (count) {
        u64 bit = frame % 64;
        u64 n   = 64 - bit;
        if (n > count) n = count;
        u64 mask = word_mask(bit, n);
        u64 *w = &g_bitmap[frame / 64];
        changed += hw_popcnt64(hw_andn64(*w, mask));
        *w |= mask;
        frame += n;
        count -= n;
    }
    return changed;
}

/* Mark [frame, frame+count) free. Returns the number that were allocated. */
static u64 bitmap_range_clear(u64 frame, u64 count)
{
    if (!range_clamp(frame, &count)) return 0;
    u64 changed = 0;
    while (count) {
        u64 bit = frame % 64;
        u64 n   = 64 - bit;
        if (n > count) n = count;
        u64 mask = word_mask(bit, n);
        u64 *w = &g_bitmap[frame / 64];
        changed += hw_popcnt64(*w & mask);
        *w &= ~mask;
        frame += n;
        count -= n;
    }
    return changed;
}

/* True when every frame in the range is unallocated. A range running past the
 * end of the bitmap reads as allocated, matching bitmap_test(). */
static bool bitmap_range_is_clear(u64 frame, u64 count)
{
    while (count) {
        if (unlikely(frame >= PMM_MAX_FRAMES)) return false;
        u64 bit = frame % 64;
        u64 n   = 64 - bit;
        if (n > count) n = count;
        if (g_bitmap[frame / 64] & word_mask(bit, n)) return false;
        frame += n;
        count -= n;
    }
    return true;
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
static u32           g_free_orders_mask = 0;

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
    bitmap_range_clear(frame, count);

    free_block_t *blk  = phys_to_block(phys);
    blk->order         = order;
    blk->magic         = PMM_BLOCK_MAGIC;
    blk->next          = g_free_list[order];
    blk->prev          = NULL;

    if (g_free_list[order]) {
        g_free_list[order]->prev = blk;
    }
    g_free_list[order] = blk;
    g_free_orders_mask |= (1U << order);
    /* Deliberately the whole block, not just the frames this call transitioned:
     * when pmm_free() coalesces, the buddy's frames are already clear here and
     * it has already subtracted them, so the two adjustments only balance if
     * this one covers the merged block in full. See the invariant note in
     * pmm_alloc_32(). */
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
    } else {
        g_free_orders_mask &= ~(1U << order);
    }

    blk->magic = 0;
    blk->next  = NULL;
    blk->prev  = NULL;

    phys_addr_t phys = VIRT_TO_PHYS((uintptr_t)blk);
    u64 frame  = phys_to_frame(phys);
    u64 count  = (u64)1 << order;
    /* Everything on a free list must be clear in the bitmap. Anything else is
     * the free-list/bitmap divergence the allocator has no other way to
     * notice — the block would be handed out while still marked allocated, and
     * the next coalesce would silently refuse to merge it forever. */
    u64 was_free = bitmap_range_set(frame, count);
    if (unlikely(was_free != count)) {
        pr_debug("[PMM] free list/bitmap divergence: order=%u frame=%llu "
                 "%llu of %llu frames were already allocated\n",
                 order, (unsigned long long)frame,
                 (unsigned long long)(count - was_free),
                 (unsigned long long)count);
    }
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
        if (!g_free_list[order]) {
            g_free_orders_mask &= ~(1U << order);
        }
    }
    if (blk->next) {
        blk->next->prev = blk->prev;
    } else if (!blk->prev && !g_free_list[order]) {
        g_free_orders_mask &= ~(1U << order);
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

    /* Mark the entire bitmap as allocated initially. g_bitmap is sized for
     * the maximum supported 64 GB regardless of how much RAM this machine
     * actually has (2 MB, always — see BITMAP_WORDS), so this runs on
     * every boot regardless of installed RAM. A hand-written word-at-a-time
     * loop compiles to one `mov [mem+idx*8], imm64` per two words — 131072
     * discrete instructions for 2 MB; the kernel's own memset() (kernel/lib/
     * string.c) picks `rep stosb` when the CPU advertises ERMS, which does
     * the same fill as one microcoded operation. */
    memset(g_bitmap, 0xFF, sizeof(g_bitmap));

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

    /* O(1) search for the smallest non-empty order >= order via hardware TZCNT */
    u32 avail = g_free_orders_mask & ~((1U << order) - 1);
    if (unlikely(!avail)) {
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        pr_debug("[PMM] Out of memory (order=%u requested)\n", order);
        return 0;
    }
    u32 found = hw_ctz32(avail);

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

phys_addr_t pmm_alloc_page_zeroed(void)
{
    phys_addr_t p = pmm_alloc(0);
    if (p) {
        hw_clear_page((void *)PHYS_TO_VIRT(p));
    }
    return p;
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
        u64  count      = (u64)1 << order;
        bool buddy_free = bitmap_range_is_clear(buddy_frame, count);

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
    /* Smallest order such that 2^order >= count, i.e. ceil(log2(count)):
     * count <= 1 needs no bits at all (order 0), and otherwise
     * 64 - clz64(count - 1) lands exactly on it (count a power of two
     * includes its own bit in count-1's top position; anything above the
     * next-lower power of two carries into the same top bit as the power of
     * two above it). Replaces a loop that ran up to PMM_MAX_ORDER times with
     * one LZCNT — this runs on every multi-page alloc/free. */
    if (count <= 1) return 0;
    u32 order = 64 - hw_clz64((u64)count - 1);
    return (order > PMM_MAX_ORDER) ? PMM_MAX_ORDER : order;
}

/* BUG-U note: pmm_alloc_pages(N) allocates the smallest 2^order block that
 * holds N pages (2^order >= N).  The CALLER must remember the exact `page_count`
 * it passed and use the SAME value when calling pmm_free_pages().  Passing a
 * different count produces mis-sized frees that corrupt the buddy free list.
 * If you need to know the actual allocated size, compute (1 << pages_to_order(N))
 * yourself or use pmm_alloc()/pmm_free() with an explicit order instead. */
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
    /* BUG-7 hardening: unlike pmm_alloc() which delegates accounting to
     * free_list_pop(), this path calls free_list_remove() which does NOT touch
     * g_free_frames.  We must manually subtract the block we removed (count
     * frames at order `found`) here, BEFORE the split loop calls
     * free_list_push() for each buddy — free_list_push adds (1<<buddy_order)
     * frames back each iteration.  Net effect after the loop:
     *   −(1<<found) + [(1<<(found−1)) + … + (1<<order)] = −(1<<order)
     * which is exactly the number of frames handed to the caller.  Any change
     * to this block (reordering the decrement, changing the split loop) MUST
     * preserve this invariant or g_free_frames will drift. */
    u64 frame = phys_to_frame(blk_phys);
    u64 count = (u64)1 << found;
    if (unlikely(bitmap_range_set(frame, count) != count)) {
        pr_debug("[PMM] free list/bitmap divergence in alloc_32: order=%u "
                 "frame=%llu\n", found, (unsigned long long)frame);
    }
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

/* BUG-AC fix: g_free_frames is written under g_pmm_lock but read here without
 * any lock.  On x86-64 a 64-bit aligned load is atomic at the hardware level,
 * but the C standard still considers an un-annotated plain load a data race
 * (UB).  __ATOMIC_RELAXED costs nothing on x86 and makes the intent explicit. */
u64 pmm_get_free_pages(void)  { return __atomic_load_n(&g_free_frames,  __ATOMIC_RELAXED); }
u64 pmm_get_total_pages(void) { return __atomic_load_n(&g_total_frames, __ATOMIC_RELAXED); }

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
