/* ============================================================================
 * AzamiOS — Physical Memory Manager: Zoned Buddy Allocator
 * File: kernel/mm/pmm.c
 *
 * Algorithm overview (binary buddy system):
 *
 *   Physical memory is divided into "blocks" whose sizes are powers of two
 *   times the page size (4 KB).  Blocks at order N have size 2^N pages.
 *
 *   Free blocks at each order are kept in a doubly-linked intrusive list.
 *   The link pointers are stored IN the physical page itself (via the HHDM
 *   virtual mapping) so no external node storage is required.
 *
 *   Allocation:
 *     1. Find the smallest order >= requested order with a free block.
 *        The per-zone bitmask of non-empty orders makes that one TZCNT.
 *     2. Split the block in half repeatedly until we reach the target order.
 *        Each upper half ("buddy") is pushed onto its order's free list.
 *     3. Mark the returned block allocated in the bitmap.
 *     4. Return the physical address.
 *
 *   Freeing:
 *     1. Mark the block's frames free in the bitmap.
 *     2. Check if the buddy (block at the same order with address XOR size)
 *        is also entirely free.
 *     3. If yes: remove buddy from its free list, merge, and repeat upward.
 *     4. Push the final merged block onto the free list for that order.
 *
 *   Complexity: O(log N) allocation and free, O(1) per merge step.
 *
 * Three layers sit on top of that core, and the file is organised in that
 * order:
 *
 *   Bitmap        One bit per page frame, 1 = allocated. It is the authority
 *                 on whether a frame is in use, and it is what makes the
 *                 coalescing test safe: the buddy's own header claims an order
 *                 and a magic, but an *allocated* buddy's contents are
 *                 arbitrary (possibly attacker-chosen) bytes, so a header that
 *                 says "I am a free order-4 block" is only believed after the
 *                 bitmap agrees every frame in it is free.
 *
 *                 The bitmap is touched only where a frame actually changes
 *                 hands — once per pmm_alloc() over the returned block, once
 *                 per pmm_free() over the freed block, once per usable region
 *                 at init. Splitting and coalescing move blocks between free
 *                 lists without changing any frame's allocated-ness, so they
 *                 do not touch it at all. That is what keeps an order-0
 *                 allocation carved out of an order-18 block at one bitmap
 *                 word instead of eight thousand.
 *
 *   Zones         Two of them, split at the 4 GB line: ZONE_DMA32 below,
 *                 ZONE_NORMAL at or above. Every block size up to
 *                 PMM_MAX_ORDER (1 GB) divides 4 GB evenly and blocks are
 *                 naturally aligned, so a block never straddles the boundary
 *                 and a block's buddy is always in the same zone — the buddy
 *                 arithmetic needs no zone check at all. pmm_alloc_32() then
 *                 becomes the same O(1) mask lookup as pmm_alloc() instead of
 *                 walking free lists hunting for a low block, and pmm_alloc()
 *                 prefers ZONE_NORMAL so ordinary kernel allocations stop
 *                 eating the memory that 32-bit DMA devices are the only
 *                 consumers of.
 *
 *   Per-CPU cache A small stack of order-0 frames per (CPU, zone), Linux's
 *                 per-cpu pageset in miniature. Single-page alloc and free —
 *                 which is almost all of the traffic, every page table, every
 *                 slab refill, every COW fault — pops or pushes it with
 *                 interrupts merely disabled: no lock, no shared cache line.
 *                 The global lock is taken once per PCP_BATCH pages instead of
 *                 once per page. See the section comment there for why a
 *                 lock-free per-CPU structure is safe here.
 *
 * SMP safety:
 *   One spinlock protects every zone's free lists and the bitmap. With the
 *   per-CPU caches in front of it, the order-0 path reaches it once per batch,
 *   so a single lock is no longer the bottleneck it would otherwise be.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "pmm.h"
#include "../lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../arch/x86_64/cpu/smp.h"
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

static __always_inline bool bitmap_test(u64 frame) {
    if (unlikely(frame >= PMM_MAX_FRAMES)) return true;
    return (g_bitmap[frame / 64] >> (frame % 64)) & 1ULL;
}

/* ── Word-at-a-time range operations ─────────────────────────────────────── */
/*
 * Every allocator path below touches a whole buddy block at once, and a block
 * at PMM_MAX_ORDER is 262144 frames. Walking that a bit at a time is a quarter
 * of a million loads, shifts and stores where four thousand masked word
 * operations do the identical job. The loops below cost one iteration per 64
 * frames.
 *
 * The set and clear variants also return how many bits they actually changed.
 * That is what turns a silent corruption into a diagnosable one: a free whose
 * frames were already free is a double free, and an allocation whose frames
 * were already allocated is free-list/bitmap divergence. POPCNT makes both
 * checks cost nothing on top of the store that was happening anyway.
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

/* ── Zones and free lists ────────────────────────────────────────────────── */

