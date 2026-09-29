#include "softirq.h"
#include <arch/x86_64/cpu/spinlock.h>
#include <kernel/sched/sched.h>
#include <azami/defs.h>

static spinlock_t tasklet_lock = SPINLOCK_INIT;
static struct tasklet_struct *tasklet_list = NULL;
static int bh_disable_count = 0; /* Note: should be per-cpu, but simple global for now */

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

    if (bh_disable_count > 0)
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
    __atomic_fetch_add(&bh_disable_count, 1, __ATOMIC_SEQ_CST);
}

void local_bh_enable(void) {
    if (__atomic_sub_fetch(&bh_disable_count, 1, __ATOMIC_SEQ_CST) == 0) {
        tasklet_action();
    }
}
