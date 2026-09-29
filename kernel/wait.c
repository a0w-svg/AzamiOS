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
    
    sched_block(THREAD_SLEEPING);
    spinlock_unlock_irqrestore(&wq->lock, flags);

    sched_yield();

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