#define PMM_BLOCK_MAGIC 0x504D4D31ULL

/* The 4 GB line, above which a 32-bit bus address cannot reach. */
#define PMM_DMA32_LIMIT 0x100000000ULL

/* Each free block stores intrusive next/prev pointers, order, and magic
 * in physical memory via HHDM. */
typedef struct free_block {
    struct free_block *next;
    struct free_block *prev;
    u32 order;
    u64 magic;
} free_block_t;

typedef struct {
    const char   *name;
    free_block_t *free_list[PMM_ORDER_COUNT];
    u64           blocks[PMM_ORDER_COUNT];  /* free blocks at each order */
    u32           orders_mask;              /* bit N set iff free_list[N] != NULL */
    u64           free_frames;              /* frames on this zone's free lists */
    u64           total_frames;             /* frames this zone was born with */
} pmm_zone_t;

static pmm_zone_t g_zone[PMM_ZONE_COUNT];

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
/* Which zone owns this address. Blocks never straddle the boundary (see the
 * file header), so the zone of a block is the zone of its base. */
static inline u32 zone_of(phys_addr_t p) {
    return (p < PMM_DMA32_LIMIT) ? PMM_ZONE_DMA32 : PMM_ZONE_NORMAL;
}
/* Compute the address of a block's buddy at the same order. */
static inline phys_addr_t buddy_of(phys_addr_t phys, u32 order)
{
    return phys ^ (phys_addr_t)(PAGE_SIZE << order);
}

/*
 * The three list primitives below maintain free-list membership and the
 * counters that shadow it (per-order block count, per-zone frame count, and
 * the non-empty-orders bitmask). They deliberately do NOT touch the bitmap:
 * splitting and coalescing move a block between orders without any frame
 * changing hands, and the callers that really do change a frame's
 * allocated-ness — pmm_alloc(), pmm_free(), pmm_init() — update the bitmap
 * once, over exactly the range that changed.
 *
 * Invariant, relied on everywhere below: a block is on a free list if and only
 * if every one of its frames is clear in the bitmap.
 */

static __always_inline void zone_mark_order(pmm_zone_t *z, u32 order)
{
    if (z->free_list[order]) z->orders_mask |=  (1U << order);
    else                     z->orders_mask &= ~(1U << order);
}

/* Insert a block at the head of its order's free list. */
static void blk_insert(pmm_zone_t *z, u32 order, phys_addr_t phys)
{
    free_block_t *blk = phys_to_block(phys);
    blk->order = order;
    blk->magic = PMM_BLOCK_MAGIC;
    blk->next  = z->free_list[order];
    blk->prev  = NULL;
    if (blk->next) blk->next->prev = blk;
    z->free_list[order] = blk;

    z->blocks[order]++;
    z->free_frames += (u64)1 << order;
    z->orders_mask |= (1U << order);
}

/* Unlink a block that is known to be on @order's free list. */
static void blk_unlink(pmm_zone_t *z, u32 order, free_block_t *blk)
{
    if (blk->prev) blk->prev->next = blk->next;
    else           z->free_list[order] = blk->next;
    if (blk->next) blk->next->prev = blk->prev;

    blk->magic = 0;
    blk->next  = NULL;
    blk->prev  = NULL;

    z->blocks[order]--;
    z->free_frames -= (u64)1 << order;
    zone_mark_order(z, order);
}

