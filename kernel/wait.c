#include "wait.h"
#include "sched/sched.h"

void wait_queue_init(void)
{
}

void __wait_event_sleep(wait_queue_head_t *wq)
{
    unsigned long flags;
    struct wait_queue_entry entry;
    struct wait_queue_entry **p;

    entry.private = sched_current_thread();
    entry.next = NULL;

    flags = spinlock_lock_irqsave(&wq->lock);
    p = &wq->first;
    while (*p) {
        p = &(*p)->next;
    }
    *p = &entry;
    spinlock_unlock_irqrestore(&wq->lock, flags);

    /* sched_block() switches threads. Never carry the queue lock across that
     * switch: the IRQ wakeup needs the same lock to make us runnable again.
     * A wake between unlock and sched_block() sets this thread READY, which
     * sched_block() recognizes and leaves running. */
    sched_block(THREAD_SLEEPING);

    flags = spinlock_lock_irqsave(&wq->lock);
    p = &wq->first;
    while (*p) {
        if (*p == &entry) {
            *p = entry.next;
            break;
        }
        p = &(*p)->next;
    }
    spinlock_unlock_irqrestore(&wq->lock, flags);
}

void __wait_event_sleep_timeout(wait_queue_head_t *wq, u64 deadline_ns)
{
    struct wait_queue_entry entry = { .private = sched_current_thread(), .next = NULL };
    irqflags_t flags = spinlock_lock_irqsave(&wq->lock);
    struct wait_queue_entry **p = &wq->first;
    while (*p) p = &(*p)->next;
    *p = &entry;
    spinlock_unlock_irqrestore(&wq->lock, flags);

    /* An IRQ normally wakes this sleeper. Poll once per millisecond as well
     * so a controller that never raises its completion IRQ still reaches the
     * caller's deadline. An IRQ racing the sleep can cost at most 1 ms. */
    u64 now = ktime_get_ns();
    if (now < deadline_ns) {
        u64 next = now + 1000000ULL;
        sched_sleep_until_ns(next < deadline_ns ? next : deadline_ns);
    }

    flags = spinlock_lock_irqsave(&wq->lock);
    p = &wq->first;
    while (*p) {
        if (*p == &entry) {
            *p = entry.next;
            break;
        }
        p = &(*p)->next;
    }
    spinlock_unlock_irqrestore(&wq->lock, flags);
}

void wake_up(wait_queue_head_t *wq)
{
    unsigned long flags;
    struct wait_queue_entry *entry;
    
    flags = spinlock_lock_irqsave(&wq->lock);
    if (wq->first) {
        entry = wq->first;
        wq->first = entry->next;
        sched_unblock((thread_t*)entry->private);
    }
    spinlock_unlock_irqrestore(&wq->lock, flags);
}

void wake_up_all(wait_queue_head_t *wq)
{
    unsigned long flags;
    struct wait_queue_entry *entry;
    
    flags = spinlock_lock_irqsave(&wq->lock);
    while (wq->first) {
        entry = wq->first;
        wq->first = entry->next;
        sched_unblock((thread_t*)entry->private);
    }
    spinlock_unlock_irqrestore(&wq->lock, flags);
}

void wake_up_interruptible(wait_queue_head_t *wq)
{
    wake_up(wq);
}

void wake_up_interruptible_all(wait_queue_head_t *wq)
{
    wake_up_all(wq);
}
