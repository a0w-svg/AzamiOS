/* ============================================================================
 * AzamiOS — Per-process VMA registry implementation
 * File: kernel/mm/vma.c
 * ============================================================================ */

#include "vma.h"
#include "kmalloc.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../include/azami/defs.h"

/* One coarse lock for all processes' lists. Held only for short list surgery,
 * never across a blocking call. */
static spinlock_t g_vma_lock = SPINLOCK_INIT;

static vm_area_t **vma_head(struct process *p)
{
    return (vm_area_t **)&p->vma_list;
}

static vm_area_t *vma_new(u64 start, u64 end, u32 prot, u32 flags)
{
    vm_area_t *v = (vm_area_t *)kmalloc(sizeof(*v));
    if (!v) return NULL;
    v->start = start;
    v->end   = end;
    v->prot  = prot;
    v->flags = flags;
    v->next  = NULL;
    return v;
}

/* Insert `v` into the sorted list; caller holds g_vma_lock. */
static void vma_link_sorted(vm_area_t **head, vm_area_t *v)
{
    vm_area_t **pp = head;
    while (*pp && (*pp)->start < v->start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
}

/* Merge consecutive regions that are contiguous and identical in prot/flags. */
static void vma_coalesce(vm_area_t **head)
{
    for (vm_area_t *v = *head; v && v->next; ) {
        vm_area_t *n = v->next;
        if (v->end == n->start && v->prot == n->prot && v->flags == n->flags) {
            v->end = n->end;
            v->next = n->next;
            kfree(n);
        } else {
            v = v->next;
        }
    }
}

/* Excise [start,end) from the list. Caller holds g_vma_lock. */
static void vma_remove_locked(vm_area_t **head, u64 start, u64 end)
{
    vm_area_t **pp = head;
    while (*pp) {
        vm_area_t *v = *pp;
        if (v->end <= start || v->start >= end) {   /* no overlap */
            pp = &v->next;
            continue;
        }
        if (v->start >= start && v->end <= end) {    /* fully covered → drop */
            *pp = v->next;
            kfree(v);
            continue;
        }
        if (v->start < start && v->end > end) {       /* split into two */
            vm_area_t *tail = vma_new(end, v->end, v->prot, v->flags);
            if (tail) {
                tail->next = v->next;
                v->next = tail;
                v->end = start;
                pp = &tail->next;
            } else {
                pp = &v->next;
            }
            continue;
        }
        if (v->start < start) {                        /* trim tail */
            v->end = start;
        } else {                                      /* trim head */
            v->start = end;
        }
        pp = &v->next;
    }
}

int vma_add(struct process *p, u64 start, u64 end, u32 prot, u32 flags)
{
    if (!p || end <= start) return 0;
    vm_area_t *v = vma_new(start, end, prot, flags);
    if (!v) return -ENOMEM;

    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t **head = vma_head(p);
    vma_remove_locked(head, start, end);
    vma_link_sorted(head, v);
    vma_coalesce(head);
    spinlock_unlock_irqrestore(&g_vma_lock, f);
    return 0;
}

void vma_remove(struct process *p, u64 start, u64 end)
{
    if (!p || end <= start) return;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vma_remove_locked(vma_head(p), start, end);
    spinlock_unlock_irqrestore(&g_vma_lock, f);
}

/*
 * Apply @prot to [start,end).
 *
 * The two spare nodes are allocated up front, before the lock, and that is the
 * whole point of the shape of this function. Splitting a region that only
 * partly overlaps the range needs a new node, and the old code applied the new
 * prot to the *entire* region when that allocation failed. The VMA prot is not
 * bookkeeping: the #PF handler reads it back through vma_probe() and maps each
 * demand-paged frame with exactly those rights. So an mprotect(PROT_EXEC) over
 * a few pages of a large PROT_READ|PROT_WRITE region, made to fail its
 * allocation, silently marked the whole region executable and every later
 * fault in it produced a page mapped without NX — a W^X bypass reachable from
 * ring 3 under memory pressure.
 *
 * At most two nodes are ever needed no matter how many regions the range
 * spans: only the first and last can be partially covered, everything between
 * them is fully inside [start,end). Taking them before the lock means the
 * operation either applies in full or does not begin.
 *
 * Returns 0, or -ENOMEM with the registry untouched.
 */
int vma_setprot(struct process *p, u64 start, u64 end, u32 prot)
{
    if (!p || end <= start) return 0;

    vm_area_t *spare_a = vma_new(0, 1, 0, 0);
    vm_area_t *spare_b = vma_new(0, 1, 0, 0);
    if (!spare_a || !spare_b) {
        kfree(spare_a);
        kfree(spare_b);
        return -ENOMEM;
    }

    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t **head = vma_head(p);

    for (vm_area_t *v = *head; v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;

        /* Trim a leading slice that keeps its old prot. */
        if (v->start < start && spare_a) {
            vm_area_t *mid = spare_a;
            spare_a = NULL;
            mid->start = start; mid->end = v->end;
            mid->prot = v->prot; mid->flags = v->flags;
            v->end = start;
            mid->next = v->next;
            v->next = mid;
            v = mid;
        }
        /* Trim a trailing slice that keeps its old prot. */
        if (v->end > end && spare_b) {
            vm_area_t *tail = spare_b;
            spare_b = NULL;
            tail->start = end; tail->end = v->end;
            tail->prot = v->prot; tail->flags = v->flags;
            tail->next = v->next;
            v->next = tail;
            v->end = end;
        }
        v->prot = prot;
    }
    vma_coalesce(head);
    spinlock_unlock_irqrestore(&g_vma_lock, f);

    kfree(spare_a);
    kfree(spare_b);
    return 0;
}

/* Set or clear VMA_F_LOCKED over [start,end), splitting at the edges the
 * same way vma_setprot() does — but for `flags` instead of `prot`.
 *
 * Real mlock(2) rejects a range with any unmapped gap (-ENOMEM). This
 * kernel can't replicate that: per this file's own header comment, the VMA
 * list is a best-effort index, not an authoritative map — brk()-managed
 * heap growth (kernel/syscall/syscall.c's sys_brk_impl) maps pages directly
 * via vmm_map() and never calls vma_add() at all, so "no VMA covers this
 * range" is true of plenty of genuinely-valid memory, not just garbage
 * addresses. Rather than reject real heap/stack ranges as a false
 * positive, this only ever touches whatever VMAs *do* overlap the range —
 * a range with no VMA at all is a harmless no-op, consistent with
 * VMA_F_LOCKED being bookkeeping with nothing behind it to actually pin
 * anyway (see vma.h's comment on the flag). */
int vma_set_locked(struct process *p, u64 start, u64 end, bool locked)
{
    if (!p || end <= start) return 0;

    vm_area_t *spare_a = vma_new(0, 1, 0, 0);
    vm_area_t *spare_b = vma_new(0, 1, 0, 0);
    if (!spare_a || !spare_b) {
        kfree(spare_a);
        kfree(spare_b);
        return -ENOMEM;
    }

    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t **head = vma_head(p);

    for (vm_area_t *v = *head; v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;

        if (v->start < start && spare_a) {
            vm_area_t *mid = spare_a;
            spare_a = NULL;
            mid->start = start; mid->end = v->end;
            mid->prot = v->prot; mid->flags = v->flags;
            v->end = start;
            mid->next = v->next;
            v->next = mid;
            v = mid;
        }
        if (v->end > end && spare_b) {
            vm_area_t *tail = spare_b;
            spare_b = NULL;
            tail->start = end; tail->end = v->end;
            tail->prot = v->prot; tail->flags = v->flags;
            tail->next = v->next;
            v->next = tail;
            v->end = end;
        }
        if (locked) v->flags |= VMA_F_LOCKED;
        else        v->flags &= ~(u32)VMA_F_LOCKED;
    }
    vma_coalesce(head);
    spinlock_unlock_irqrestore(&g_vma_lock, f);

    kfree(spare_a);
    kfree(spare_b);
    return 0;
}

/* Set or clear VMA_F_LOCKED on every current VMA (mlockall(2)/munlockall(2)).
 * Unlike vma_set_locked() above there is no "gap" to fail on — by
 * definition every VMA in the list is covered. */
void vma_set_locked_all(struct process *p, bool locked)
{
    if (!p) return;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        if (locked) v->flags |= VMA_F_LOCKED;
        else        v->flags &= ~(u32)VMA_F_LOCKED;
    }
    vma_coalesce(vma_head(p));
    spinlock_unlock_irqrestore(&g_vma_lock, f);
}

