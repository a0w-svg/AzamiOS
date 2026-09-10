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
/* Set by fork() on copy-on-write regions: a write fault here must COW-break
 * before the write is allowed (mirrors the VMM_F_COW bit in the leaf PTE). */
#define VMA_F_COW       0x10
/* Set by mmap(MAP_NORESERVE): physical frames are not allocated until touched.
 * The #PF handler allocates on first access; vmm_map() is never called at
 * mmap time. Incompatible with VMA_F_FILE (file-backed demand is separate). */
#define VMA_F_DEMAND    0x20
/* Set by mlock(2)/mlockall(2). This kernel has no page reclaim or swap at
 * all (nothing ever evicts a resident page), so there is nothing for
 * "locked" to actually pin against — this bit is honest bookkeeping only:
 * mlock()/munlock()/mlockall()/munlockall() genuinely record and report
 * which mappings the caller asked to be locked (readable back via
 * /proc/<pid>/maps or an mincore()-style query), rather than silently
 * discarding the request the way this kernel used to. */
#define VMA_F_LOCKED    0x40

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
/* Returns 0, or -ENOMEM leaving the registry unchanged — never a partial or
 * over-wide application, since the #PF handler grants demand-paged frames the
 * rights this records. */
int vma_setprot(struct process *p, u64 start, u64 end, u32 prot);

/** Set or clear VMA_F_LOCKED over [start,end) (mlock(2)/munlock(2)/mlock2(2)).
 *  Returns 0, or -ENOMEM only if a split allocation fails. Touches whatever
 *  VMAs overlap the range and leaves the rest alone — this kernel's VMA list
 *  isn't an authoritative map (see this header's own comment), so a range
 *  with no VMA at all (e.g. brk()-managed heap, which never gets one) is a
 *  harmless no-op rather than a rejected call. Bookkeeping only: see
 *  VMA_F_LOCKED's comment. */
int vma_set_locked(struct process *p, u64 start, u64 end, bool locked);

/** Set or clear VMA_F_LOCKED on every current VMA (mlockall(2)/munlockall(2)). */
void vma_set_locked_all(struct process *p, bool locked);

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
