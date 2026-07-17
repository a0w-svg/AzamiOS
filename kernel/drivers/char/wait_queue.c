/**
 * kernel/drivers/char/wait_queue.c — I/O Wait Queue Implementation
 *
 * Integrates with the scheduler's PROC_BLOCKED state:
 *   - wq_wait_event sets caller to PROC_BLOCKED, enqueues, then yields.
 *   - wq_wake_* sets PROC_READY, removing the block so the scheduler
 *     picks the task up on the next preemption tick.
 *
 * Spinlock scope: minimal — only queue mutation; the yield/HLT loop
 * runs without the lock held to avoid deadlock with the IRQ path.
 */
#include "../include/wait_queue.h"
#include "../../proc/include/scheduler.h"
#include "../../arch/include/spinlock.h"

void wq_wait_event(wait_queue_t *wq) {
    process_t *cur = scheduler_get_current();
    if (!cur) return; /* bare-metal boot path — no process yet */

    /* ── Enqueue self under lock ────────────────────────────────────── */
    unsigned long flags;
    spinlock_acquire_irqsave(&wq->lock, &flags);

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->entries[i].proc == (void*)0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Queue full — caller cannot block; return immediately (lossy)  */
        spinlock_release_irqrestore(&wq->lock, flags);
        return;
    }

    wq->entries[slot].proc  = cur;
    wq->entries[slot].woken = false;
    wq->count++;

    /* Mark process BLOCKED before releasing — prevents a race where     */
    /* the ISR fires between unlock and state change.                    */
    cur->state = PROC_BLOCKED;

    spinlock_release_irqrestore(&wq->lock, flags);

    /* ── Yield loop: spin with HLT until woken ─────────────────────── */
    /* The preemptive scheduler skips PROC_BLOCKED tasks, so this core  */
    /* will be reassigned to another task on the next timer IRQ.        */
    while (1) {
        /* Re-check woken flag under lock */
        spinlock_acquire_irqsave(&wq->lock, &flags);
        bool done = wq->entries[slot].woken;
        if (done) {
            wq->entries[slot].proc  = (void*)0;
            wq->entries[slot].woken = false;
            wq->count--;
        }
        spinlock_release_irqrestore(&wq->lock, flags);

        if (done) break;

        /* Allow interrupts and halt until the next IRQ wakes us         */
        asm volatile("sti; hlt; cli");
    }

    /* Restore RUNNING — scheduler will set it formally on next switch;  */
    /* for now just ensure we are not stuck in BLOCKED.                  */
    cur->state = PROC_RUNNING;
}

void wq_wake_one(wait_queue_t *wq) {
    unsigned long flags;
    spinlock_acquire_irqsave(&wq->lock, &flags);

    for (int i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->entries[i].proc != (void*)0 && !wq->entries[i].woken) {
            wq->entries[i].proc->state = PROC_READY; /* unblock task    */
            wq->entries[i].woken       = true;
            break;
        }
    }

    spinlock_release_irqrestore(&wq->lock, flags);
}

void wq_wake_all(wait_queue_t *wq) {
    unsigned long flags;
    spinlock_acquire_irqsave(&wq->lock, &flags);

    for (int i = 0; i < WQ_MAX_WAITERS; i++) {
        if (wq->entries[i].proc != (void*)0) {
            wq->entries[i].proc->state = PROC_READY; /* unblock each    */
            wq->entries[i].woken       = true;
        }
    }

    spinlock_release_irqrestore(&wq->lock, flags);
}
