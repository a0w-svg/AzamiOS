/**
 * kernel/drivers/include/wait_queue.h — I/O Wait Queue + Scheduler Bridge
 *
 * Provides a lightweight wait-queue so that drivers can block a process on
 * I/O and have the scheduler skip it (PROC_BLOCKED) until data is ready.
 *
 * Usage pattern:
 *   Driver ISR path:  wq_wake_all(&uart_rx_wq);
 *   Kernel read path: wq_wait_event(&uart_rx_wq);  // blocks caller
 */
#ifndef AZAMI_WAIT_QUEUE_H
#define AZAMI_WAIT_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "../arch/include/spinlock.h"
#include "../proc/include/process.h"

/* ── Maximum waiters per queue (static allocation, no heap) ─────────── */
#define WQ_MAX_WAITERS 16

/* ── Wait queue entry: one slot per blocked process ─────────────────── */
typedef struct wq_entry {
    process_t  *proc;      /* blocked process; NULL = slot free       */
    bool        woken;     /* set by wq_wake_*(); cleared by waiter   */
} wq_entry_t;

/* ── Wait queue head ─────────────────────────────────────────────────── */
typedef struct wait_queue {
    volatile int lock;                  /* spinlock protecting this wq */
    wq_entry_t   entries[WQ_MAX_WAITERS];
    uint32_t     count;                 /* active waiter count         */
} wait_queue_t;

/* ── Initialise a wait queue (zero-safe; also callable at runtime) ──── */
static inline void wq_init(wait_queue_t *wq) {
    wq->lock  = 0;
    wq->count = 0;
    for (int i = 0; i < WQ_MAX_WAITERS; i++) {
        wq->entries[i].proc  = (void*)0;
        wq->entries[i].woken = false;
    }
}

/**
 * wq_wait_event — put the calling process to sleep until woken.
 *
 * Sets caller state to PROC_BLOCKED, enqueues it, then spins (the
 * scheduler will preempt and skip PROC_BLOCKED tasks in its runqueue walk).
 * Returns when wq_wake_all or wq_wake_one fires and sets woken=true.
 *
 * MUST NOT be called from interrupt context.
 */
void wq_wait_event(wait_queue_t *wq);

/**
 * wq_wake_one — wake the oldest blocked waiter.
 * Safe to call from interrupt context (spinlock-protected).
 */
void wq_wake_one(wait_queue_t *wq);

/**
 * wq_wake_all — wake every blocked waiter.
 * Safe to call from interrupt context.
 */
void wq_wake_all(wait_queue_t *wq);

#endif /* AZAMI_WAIT_QUEUE_H */