/* Pop the head block of an order's free list, or 0 when it is empty. */
static phys_addr_t blk_pop(pmm_zone_t *z, u32 order)
{
    free_block_t *blk = z->free_list[order];
    if (!blk) return 0;
    blk_unlink(z, order, blk);
    return VIRT_TO_PHYS((uintptr_t)blk);
}

/* Remove one specific block during coalescing. Returns false when the memory
 * at @phys is not in fact a free block of this order — the caller has already
 * confirmed via the bitmap that the frames are free, so this only fails when
 * the region is free as several smaller blocks rather than one of @order, in
 * which case there is nothing to merge with. */
static bool blk_remove(pmm_zone_t *z, u32 order, phys_addr_t phys)
{
    free_block_t *blk = phys_to_block(phys);
    if (blk->magic != PMM_BLOCK_MAGIC || blk->order != order) return false;
    blk_unlink(z, order, blk);
    return true;
}

/* ── Core allocate / free, called with g_pmm_lock held ───────────────────── */

/*
 * Carve a block of exactly @order out of @z, splitting a larger one if that is
 * all the zone has. Returns 0 when the zone cannot satisfy the request.
 *
 * The bitmap is updated once, over the returned block only. The upper halves
 * produced by the split stay free and stay clear, so they cost nothing but a
 * list insertion each — which is why splitting an order-18 block down to a
 * single page is 18 pointer updates rather than a quarter-million bit writes.
 */
static phys_addr_t zone_alloc_locked(pmm_zone_t *z, u32 order)
{
    u32 avail = z->orders_mask & ~((1U << order) - 1);
    if (!avail) return 0;

    u32 found = hw_ctz32(avail);
    phys_addr_t blk = blk_pop(z, found);
    if (unlikely(!blk)) {
        /* orders_mask said this order had a block and the list disagreed. */
        pr_debug("[PMM] %s: orders_mask/free_list divergence at order %u\n",
                 z->name, found);
        z->orders_mask &= ~(1U << found);
        return 0;
    }

    /* The split loop below writes a block header into each half it hands back:
     * up to PMM_MAX_ORDER distinct cache lines, every one of them cold (a free
     * page's contents are touched by nothing else) and every one of them known
     * in advance. Issuing the prefetches first lets those misses overlap each
     * other instead of serialising one per iteration — on the critical path of
     * a lock every core shares. There is no false-sharing risk to weigh
     * against it: these lines belong to blocks that are provably free, so no
     * other CPU holds them. */
    for (u32 o = order; o < found; o++)
        hw_prefetch_write(phys_to_block(blk + (phys_addr_t)(PAGE_SIZE << o)));

    while (found > order) {
        found--;
        blk_insert(z, found, blk + (phys_addr_t)(PAGE_SIZE << found));
    }

    u64 frame = phys_to_frame(blk);
    u64 count = (u64)1 << order;
    u64 was_free = bitmap_range_set(frame, count);
    if (unlikely(was_free != count)) {
        /* A block reached a free list while some of its frames were still
         * marked allocated. Left unreported it is invisible: the frames would
         * be handed out twice and their buddies could never coalesce again. */
        pr_debug("[PMM] %s: free list/bitmap divergence: order=%u frame=%llu "
                 "%llu of %llu frames were already allocated\n",
                 z->name, order, (unsigned long long)frame,
                 (unsigned long long)(count - was_free),
                 (unsigned long long)count);
    }
    return blk;
}

/* Try ZONE_NORMAL first, then ZONE_DMA32. Ordinary kernel allocations have no
 * addressing constraint, so spending high memory first leaves the low 4 GB for
 * the 32-bit DMA devices that are the only callers who cannot use anything
 * else. */
static phys_addr_t zone_alloc_any_locked(u32 order)
{
    phys_addr_t p = zone_alloc_locked(&g_zone[PMM_ZONE_NORMAL], order);
    if (!p) p = zone_alloc_locked(&g_zone[PMM_ZONE_DMA32], order);
    return p;
}

