/* ============================================================================
 * AzamiOS — Physical Memory Manager (Zoned Buddy Allocator + per-CPU caches)
 * File: kernel/mm/pmm.h
 *
 * API summary:
 *   pmm_init(memmap)       — called once from kernel_main with Limine memmap
 *   pmm_enable_percpu()    — called once after smp_init(); arms the per-CPU
 *                            page caches that make order-0 alloc/free lockless
 *   pmm_alloc(order)       → phys_addr_t  (allocates 2^order 4 KB pages)
 *   pmm_free(phys, order)  — returns frames to the free list
 *   pmm_alloc_page()       → phys_addr_t  (convenience: order 0, 1 page)
 *   pmm_free_page(phys)    — convenience: order 0
 *
 * Order range: 0 (4 KB) through PMM_MAX_ORDER (1 GB = 2^18 × 4 KB), so every
 * page size x86-64 paging can express — 4 KB, 2 MB and 1 GB — is a single
 * naturally aligned buddy block.
 *
 * Zones: physical memory is split at the 4 GB line into ZONE_DMA32 (below,
 * reachable by a 32-bit bus address) and ZONE_NORMAL (at or above). Each zone
 * carries its own free lists, so pmm_alloc_32() is the same O(1) lookup as
 * pmm_alloc() rather than a search, and a general allocation prefers
 * ZONE_NORMAL so it does not consume memory only devices can use.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

/* Maximum buddy order. Order N = 2^N pages = 2^N × 4 KB.
 *
 * 18 is not arbitrary: 2^18 pages is exactly 1 GB, the largest page x86-64
 * paging can map with a single PDPT entry. Capping lower (this allocator used
 * to stop at order 10 = 4 MB) means no caller can ever obtain a 1 GB-aligned
 * 1 GB frame, so vmm_clone_address_space()'s 1 GB huge-page copy could only
 * ever fail. Blocks are naturally aligned, so an order-18 block is 1 GB-aligned
 * for free — exactly what a PDPT huge entry requires. */
#define PMM_MAX_ORDER   18          /* order 18 = 1 GB block */
#define PMM_ORDER_COUNT (PMM_MAX_ORDER + 1)

/* Memory zones. The boundary is the 4 GB line, which every block size up to
 * PMM_MAX_ORDER divides evenly — so a naturally aligned buddy block is always
 * wholly inside one zone and a block's buddy is always in the same zone. */
#define PMM_ZONE_DMA32   0          /* phys <  4 GB — 32-bit device reachable */
#define PMM_ZONE_NORMAL  1          /* phys >= 4 GB                           */
#define PMM_ZONE_COUNT   2

/**
 * pmm_init() — Initialise the physical memory manager.
 *
 * Reads the Limine memory map to discover usable physical memory ranges,
 * then populates the buddy free lists. Must be called before any pmm_alloc().
 *
 * @memmap   Pointer to the Limine memory-map response structure.
 *           (Typed as void* to avoid pulling limine.h into every consumer.)
 */
void pmm_init(void *memmap);

/**
 * pmm_enable_percpu() — Arm the per-CPU page caches.
 *
 * Call once from the boot path after smp_init() has published a valid GS base
 * on every core. Until then every allocation goes straight to the zone free
 * lists under the global lock, which is correct (the BSP is single-threaded at
 * that point) but serialising.
 */
void pmm_enable_percpu(void);

/**
 * pmm_drain_local() — Return the calling CPU's cached pages to the buddy.
 *
 * Safe only on the current core: a remote core's cache is mutated lock-free by
 * its owner. Called automatically before an allocation is declared OOM, so the
 * pages this CPU is holding can coalesce instead of being lost to a caller
 * that needs a high order.
 */
void pmm_drain_local(void);

/**
 * pmm_alloc(order) → physical address of the allocated block, or 0 on failure.
 *
 * Allocates a naturally aligned, contiguous physical memory block of
 * (1 << order) × PAGE_SIZE bytes. Prefers ZONE_NORMAL and falls back to
 * ZONE_DMA32.
 *
 * @order   0–PMM_MAX_ORDER.
 */
phys_addr_t pmm_alloc(u32 order);

/**
 * pmm_free(phys, order) — Return a block to the buddy free list.
 *
 * @phys    Physical address of the block (must be naturally aligned to the
 *          block's size: phys % (PAGE_SIZE << order) == 0).
 * @order   Must match the order passed to pmm_alloc().
 */
void pmm_free(phys_addr_t phys, u32 order);

/** Convenience wrappers for single-page allocation (order 0). */
static inline phys_addr_t pmm_alloc_page(void) { return pmm_alloc(0); }
static inline void        pmm_free_page(phys_addr_t p) { pmm_free(p, 0); }
phys_addr_t               pmm_alloc_page_zeroed(void);

/** Multi-page allocation helper (calculates required order automatically). */
phys_addr_t pmm_alloc_pages(size_t page_count);
void pmm_free_pages(phys_addr_t phys, size_t page_count);

/** 32-bit low memory allocation helpers for legacy PCI DMA devices. */
phys_addr_t pmm_alloc_32(u32 order);
phys_addr_t pmm_alloc_pages_32(size_t page_count);

/** Statistics */
u64 pmm_get_free_pages(void);
u64 pmm_get_total_pages(void);
void pmm_dump_stats(void);

/** Per-zone snapshot, one entry per zone. Backs /proc/buddyinfo. */
typedef struct {
    const char *name;                    /* "DMA32" / "Normal"               */
    u64         total_pages;             /* frames the zone was born with    */
    u64         free_pages;              /* frames on its free lists         */
    u64         cached_pages;            /* frames parked in per-CPU caches  */
    u64         blocks[PMM_ORDER_COUNT]; /* free block count per order       */
} pmm_zone_stat_t;

/** pmm_zone_stats(out, max) — fill up to @max zone entries, returns the number
 *  written. A zone with no memory at all is still reported, matching Linux's
 *  /proc/buddyinfo which lists every configured zone. */
int pmm_zone_stats(pmm_zone_stat_t *out, int max);
