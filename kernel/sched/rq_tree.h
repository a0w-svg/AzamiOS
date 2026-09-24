/* ============================================================================
 * AzamiOS — CFS Run-Queue Red-Black Tree
 * File: kernel/sched/rq_tree.h
 *
 * The per-CPU ready queue, ordered by CFS virtual runtime.
 *
 * It used to be a singly-linked list kept in sorted order. That made picking
 * the next thread free, but three operations on the hot path walked it:
 *
 *   - enqueue_ready() inserted in order. Head and tail insertion were special-
 *     cased (a just-preempted thread sorts at the tail), but a waking thread
 *     lands in the middle of the distribution, which is exactly the case the
 *     fast paths miss — so a wakeup cost O(n) pointer chases with the
 *     run-queue spinlock held and interrupts off.
 *   - removing a specific thread (exit, affinity change, block) scanned the
 *     whole queue to find it.
 *   - work stealing scanned the victim's queue for an affinity match, holding
 *     the victim's lock across the walk.
 *
 * With n runnable threads on a core those are O(n) with a lock held, and the
 * lock is the one other cores contend to steal from. This is the same reason
 * Linux moved CFS off a list and onto an rbtree: insertion and removal become
 * O(log n), and "who runs next" stays free because the leftmost node is
 * cached.
 *
 * Ordering is (vruntime, tid). vruntime alone is not a total order — ties are
 * common right after a fork, when children start at the parent's vruntime —
 * and a tree needs one. tid breaks the tie deterministically; it cannot
 * starve anyone, because a thread's vruntime advances as soon as it runs.
 *
 * `leftmost` is the minimum, kept current by both insert and erase. It also
 * takes over the role rq->head played for the lock-free readers: "is anything
 * runnable on this CPU" and the address MONITOR/MWAIT arms against in the
 * idle loop. Like rq->head before it, it is written under the run-queue lock
 * and read without it.
 *
 * This header is deliberately free of kernel includes and touches only these
 * fields of thread_t — rb_left, rb_right, rb_parent, rb_color, vruntime, tid
 * — so the tree can be compiled and exhaustively tested on its own.
 * ============================================================================ */
#pragma once

#define RQ_RED   0
#define RQ_BLACK 1

typedef struct rq_tree {
    thread_t *root;
    thread_t *leftmost;   /* cached minimum; NULL iff the tree is empty */
} rq_tree_t;

static inline void rq_tree_init(rq_tree_t *t)
{
    t->root = (thread_t *)0;
    t->leftmost = (thread_t *)0;
}

static inline int rq_tree_empty(const rq_tree_t *t)
{
    return t->root == (thread_t *)0;
}

static inline thread_t *rq_tree_first(const rq_tree_t *t)
{
    return t->leftmost;
}

/* Total order over the queue: virtual runtime first, thread id to break the
 * ties a fork storm produces. */
static inline int rq_tree_less(const thread_t *a, const thread_t *b)
{
    if (a->vruntime != b->vruntime) return a->vruntime < b->vruntime;
    return a->tid < b->tid;
}

/* In-order successor, for the work-stealing walk. */
static inline thread_t *rq_tree_next(thread_t *n)
{
    if (n->rb_right) {
        n = n->rb_right;
        while (n->rb_left) n = n->rb_left;
        return n;
    }
    thread_t *p = n->rb_parent;
    while (p && n == p->rb_right) {
        n = p;
        p = p->rb_parent;
    }
    return p;
}

static inline void rq_rotate_left(rq_tree_t *t, thread_t *x)
{
    thread_t *y = x->rb_right;
    x->rb_right = y->rb_left;
    if (y->rb_left) y->rb_left->rb_parent = x;
    y->rb_parent = x->rb_parent;
    if (!x->rb_parent)                    t->root = y;
    else if (x == x->rb_parent->rb_left)  x->rb_parent->rb_left = y;
    else                                  x->rb_parent->rb_right = y;
    y->rb_left = x;
    x->rb_parent = y;
}