/*
 * Return one block and coalesce as far as its buddies allow.
 *
 * The bitmap is cleared once, over the incoming block. Every buddy absorbed
 * afterwards was already free and already clear, so the merge loop is pure
 * list surgery.
 *
 * Returns false when the block was refused — see the double-free check.
 */
static bool zone_free_locked(phys_addr_t phys, u32 order)
{
    pmm_zone_t *z = &g_zone[zone_of(phys)];

    u64 frame = phys_to_frame(phys);
    u64 count = (u64)1 << order;
    u64 was_allocated = bitmap_range_clear(frame, count);
    if (unlikely(was_allocated != count)) {
        /* Some of these frames were already free. Either this exact block is
         * being freed twice, or it is being freed at an order it was not
         * allocated at. Inserting it would put one region on two free lists
         * at once and corrupt the allocator permanently, so refuse: leaking
         * the block is recoverable, and the message below is the only lead
         * the offending caller leaves.
         *
         * The bits this call cleared are deliberately NOT put back. For a
         * plain double free every frame was already clear, so the clear was a
         * no-op and the state is exactly as it was — the block is still on its
         * free list, still consistent. For a mis-sized free the frames that
         * really were allocated are now clear but on no list, which leaks them
         * and nothing more: coalescing still has to find a valid block header
         * at the buddy's base before it will merge anything. Setting the bits
         * back would instead mark a live free block allocated and hand the
         * same frames out twice, which is the outcome this branch exists to
         * prevent. */
        pr_debug("[PMM] refusing double free: phys=0x%llx order=%u "
                 "(%llu of %llu frames were already free)\n",
                 (unsigned long long)phys, order,
                 (unsigned long long)(count - was_allocated),
                 (unsigned long long)count);
        return false;
    }

    while (order < PMM_MAX_ORDER) {
        phys_addr_t buddy = buddy_of(phys, order);

        /* The buddy's own header claims an order and a magic, but an allocated
         * buddy holds arbitrary bytes that could spell both. The bitmap is the
         * authority: only once it says every frame of the buddy is free is the
         * header worth reading. This bails on the first allocated word, so the
         * common "buddy is in use" case is a single load. */
        if (!bitmap_range_is_clear(phys_to_frame(buddy), (u64)1 << order)) break;
        if (!blk_remove(z, order, buddy)) break;

        phys = MIN(phys, buddy);
        order++;
    }

    blk_insert(z, order, phys);
    return true;
}

/* ── Per-CPU page caches ─────────────────────────────────────────────────── *
 *
 * Almost every physical allocation in this kernel is a single page: a page
 * table level, a slab refill, a COW fault, a kernel stack frame's backing
 * store. Sending each of those through the global lock makes one cache line
 * the meeting point of every core, and the line ping-pongs on work that shares
 * nothing but the allocator.
 *
 * Each (CPU, zone) pair therefore keeps a small stack of order-0 frames. The
 * fast path pops or pushes it with interrupts merely disabled — no lock, no
 * shared line — and reaches the buddy only when the stack runs dry (one bulk
 * refill) or overflows (one bulk return). Pages are interchangeable, so the
 * CPU that frees one need not be the one that allocated it.
 *
 * What makes the lock-free path safe is that a row is only ever mutated by its
 * owning CPU with interrupts off: an interrupt-context allocation on the same
 * core is excluded, and no other core touches the row. Nothing here drains a
 * remote CPU's cache, which is why pmm_drain_local() is documented as local
 * only and why the OOM path drains just the current core.
 *
 * Frames sitting in a cache have left the buddy's free_frames, so
 * pmm_get_free_pages() adds the cached counts back in — otherwise the
 * page-fault handler's low-memory check would see up to a few thousand pages
 * of phantom pressure. The counts are read without synchronisation and are an
 * estimate, exactly like the slab statistics next door.
 *
 * Disabled until pmm_enable_percpu(), which the boot path calls once smp_init()
 * has published a valid GS base on every core; before that the BSP is
 * single-threaded and allocates straight from the zones.
 */

