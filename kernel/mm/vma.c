/* ============================================================================
 * AzamiOS — Per-process VMA registry implementation
 * File: kernel/mm/vma.c
 * ============================================================================ */

#include "vma.h"
#include "kmalloc.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"

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

static void vma_rb_insert(struct process *p, vm_area_t *v)
{
    struct rb_root *root = &p->vma_tree;
    struct rb_node **new = &root->rb_node, *parent = NULL;
    
    while (*new) {
        vm_area_t *this = rb_entry(*new, vm_area_t, rb);
        parent = *new;
        if (v->start < this->start)
            new = &(*new)->rb_left;
        else
            new = &(*new)->rb_right;
    }
    
    rb_link_node(&v->rb, parent, new);
    rb_insert_color(&v->rb, root);
}

static void vma_link(struct process *p, vm_area_t *v)
{
    vm_area_t **pp = vma_head(p);
    while (*pp && (*pp)->start < v->start) pp = &(*pp)->next;
    v->next = *pp;
    *pp = v;
    vma_rb_insert(p, v);
}

static void vmacache_invalidate(struct process *p)
{
    p->vmacache_seqnum++;
}

/* O(1) / O(log N) merging of consecutive regions. */
static void vma_coalesce_range(struct process *p, u64 start, u64 end)
{
    struct rb_node *node = p->vma_tree.rb_node;
    vm_area_t *first = NULL;
    while (node) {
        vm_area_t *v = rb_entry(node, vm_area_t, rb);
        if (v->end <= start) {
            node = node->rb_right;
        } else if (v->start > start) {
            first = v;
            node = node->rb_left;
        } else {
            first = v;
            break;
        }
    }
    
    if (!first) {
        first = *vma_head(p);
    } else {
        struct rb_node *prev_node = rb_prev(&first->rb);
        if (prev_node) {
            first = rb_entry(prev_node, vm_area_t, rb);
        }
    }

    for (vm_area_t *v = first; v && v->end <= end && v->next; ) {
        vm_area_t *n = v->next;
        if (v->end == n->start && v->prot == n->prot && v->flags == n->flags) {
            v->end = n->end;
            v->next = n->next;
            rb_erase(&n->rb, &p->vma_tree);
            kfree(n);
        } else {
            v = v->next;
        }
    }
}

static void vma_remove_locked(struct process *p, u64 start, u64 end)
{
    vm_area_t **pp = vma_head(p);
    while (*pp) {
        vm_area_t *v = *pp;
        if (v->end <= start || v->start >= end) {
            pp = &v->next;
            continue;
        }
        if (v->start >= start && v->end <= end) {
            *pp = v->next;
            rb_erase(&v->rb, &p->vma_tree);
            kfree(v);
            continue;
        }
        if (v->start < start && v->end > end) {
            vm_area_t *tail = vma_new(end, v->end, v->prot, v->flags);
            if (tail) {
                tail->next = v->next;
                v->next = tail;
                v->end = start;
                vma_rb_insert(p, tail);
                pp = &tail->next;
            } else {
                pp = &v->next;
            }
            continue;
        }
        if (v->start < start) {
            v->end = start;
        } else {
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

    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    vma_remove_locked(p, start, end);
    vma_link(p, v);
    vma_coalesce_range(p, start, end);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);
    return 0;
}

void vma_remove(struct process *p, u64 start, u64 end)
{
    if (!p || end <= start) return;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    vma_remove_locked(p, start, end);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);
}

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

    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
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
            vma_rb_insert(p, mid);
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
            vma_rb_insert(p, tail);
        }
        v->prot = prot;
    }
    vma_coalesce_range(p, start, end);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);

    kfree(spare_a);
    kfree(spare_b);
    return 0;
}

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

    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
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
            vma_rb_insert(p, mid);
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
            vma_rb_insert(p, tail);
        }
        if (locked) v->flags |= VMA_F_LOCKED;
        else        v->flags &= ~(u32)VMA_F_LOCKED;
    }
    vma_coalesce_range(p, start, end);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);

    kfree(spare_a);
    kfree(spare_b);
    return 0;
}

void vma_set_locked_all(struct process *p, bool locked)
{
    if (!p) return;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        if (locked) v->flags |= VMA_F_LOCKED;
        else        v->flags &= ~(u32)VMA_F_LOCKED;
    }
    vma_coalesce_range(p, 0, ~0ULL);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);
}

