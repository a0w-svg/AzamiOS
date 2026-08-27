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
            v->end = start;
            if (tail) { tail->next = v->next; v->next = tail; }
            pp = &v->next;
            if (tail) pp = &tail->next;
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

void vma_setprot(struct process *p, u64 start, u64 end, u32 prot)
{
    if (!p || end <= start) return;
    irqflags_t f = spinlock_lock_irqsave(&g_vma_lock);
    vm_area_t **head = vma_head(p);

    for (vm_area_t *v = *head; v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;

        /* Trim a leading slice that keeps its old prot. */
        if (v->start < start) {
            vm_area_t *mid = vma_new(start, v->end, v->prot, v->flags);
            v->end = start;
            if (mid) { mid->next = v->next; v->next = mid; v = mid; }
        }
        /* Trim a trailing slice that keeps its old prot. */
        if (v->end > end) {
            vm_area_t *tail = vma_new(end, v->end, v->prot, v->flags);
            v->end = end;
            if (tail) { tail->next = v->next; v->next = tail; }
        }
        v->prot = prot;
    }
    vma_coalesce(head);
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
        if (!c) { spinlock_unlock_irqrestore(&g_vma_lock, f); vma_reset(dst); return -ENOMEM; }
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