#define PCP_CAP    64               /* frames cached per CPU per zone       */
#define PCP_BATCH  32               /* moved in/out on a miss / overflow    */

typedef struct {
    u32         count;
    phys_addr_t page[PCP_CAP];
} pcp_t;

/* [cpu][zone]. 66 KB of BSS at the 64-core ceiling, and at most
 * SMP_MAX_CPUS * PMM_ZONE_COUNT * PCP_CAP frames (32 MB) held out of the
 * buddy — bounded, and small next to any machine with that many cores. */
static pcp_t g_pcp[SMP_MAX_CPUS][PMM_ZONE_COUNT] __attribute__((aligned(64)));

static bool g_pmm_percpu_ready = false;

static __always_inline irqflags_t irq_save(void)
{
    irqflags_t f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}
static __always_inline void irq_restore(irqflags_t f)
{
    __asm__ volatile("pushq %0; popfq" : : "r"(f) : "memory");
}

/* Pull up to @want order-0 frames out of one zone under a single lock
 * acquisition. Returns how many were obtained; fewer than @want (possibly 0)
 * means the zone is exhausted. */
static u32 zone_bulk_alloc(u32 zone, phys_addr_t *out, u32 want)
{
    u32 got = 0;
    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
    while (got < want) {
        phys_addr_t p = zone_alloc_locked(&g_zone[zone], 0);
        if (!p) break;
        out[got++] = p;

        /* Refilling a cache is PCP_BATCH pops in a row, and each pop reads the
         * ->next field out of a page nothing has touched since it was freed.
         * Left alone that is a chain of dependent cache misses, each one
         * waiting on the last, all of it inside the global lock. Starting the
         * fetch for the head the next iteration will pop turns the chain into
         * an overlapped walk. PREFETCHT0 rather than a read hint because the
         * pop writes the header as well as reads it. */
        free_block_t *nxt = g_zone[zone].free_list[0];
        if (nxt) hw_prefetch_write(nxt);
    }
    spinlock_unlock_irqrestore(&g_pmm_lock, flags);
    return got;
}

/* Hand @n order-0 frames back to the buddy under a single lock acquisition. */
static void zone_bulk_free(phys_addr_t *pages, u32 n)
{
    if (!n) return;
    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
    for (u32 i = 0; i < n; i++) zone_free_locked(pages[i], 0);
    spinlock_unlock_irqrestore(&g_pmm_lock, flags);
}

/* Called with interrupts off on the owning CPU. */
static phys_addr_t pcp_alloc(u32 cpu, u32 zone)
{
    pcp_t *p = &g_pcp[cpu][zone];
    if (p->count == 0) {
        /* Don't take the global lock only to be told the zone has nothing.
         * A zone with no free block at any order has orders_mask == 0, and a
         * refill from it could only fail. This matters more than it looks:
         * pmm_alloc() tries ZONE_NORMAL first, and on any machine with 4 GB of
         * RAM or less ZONE_NORMAL is permanently empty — without this check
         * every single page allocation would take the global lock, learn
         * nothing, and drop it again, which is precisely the cost the per-CPU
         * cache exists to avoid.
         *
         * The load is unsynchronised and may be a moment stale. The cost of
         * being wrong is one refill attempt that fails, or one fallback to the
         * other zone — never correctness. It is also read only on a refill,
         * once per PCP_BATCH allocations, so a zone whose mask does churn does
         * not put a contended cache line on the hot path. */
        if (__atomic_load_n(&g_zone[zone].orders_mask, __ATOMIC_RELAXED) == 0)
            return 0;

        u32 got = zone_bulk_alloc(zone, p->page, PCP_BATCH);
        if (got == 0) return 0;
        p->count = got;
    }
    return p->page[--p->count];
}