static inline void rq_rotate_right(rq_tree_t *t, thread_t *x)
{
    thread_t *y = x->rb_left;
    x->rb_left = y->rb_right;
    if (y->rb_right) y->rb_right->rb_parent = x;
    y->rb_parent = x->rb_parent;
    if (!x->rb_parent)                     t->root = y;
    else if (x == x->rb_parent->rb_right)  x->rb_parent->rb_right = y;
    else                                   x->rb_parent->rb_left = y;
    y->rb_right = x;
    x->rb_parent = y;
}

static inline void rq_insert_fixup(rq_tree_t *t, thread_t *z)
{
    while (z->rb_parent && z->rb_parent->rb_color == RQ_RED) {
        thread_t *p = z->rb_parent;
        thread_t *g = p->rb_parent;
        if (!g) break;                      /* parent is a red root */

        if (p == g->rb_left) {
            thread_t *u = g->rb_right;
            if (u && u->rb_color == RQ_RED) {
                p->rb_color = RQ_BLACK;
                u->rb_color = RQ_BLACK;
                g->rb_color = RQ_RED;
                z = g;
            } else {
                if (z == p->rb_right) {
                    z = p;
                    rq_rotate_left(t, z);
                    p = z->rb_parent;
                    g = p->rb_parent;
                }
                p->rb_color = RQ_BLACK;
                g->rb_color = RQ_RED;
                rq_rotate_right(t, g);
            }
        } else {
            thread_t *u = g->rb_left;
            if (u && u->rb_color == RQ_RED) {
                p->rb_color = RQ_BLACK;
                u->rb_color = RQ_BLACK;
                g->rb_color = RQ_RED;
                z = g;
            } else {
                if (z == p->rb_left) {
                    z = p;
                    rq_rotate_right(t, z);
                    p = z->rb_parent;
                    g = p->rb_parent;
                }
                p->rb_color = RQ_BLACK;
                g->rb_color = RQ_RED;
                rq_rotate_left(t, g);
            }
        }
    }
    t->root->rb_color = RQ_BLACK;
}

static inline void rq_tree_insert(rq_tree_t *t, thread_t *z)
{
    thread_t *y = (thread_t *)0;
    thread_t *x = t->root;
    int is_leftmost = 1;

    while (x) {
        y = x;
        if (rq_tree_less(z, x)) {
            x = x->rb_left;
        } else {
            x = x->rb_right;
            is_leftmost = 0;       /* went right at least once */
        }
    }

    z->rb_parent = y;
    z->rb_left = (thread_t *)0;
    z->rb_right = (thread_t *)0;
    z->rb_color = RQ_RED;

    if (!y)                       t->root = z;
    else if (rq_tree_less(z, y))  y->rb_left = z;
    else                          y->rb_right = z;

    if (is_leftmost) t->leftmost = z;

    rq_insert_fixup(t, z);
}

/* Replace the subtree rooted at @u with the one rooted at @v. */
static inline void rq_transplant(rq_tree_t *t, thread_t *u, thread_t *v)
{
    if (!u->rb_parent)                    t->root = v;
    else if (u == u->rb_parent->rb_left)  u->rb_parent->rb_left = v;
    else                                  u->rb_parent->rb_right = v;
    if (v) v->rb_parent = u->rb_parent;
}

/*
 * Rebalance after removing a black node. @x is the node that took the removed
 * node's place (possibly NULL) and @xp is its parent — passed explicitly
 * because a NULL child cannot carry one, and this tree has no sentinel.
 */
