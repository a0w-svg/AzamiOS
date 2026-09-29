#include "timer_list.h"
#include "sched/sched.h"
#include "../arch/x86_64/cpu/spinlock.h"

static struct timer_list *timer_queue;
static spinlock_t timer_lock = SPINLOCK_INIT;

void timer_list_init(void)
{
    timer_queue = NULL;
    spinlock_init(&timer_lock);
}

void __timer_setup(struct timer_list *timer, void (*function)(struct timer_list *), unsigned int flags)
{
    timer->next = NULL;
    timer->function = function;
    timer->flags = flags;
    timer->pending = false;
    timer->expires = 0;
}

static void insert_timer_locked(struct timer_list *timer)
{
    struct timer_list **p = &timer_queue;
    while (*p && time_before_eq((*p)->expires, timer->expires)) {
        p = &(*p)->next;
    }
    timer->next = *p;
    *p = timer;
    timer->pending = true;
}

static void remove_timer_locked(struct timer_list *timer)
{
    struct timer_list **p = &timer_queue;
    while (*p) {
        if (*p == timer) {
            *p = timer->next;
            timer->next = NULL;
            timer->pending = false;
            break;
        }
        p = &(*p)->next;
    }
}

int mod_timer(struct timer_list *timer, unsigned long expires)
{
    unsigned long flags;
    int was_pending;

    flags = spinlock_lock_irqsave(&timer_lock);
    was_pending = timer->pending;
    if (was_pending) {
        remove_timer_locked(timer);
    }
    timer->expires = expires;
    insert_timer_locked(timer);
    spinlock_unlock_irqrestore(&timer_lock, flags);

    return was_pending;
}

void add_timer(struct timer_list *timer)
{
    mod_timer(timer, timer->expires);
}

int del_timer(struct timer_list *timer)
{
    unsigned long flags;
    int was_pending;

    flags = spinlock_lock_irqsave(&timer_lock);
    was_pending = timer->pending;
    if (was_pending) {
        remove_timer_locked(timer);
    }
    spinlock_unlock_irqrestore(&timer_lock, flags);

    return was_pending;
}

int del_timer_sync(struct timer_list *timer)
{
    int ret = del_timer(timer);
    return ret;
}

int timer_pending(const struct timer_list *timer)
{
    return timer->pending;
}

void run_timer_list(void)
{
    unsigned long flags;
    struct timer_list *timer;

    flags = spinlock_lock_irqsave(&timer_lock);
    while (timer_queue && time_before_eq(timer_queue->expires, jiffies)) {
        timer = timer_queue;
        timer_queue = timer->next;
        timer->pending = false;
        timer->next = NULL;

        /* Call with lock released */
        spinlock_unlock_irqrestore(&timer_lock, flags);
        if (timer->function) {
            timer->function(timer);
        }
        flags = spinlock_lock_irqsave(&timer_lock);
    }
    spinlock_unlock_irqrestore(&timer_lock, flags);
}