/* Called with interrupts off on the owning CPU. */
static void pcp_free(u32 cpu, phys_addr_t phys)
{
    pcp_t *p = &g_pcp[cpu][zone_of(phys)];
    if (p->count == PCP_CAP) {
        /* Spill the coldest half. The freshly freed frames at the top of the
         * stack are the ones whose cache lines are still warm, so they are the
         * ones worth keeping; the bottom half has been sitting untouched and
         * is what the buddy should get back for coalescing. */
        zone_bulk_free(&p->page[0], PCP_BATCH);
        __builtin_memmove(&p->page[0], &p->page[PCP_BATCH],
                          (PCP_CAP - PCP_BATCH) * sizeof(p->page[0]));
        p->count = PCP_CAP - PCP_BATCH;
    }
    p->page[p->count++] = phys;
}

void pmm_enable_percpu(void)
{
    g_pmm_percpu_ready = true;
    pr_debug("[PMM] per-CPU page caches active (cap %d, batch %d)\n",
             PCP_CAP, PCP_BATCH);
}

void pmm_drain_local(void)
{
    if (!g_pmm_percpu_ready) return;

    for (u32 zone = 0; zone < PMM_ZONE_COUNT; zone++) {
        phys_addr_t tmp[PCP_CAP];
        u32 n;

        irqflags_t f = irq_save();
        pcp_t *p = &g_pcp[smp_current_cpu_id()][zone];
        n = p->count;
        for (u32 i = 0; i < n; i++) tmp[i] = p->page[i];
        p->count = 0;
        irq_restore(f);

        zone_bulk_free(tmp, n);
    }
}

/* Total frames parked across every CPU's cache for one zone. Raced against
 * live pushes and pops by design; every caller wants an estimate. */
static u64 pcp_cached(u32 zone)
{
    if (!g_pmm_percpu_ready) return 0;
    u64 n = 0;
    u32 ncpu = smp_cpu_count();
    if (ncpu > SMP_MAX_CPUS) ncpu = SMP_MAX_CPUS;
    for (u32 c = 0; c < ncpu; c++)
        n += __atomic_load_n(&g_pcp[c][zone].count, __ATOMIC_RELAXED);
    return n;
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

    static const char *zone_names[PMM_ZONE_COUNT] = { "DMA32", "Normal" };
    for (u32 zi = 0; zi < PMM_ZONE_COUNT; zi++) {
        pmm_zone_t *z = &g_zone[zi];
        z->name         = zone_names[zi];
        z->orders_mask  = 0;
        z->free_frames  = 0;
        z->total_frames = 0;
        for (u32 o = 0; o < PMM_ORDER_COUNT; o++) {
            z->free_list[o] = NULL;
            z->blocks[o]    = 0;
        }
    }
    for (u32 c = 0; c < SMP_MAX_CPUS; c++)
        for (u32 zi = 0; zi < PMM_ZONE_COUNT; zi++)
            g_pcp[c][zi].count = 0;

    /* The bitmap is sized at compile time (kmalloc doesn't exist yet to size
     * it from the real memmap total), so it can only ever track up to
     * PMM_MAX_FRAMES frames. Pushing frames beyond that onto the free lists
     * anyway would let pmm_alloc() hand them out while the bitmap still
     * reads them as allocated: their buddies could never coalesce (a
     * permanent, silent fragmentation leak), and every alloc/free of one
     * would trip the free-list/bitmap divergence check in zone_alloc_locked().
     * Clamp to what the bitmap can actually track instead, and say so once,
     * so a machine with more real RAM than this allocator supports loses
     * that memory visibly at boot rather than getting a subtly unsound
     * allocator. */
    const phys_addr_t pmm_max_addr = (phys_addr_t)PMM_MAX_FRAMES * PAGE_SIZE;
    bool warned_truncated = false;

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

        if (base >= pmm_max_addr || end > pmm_max_addr) {
            if (!warned_truncated) {
                pr_debug("[PMM] WARNING: usable memory above %llu GB ignored -- "
                         "allocator bitmap only tracks that much\n",
                         (unsigned long long)(pmm_max_addr / (1024 * 1024 * 1024)));
                warned_truncated = true;
            }
            if (base >= pmm_max_addr) continue;
            end = pmm_max_addr;
        }

        /* One word-at-a-time pass marks the whole region free; the block loop
         * below then only builds list structure. */
        u64 frames = (end - base) / PAGE_SIZE;
        bitmap_range_clear(phys_to_frame(base), frames);

        /* Free each aligned block from largest to smallest order. A block is
         * naturally aligned and at most 1 GB, and 4 GB is a multiple of 1 GB,
         * so no block produced here can straddle the zone boundary. */
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
            pmm_zone_t *z = &g_zone[zone_of(cur)];
            blk_insert(z, order, cur);
            z->total_frames += (u64)1 << order;
            cur += block_size;
        }
    }

    u64 total = 0, free = 0;
    for (u32 zi = 0; zi < PMM_ZONE_COUNT; zi++) {
        total += g_zone[zi].total_frames;
        free  += g_zone[zi].free_frames;
    }
    pr_debug("[PMM] Buddy allocator ready: %llu MB free / %llu MB total "
             "(DMA32 %llu MB, Normal %llu MB)\n",
             (unsigned long long)(free  * PAGE_SIZE / (1024 * 1024)),
             (unsigned long long)(total * PAGE_SIZE / (1024 * 1024)),
             (unsigned long long)(g_zone[PMM_ZONE_DMA32].total_frames * PAGE_SIZE / (1024 * 1024)),
             (unsigned long long)(g_zone[PMM_ZONE_NORMAL].total_frames * PAGE_SIZE / (1024 * 1024)));
}