static inline void rq_erase_fixup(rq_tree_t *t, thread_t *x, thread_t *xp)
{
    while (x != t->root && (!x || x->rb_color == RQ_BLACK)) {
        if (!xp) break;

        if (x == xp->rb_left) {
            thread_t *w = xp->rb_right;
            if (w && w->rb_color == RQ_RED) {
                w->rb_color = RQ_BLACK;
                xp->rb_color = RQ_RED;
                rq_rotate_left(t, xp);
                w = xp->rb_right;
            }
            if (!w) { x = xp; xp = xp->rb_parent; continue; }

            if ((!w->rb_left  || w->rb_left->rb_color  == RQ_BLACK) &&
                (!w->rb_right || w->rb_right->rb_color == RQ_BLACK)) {
                w->rb_color = RQ_RED;
                x = xp;
                xp = xp->rb_parent;
            } else {
                if (!w->rb_right || w->rb_right->rb_color == RQ_BLACK) {
                    if (w->rb_left) w->rb_left->rb_color = RQ_BLACK;
                    w->rb_color = RQ_RED;
                    rq_rotate_right(t, w);
                    w = xp->rb_right;
                }
                if (w) {
                    w->rb_color = xp->rb_color;
                    xp->rb_color = RQ_BLACK;
                    if (w->rb_right) w->rb_right->rb_color = RQ_BLACK;
                    rq_rotate_left(t, xp);
                }
                x = t->root;
                xp = (thread_t *)0;
            }
        } else {
            thread_t *w = xp->rb_left;
            if (w && w->rb_color == RQ_RED) {
                w->rb_color = RQ_BLACK;
                xp->rb_color = RQ_RED;
                rq_rotate_right(t, xp);
                w = xp->rb_left;
            }
            if (!w) { x = xp; xp = xp->rb_parent; continue; }

            if ((!w->rb_right || w->rb_right->rb_color == RQ_BLACK) &&
                (!w->rb_left  || w->rb_left->rb_color  == RQ_BLACK)) {
                w->rb_color = RQ_RED;
                x = xp;
                xp = xp->rb_parent;
            } else {
                if (!w->rb_left || w->rb_left->rb_color == RQ_BLACK) {
                    if (w->rb_right) w->rb_right->rb_color = RQ_BLACK;
                    w->rb_color = RQ_RED;
                    rq_rotate_left(t, w);
                    w = xp->rb_left;
                }
                if (w) {
                    w->rb_color = xp->rb_color;
                    xp->rb_color = RQ_BLACK;
                    if (w->rb_left) w->rb_left->rb_color = RQ_BLACK;
                    rq_rotate_right(t, xp);
                }
                x = t->root;
                xp = (thread_t *)0;
            }
        }
    }
    if (x) x->rb_color = RQ_BLACK;
}

static inline void rq_tree_erase(rq_tree_t *t, thread_t *z)
{
    /* Take the successor before the shape changes under us. */
    if (t->leftmost == z) t->leftmost = rq_tree_next(z);

    thread_t *y = z;
    int y_orig_color = y->rb_color;
    thread_t *x;
    thread_t *xp;

    if (!z->rb_left) {
        x  = z->rb_right;
        xp = z->rb_parent;
        rq_transplant(t, z, z->rb_right);
    } else if (!z->rb_right) {
        x  = z->rb_left;
        xp = z->rb_parent;
        rq_transplant(t, z, z->rb_left);
    } else {
        y = z->rb_right;
        while (y->rb_left) y = y->rb_left;
        y_orig_color = y->rb_color;
        x = y->rb_right;

        if (y->rb_parent == z) {
            xp = y;
            if (x) x->rb_parent = y;
        } else {
            xp = y->rb_parent;
            rq_transplant(t, y, y->rb_right);
            y->rb_right = z->rb_right;
            y->rb_right->rb_parent = y;
        }
        rq_transplant(t, z, y);
        y->rb_left = z->rb_left;
        y->rb_left->rb_parent = y;
        y->rb_color = z->rb_color;
    }

    if (y_orig_color == RQ_BLACK) rq_erase_fixup(t, x, xp);

    z->rb_left = (thread_t *)0;
    z->rb_right = (thread_t *)0;
    z->rb_parent = (thread_t *)0;
    z->rb_color = RQ_BLACK;
}