bool vma_probe(struct process *p, u64 addr, u32 *out_prot)
{
    if (!p) return false;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    bool hit = false;
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        if (addr >= v->start && addr < v->end) {
            if (out_prot) *out_prot = v->prot;
            hit = true;
            break;
        }
        if (v->start > addr) break;
    }
    spinlock_unlock_irqrestore(&g_vma_lock, f);
    return hit;
}

void vma_reset(struct process *p)
{
    if (!p) return;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t *v = *vma_head(p);
    *vma_head(p) = NULL;
    spinlock_unlock_irqrestore(&g_vma_lock, f);
    while (v) {
        vm_area_t *n = v->next;
        kfree(v);
        v = n;
    }
}

int vma_clone(struct process *dst, struct process *src)
{
    if (!dst || !src) return 0;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t **dhead = vma_head(dst);
    vm_area_t  *tail  = NULL;
    for (vm_area_t *v = *vma_head(src); v; v = v->next) {
        vm_area_t *c = vma_new(v->start, v->end, v->prot, v->flags);
        if (!c) {
            /* BUG-Y fix: free the already-cloned nodes while still holding the
             * lock so no other CPU can observe the partial dst VMA list between
             * our unlock and a subsequent vma_reset(dst) re-lock.
             * vma_reset() would also re-acquire g_vma_lock (recursive), so it
             * cannot be called while we hold it. */
            vm_area_t *n = *dhead;
            *dhead = NULL;
            spinlock_unlock_irqrestore(&g_vma_lock, f);
            while (n) { vm_area_t *nx = n->next; kfree(n); n = nx; }
            return -ENOMEM;
        }
        if (tail) tail->next = c; else *dhead = c;
        tail = c;
    }
    spinlock_unlock_irqrestore(&g_vma_lock, f);
    return 0;

}

void vma_for_each(struct process *p, void (*fn)(const vm_area_t *, void *), void *ctx)
{
    if (!p || !fn) return;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    for (vm_area_t *v = *vma_head(p); v; v = v->next) fn(v, ctx);
    spinlock_unlock_irqrestore(&g_vma_lock, f);
}