phys_addr_t pmm_alloc(u32 order)
{
    phys_addr_t ret = 0;
    if (unlikely(order > PMM_MAX_ORDER)) return 0;

    if (order == 0 && g_pmm_percpu_ready) {
        irqflags_t f = irq_save();
        u32 cpu = smp_current_cpu_id();
        ret = pcp_alloc(cpu, PMM_ZONE_NORMAL);
        if (!ret) ret = pcp_alloc(cpu, PMM_ZONE_DMA32);
        irq_restore(f);
    }
    
    if (!ret) {
        irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
        ret = zone_alloc_any_locked(order);
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);

        /* Nothing of this order is on a free list, but this core may be sitting on
         * cached single pages that would coalesce into one. Give them back and try
         * once more before calling it out of memory. */
        if (!ret && g_pmm_percpu_ready) {
            pmm_drain_local();
            flags = spinlock_lock_irqsave(&g_pmm_lock);
            ret = zone_alloc_any_locked(order);
            spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        }
    }

    if (likely(ret)) {
        u64 frames = 1ULL << order;
        for (u64 i = 0; i < frames; i++) {
            hw_clear_page((void *)PHYS_TO_VIRT(ret + i * PAGE_SIZE));
        }
    } else {
        pr_debug("[PMM] Out of memory (order=%u requested)\n", order);
    }
    return ret;
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

    if (order == 0 && g_pmm_percpu_ready) {
        /* A page parked in a per-CPU cache stays marked allocated in the
         * bitmap: it is owned by this core, not free, and coalescing must keep
         * treating it as in use. Freeing the same page twice therefore lands it
         * in the cache twice and is caught later, when the cache spills and the
         * second copy reaches zone_free_locked() — deferred, but not lost. */
        irqflags_t f = irq_save();
        pcp_free(smp_current_cpu_id(), phys);
        irq_restore(f);
        return;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
    zone_free_locked(phys, order);
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
    phys_addr_t ret = 0;
    if (unlikely(order > PMM_MAX_ORDER)) return 0;

    if (order == 0 && g_pmm_percpu_ready) {
        irqflags_t f = irq_save();
        ret = pcp_alloc(smp_current_cpu_id(), PMM_ZONE_DMA32);
        irq_restore(f);
    }

    if (!ret) {
        irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
        ret = zone_alloc_locked(&g_zone[PMM_ZONE_DMA32], order);
        spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        
        if (!ret && g_pmm_percpu_ready) {
            pmm_drain_local();
            flags = spinlock_lock_irqsave(&g_pmm_lock);
            ret = zone_alloc_locked(&g_zone[PMM_ZONE_DMA32], order);
            spinlock_unlock_irqrestore(&g_pmm_lock, flags);
        }
    }

    if (likely(ret)) {
        u64 frames = 1ULL << order;
        for (u64 i = 0; i < frames; i++) {
            hw_clear_page((void *)PHYS_TO_VIRT(ret + i * PAGE_SIZE));
        }
    } else {
        pr_debug("[PMM] Out of 32-bit low memory (order=%u requested)\n", order);
    }
    return ret;
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

    /* Region larger than one buddy block: free it as a run of naturally
     * aligned power-of-two blocks so nothing leaks. */
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

/* BUG-AC fix: the zone counters are written under g_pmm_lock but read here
 * without any lock.  On x86-64 a 64-bit aligned load is atomic at the hardware
 * level, but the C standard still considers an un-annotated plain load a data
 * race (UB).  __ATOMIC_RELAXED costs nothing on x86 and makes the intent
 * explicit.
 *
 * Frames parked in the per-CPU caches have left free_frames but are still
 * free memory from every caller's point of view — the page-fault handler's
 * low-memory check reads this — so they are added back in. */
u64 pmm_get_free_pages(void)
{
    u64 n = 0;
    for (u32 zi = 0; zi < PMM_ZONE_COUNT; zi++)
        n += __atomic_load_n(&g_zone[zi].free_frames, __ATOMIC_RELAXED)
           + pcp_cached(zi);
    return n;
}

u64 pmm_get_total_pages(void)
{
    u64 n = 0;
    for (u32 zi = 0; zi < PMM_ZONE_COUNT; zi++)
        n += __atomic_load_n(&g_zone[zi].total_frames, __ATOMIC_RELAXED);
    return n;
}

int pmm_zone_stats(pmm_zone_stat_t *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = (max < PMM_ZONE_COUNT) ? max : PMM_ZONE_COUNT;

    irqflags_t flags = spinlock_lock_irqsave(&g_pmm_lock);
    for (int zi = 0; zi < n; zi++) {
        pmm_zone_t *z = &g_zone[zi];
        out[zi].name         = z->name;
        out[zi].total_pages  = z->total_frames;
        out[zi].free_pages   = z->free_frames;
        for (u32 o = 0; o < PMM_ORDER_COUNT; o++)
            out[zi].blocks[o] = z->blocks[o];
    }
    spinlock_unlock_irqrestore(&g_pmm_lock, flags);

    /* Outside the lock: pcp_cached() walks per-CPU rows this lock does not
     * protect anyway, and holding a global lock across SMP_MAX_CPUS loads
     * would serialise every allocator on a statistics read. */
    for (int zi = 0; zi < n; zi++)
        out[zi].cached_pages = pcp_cached((u32)zi);

    return n;
}

void pmm_dump_stats(void)
{
    pmm_zone_stat_t st[PMM_ZONE_COUNT];
    int n = pmm_zone_stats(st, PMM_ZONE_COUNT);

    kprintf("[PMM] Free: %llu pages (%llu MB)  Total: %llu pages (%llu MB)\n",
            (unsigned long long)pmm_get_free_pages(),
            (unsigned long long)(pmm_get_free_pages() * PAGE_SIZE / (1024*1024)),
            (unsigned long long)pmm_get_total_pages(),
            (unsigned long long)(pmm_get_total_pages() * PAGE_SIZE / (1024*1024)));

    for (int zi = 0; zi < n; zi++) {
        kprintf("  zone %-6s  free %llu pages  cached %llu pages  total %llu pages\n",
                st[zi].name,
                (unsigned long long)st[zi].free_pages,
                (unsigned long long)st[zi].cached_pages,
                (unsigned long long)st[zi].total_pages);
        for (u32 o = 0; o < PMM_ORDER_COUNT; o++) {
            if (!st[zi].blocks[o]) continue;
            u64 kb = ((u64)PAGE_SIZE << o) / 1024;
            kprintf("    order %2u (%7llu KB): %llu blocks\n",
                    o, (unsigned long long)kb,
                    (unsigned long long)st[zi].blocks[o]);
        }
    }
}
