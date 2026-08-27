/* ============================================================================
 * AzamiOS — Per-process Virtual Memory Area (VMA) registry
 * File: kernel/mm/vma.h
 *
 * Bookkeeping layer over the eagerly-populated page tables: records every
 * mmap()/mprotect() region so the page-fault handler can validate faults
 * against real regions and /proc/<pid>/maps can be rendered accurately.
 *
 * The page tables remain the mechanism — the VMA list never has to be perfectly
 * in sync for memory safety; a stale/missing entry only degrades the fault
 * handler to "SIGSEGV" and makes /proc/maps slightly wrong.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

struct process;

#define VMA_PROT_READ   0x1
#define VMA_PROT_WRITE  0x2
#define VMA_PROT_EXEC   0x4

#define VMA_F_ANON      0x01
#define VMA_F_SHARED    0x02
#define VMA_F_STACK     0x04
#define VMA_F_FILE      0x08

typedef struct vm_area {
    u64             start;   /* page-aligned, inclusive */
    u64             end;     /* page-aligned, exclusive */
    u32             prot;    /* VMA_PROT_* */
    u32             flags;   /* VMA_F_*    */
    struct vm_area *next;    /* sorted ascending by start */
} vm_area_t;

/** Record region [start,end); merges into an adjacent identical region.
 *  Any pre-existing overlap is removed first. Returns 0 or -ENOMEM. */
int  vma_add(struct process *p, u64 start, u64 end, u32 prot, u32 flags);

/** Remove [start,end) from the region set, splitting/trimming as needed. */
void vma_remove(struct process *p, u64 start, u64 end);

/** Change protection bits over [start,end), splitting regions at the edges. */
void vma_setprot(struct process *p, u64 start, u64 end, u32 prot);

/** Probe the region containing addr. If found, stores its VMA_PROT_* bits in
 *  *out_prot (when non-NULL) and returns true. The lookup and the copy happen
 *  under the VMA lock, so — unlike handing back a vm_area_t* — the result cannot
 *  race with a concurrent munmap()/mprotect() freeing the node. */
bool vma_probe(struct process *p, u64 addr, u32 *out_prot);

/** Free every region (exec/exit). */
void vma_reset(struct process *p);

/** Deep-copy src's region list into dst (fork). Returns 0 or -ENOMEM. */
int  vma_clone(struct process *dst, struct process *src);

/** Run `fn(area, ctx)` for each region in ascending order, under the VMA lock. */
void vma_for_each(struct process *p, void (*fn)(const vm_area_t *, void *), void *ctx);
