/* ============================================================================
 * AzamiOS — Physical Memory Manager (Buddy Allocator)
 * File: kernel/mm/pmm.h
 *
 * API summary:
 *   pmm_init(memmap)       — called once from kernel_main with Limine memmap
 *   pmm_alloc(order)       → phys_addr_t  (allocates 2^order 4 KB pages)
 *   pmm_free(phys, order)  — returns frames to the free list
 *   pmm_alloc_page()       → phys_addr_t  (convenience: order 0, 1 page)
 *   pmm_free_page(phys)    — convenience: order 0
 *
 * Order range: 0 (4 KB) through PMM_MAX_ORDER (4 MB = 2^10 × 4 KB).
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

/* Maximum buddy order. Order N = 2^N pages = 2^N × 4 KB. */
#define PMM_MAX_ORDER   10          /* order 10 = 4 MB block */
#define PMM_ORDER_COUNT (PMM_MAX_ORDER + 1)

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
 * pmm_alloc(order) → physical address of the allocated block, or 0 on failure.
 *
 * Allocates a naturally aligned, contiguous physical memory block of
 * (1 << order) × PAGE_SIZE bytes.
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
