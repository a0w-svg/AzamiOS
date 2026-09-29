#include "softirq.h"
#include <arch/x86_64/cpu/spinlock.h>
#include <arch/x86_64/cpu/smp.h>
#include <kernel/sched/sched.h>
#include <azami/defs.h>

static spinlock_t tasklet_lock = SPINLOCK_INIT;
static struct tasklet_struct *tasklet_list = NULL;

/* Per-CPU bottom-half disable nesting count.  The Linux semantics are per-CPU:
 * disabling BH on one core must not block tasklet dispatch on another.  The
 * old global atomic counter ping-ponged a shared cache line on every
 * local_bh_disable/enable pair across all cores. */
static int bh_disable_count[SMP_MAX_CPUS];

static inline int *my_bh_count(void)
{
    u32 id = smp_current_cpu_id();
    if (id >= SMP_MAX_CPUS) id = 0;
    return &bh_disable_count[id];
}

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data) {
    t->next = NULL;
    t->state = 0;
    t->count = 0;
    t->func = func;
    t->data = data;
}

void tasklet_schedule(struct tasklet_struct *t) {
    unsigned long flags;
    flags = spinlock_lock_irqsave(&tasklet_lock);
    if (!(t->state & BIT(TASKLET_STATE_SCHED))) {
        t->state |= BIT(TASKLET_STATE_SCHED);
        t->next = tasklet_list;
        tasklet_list = t;
    }
    spinlock_unlock_irqrestore(&tasklet_lock, flags);
}

void tasklet_hi_schedule(struct tasklet_struct *t) {
    tasklet_schedule(t);
}

void tasklet_disable(struct tasklet_struct *t) {
    __atomic_fetch_add(&t->count, 1, __ATOMIC_SEQ_CST);
}

void tasklet_enable(struct tasklet_struct *t) {
    __atomic_sub_fetch(&t->count, 1, __ATOMIC_SEQ_CST);
}

void tasklet_kill(struct tasklet_struct *t) {
    while (t->state & BIT(TASKLET_STATE_SCHED)) {
        sched_sleep(1);
    }
}

void tasklet_action(void) {
    unsigned long flags;
    struct tasklet_struct *list, *t;

    if (*my_bh_count() > 0)
        return;

    flags = spinlock_lock_irqsave(&tasklet_lock);
    list = tasklet_list;
    tasklet_list = NULL;
    spinlock_unlock_irqrestore(&tasklet_lock, flags);

    while (list) {
        t = list;
        list = list->next;

        if (t->count == 0) {
            t->state &= ~BIT(TASKLET_STATE_SCHED);
            t->state |= BIT(TASKLET_STATE_RUN);
            t->func(t->data);
            t->state &= ~BIT(TASKLET_STATE_RUN);
        } else {
            /* Put back */
            flags = spinlock_lock_irqsave(&tasklet_lock);
            t->next = tasklet_list;
            tasklet_list = t;
            spinlock_unlock_irqrestore(&tasklet_lock, flags);
        }
    }
}

void local_bh_disable(void) {
    irqflags_t f = spinlock_lock_irqsave(&tasklet_lock);
    (*my_bh_count())++;
    spinlock_unlock_irqrestore(&tasklet_lock, f);
}

void local_bh_enable(void) {
    irqflags_t f = spinlock_lock_irqsave(&tasklet_lock);
    int val = --(*my_bh_count());
    spinlock_unlock_irqrestore(&tasklet_lock, f);
    if (val == 0) {
        tasklet_action();
    }
}