bool vma_probe(struct process *p, u64 addr, u32 *out_prot)
{
    if (!p) return false;

    struct thread *t = sched_current_thread();
    if (t && t->vmacache_seqnum != p->vmacache_seqnum) {
        for (int i = 0; i < 4; i++) t->vmacache[i] = NULL;
        t->vmacache_seqnum = p->vmacache_seqnum;
    }
    
    if (t) {
        for (int i = 0; i < 4; i++) {
            vm_area_t *cv = t->vmacache[i];
            if (cv && addr >= cv->start && addr < cv->end) {
                if (out_prot) *out_prot = cv->prot;
                return true;
            }
        }
    }

    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    bool hit = false;
    vm_area_t *found = NULL;
    
    struct rb_node *node = p->vma_tree.rb_node;
    while (node) {
        vm_area_t *v = rb_entry(node, vm_area_t, rb);
        if (addr < v->start) {
            node = node->rb_left;
        } else if (addr >= v->end) {
            node = node->rb_right;
        } else {
            if (out_prot) *out_prot = v->prot;
            found = v;
            hit = true;
            break;
        }
    }
    
    spinlock_unlock_irqrestore(&p->vma_lock, f);

    if (hit && t) {
        t->vmacache[3] = t->vmacache[2];
        t->vmacache[2] = t->vmacache[1];
        t->vmacache[1] = t->vmacache[0];
        t->vmacache[0] = found;
    }

    return hit;
}

void vma_reset(struct process *p)
{
    if (!p) return;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    vm_area_t *v = *vma_head(p);
    *vma_head(p) = NULL;
    p->vma_tree.rb_node = NULL;
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);
    while (v) {
        vm_area_t *n = v->next;
        kfree(v);
        v = n;
    }
}

int vma_clone(struct process *dst, struct process *src)
{
    if (!dst || !src) return 0;
    irqflags_t f = spinlock_lock_irqsave(&src->vma_lock);
    vm_area_t **dhead = vma_head(dst);
    vm_area_t  *tail  = NULL;
    
    dst->vma_tree.rb_node = NULL;
    vmacache_invalidate(dst);

    for (vm_area_t *v = *vma_head(src); v; v = v->next) {
        vm_area_t *c = vma_new(v->start, v->end, v->prot, v->flags);
        if (!c) {
            vm_area_t *n = *dhead;
            *dhead = NULL;
            dst->vma_tree.rb_node = NULL;
            spinlock_unlock_irqrestore(&src->vma_lock, f);
            while (n) { vm_area_t *nx = n->next; kfree(n); n = nx; }
            return -ENOMEM;
        }
        if (tail) tail->next = c; else *dhead = c;
        tail = c;
        vma_rb_insert(dst, c);
    }
    spinlock_unlock_irqrestore(&src->vma_lock, f);
    return 0;
}

void vma_for_each(struct process *p, void (*fn)(const vm_area_t *, void *), void *ctx)
{
    if (!p || !fn) return;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        fn(v, ctx);
    }
    spinlock_unlock_irqrestore(&p->vma_lock, f);
}

bool vma_range_has_flags(struct process *p, u64 start, u64 end, u32 flags)
{
    if (!p || end <= start) return false;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    bool hit = false;
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;
        if (v->flags & flags) {
            hit = true;
            break;
        }
    }
    spinlock_unlock_irqrestore(&p->vma_lock, f);
    return hit;
}

bool vma_is_sealed(struct process *p, u64 start, u64 end)
{
    if (!p || end <= start) return false;
    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
    bool sealed = false;
    for (vm_area_t *v = *vma_head(p); v; v = v->next) {
        if (v->end <= start || v->start >= end) continue;
        if (v->flags & VMA_F_SEALED) {
            sealed = true;
            break;
        }
    }
    spinlock_unlock_irqrestore(&p->vma_lock, f);
    return sealed;
}

int vma_seal(struct process *p, u64 start, u64 end)
{
    if (!p || end <= start) return 0;

    vm_area_t *spare_a = vma_new(0, 1, 0, 0);
    vm_area_t *spare_b = vma_new(0, 1, 0, 0);
    if (!spare_a || !spare_b) {
        kfree(spare_a);
        kfree(spare_b);
        return -ENOMEM;
    }

    irqflags_t f = spinlock_lock_irqsave(&p->vma_lock);
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
            vma_rb_insert(p, mid);
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
            vma_rb_insert(p, tail);
        }
        v->flags |= VMA_F_SEALED;
    }
    vma_coalesce_range(p, start, end);
    vmacache_invalidate(p);
    spinlock_unlock_irqrestore(&p->vma_lock, f);

    kfree(spare_a);
    kfree(spare_b);
    return 0;
}
