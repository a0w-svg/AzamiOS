/* ============================================================================
 * AzamiOS — Kernel Memory Allocator (kmalloc / kfree) Header
 * File: kernel/mm/kmalloc.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** kmalloc_init() — Initialize the kernel slab/bucket allocator. */
void kmalloc_init(void);

/** kmalloc(size) — Allocate `size` bytes of kernel memory. */
void *kmalloc(size_t size);

/** kzalloc(size) — Allocate and zero `size` bytes of kernel memory. */
void *kzalloc(size_t size);

/** kcalloc(nmemb, size) — Allocate and zero an array, rejecting nmemb*size
 *  overflow. Returns NULL on overflow or OOM. */
void *kcalloc(size_t nmemb, size_t size);

/** ksize(ptr) — Usable byte capacity of an allocation (>= the requested size),
 *  or 0 for NULL. Lets growable buffers skip a krealloc() when slack remains. */
size_t ksize(const void *ptr);

/** krealloc(ptr, new_size) — Reallocate memory block to `new_size`. */
void *krealloc(void *ptr, size_t new_size);

/** kfree(ptr) — Free kernel memory previously allocated by kmalloc/kzalloc. */
void kfree(void *ptr);

/** kmalloc_reclaim() — Return every fully-free slab page to the PMM.
 *  Returns the number of pages handed back. Safe to call any time; the
 *  reaper thread calls it periodically. */
size_t kmalloc_reclaim(void);

/** kmalloc_start_reaper() — Spawn the kernel thread that calls
 *  kmalloc_reclaim() on a timer. Call once, after the scheduler is up. */
void kmalloc_start_reaper(void);

/** kmalloc_meminfo(slab_kb, large_kb) — Kernel heap footprint in KiB:
 *  bucket/slab pages and above-bucket ("large") pages. Either pointer may be
 *  NULL. Backs the Slab lines in /proc/meminfo. */
void kmalloc_meminfo(u64 *slab_kb, u64 *large_kb);

/** Per-bucket slab statistics, one entry per power-of-two size class. */
typedef struct {
    u32 obj_size;       /* usable bytes per object (block minus header) */
    u32 objs_per_slab;  /* objects carved from one page */
    u64 pages;          /* pages this bucket currently owns */
    u64 total_objs;     /* pages * objs_per_slab */
    u64 active_objs;    /* total minus what is on the free list */
} kmalloc_slab_stat_t;

/** kmalloc_slab_stats(out, max) — fill up to `max` bucket entries, returns the
 *  number written. Backs /proc/slabinfo. */
int kmalloc_slab_stats(kmalloc_slab_stat_t *out, int max);
