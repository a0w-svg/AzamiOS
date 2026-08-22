/* ============================================================================
 * AzamiOS — Spinlock (x86_64, ticket-based for fairness)
 * File: arch/x86_64/cpu/spinlock.h
 *
 * Ticket spinlock: each waiter gets a sequential "ticket" and waits until
 * the "now serving" counter reaches its ticket. This guarantees FIFO ordering
 * and prevents starvation unlike a simple test-and-set lock.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../../../include/azami/defs.h"

/* ── Ticket spinlock ─────────────────────────────────────────────────────── */
typedef struct {
    _Atomic u32 ticket;   /* Next ticket to issue */
    _Atomic u32 serving;  /* Ticket currently being served */
} spinlock_t;

typedef unsigned long irqflags_t;

#define SPINLOCK_INIT  { 0, 0 }   /* ticket=0, serving=0 */

static __always_inline void spinlock_init(spinlock_t *l)
{
    __atomic_store_n(&l->ticket, 0U, __ATOMIC_RELAXED);
    __atomic_store_n(&l->serving, 0U, __ATOMIC_RELAXED);
}

/**
 * spinlock_lock(lock) — Acquire the spinlock (busy-wait with pause).
 * Does NOT save or modify interrupt flags.
 */
static __always_inline void spinlock_lock(spinlock_t *l)
{
    u32 ticket = __atomic_fetch_add(&l->ticket, 1U, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&l->serving, __ATOMIC_ACQUIRE) != ticket)
        cpu_pause();
}

/**
 * spinlock_unlock(lock) — Release the spinlock.
 */
static __always_inline void spinlock_unlock(spinlock_t *l)
{
    u32 next = __atomic_load_n(&l->serving, __ATOMIC_RELAXED) + 1U;
    __atomic_store_n(&l->serving, next, __ATOMIC_RELEASE);
}

/**
 * spinlock_lock_irqsave(lock) — Disable interrupts, then acquire the lock.
 * Returns the previous RFLAGS value (use with spinlock_unlock_irqrestore).
 */
static __always_inline irqflags_t spinlock_lock_irqsave(spinlock_t *l)
{
    irqflags_t flags;
    __asm__ volatile(
        "pushfq         \n"
        "popq  %0       \n"
        "cli            \n"
        : "=r"(flags) : : "memory"
    );
    spinlock_lock(l);
    return flags;
}

/**
 * spinlock_unlock_irqrestore(lock, flags) — Release the lock, restore RFLAGS.
 */
static __always_inline void spinlock_unlock_irqrestore(spinlock_t *l, irqflags_t flags)
{
    spinlock_unlock(l);
    __asm__ volatile(
        "pushq %0   \n"
        "popfq      \n"
        : : "r"(flags) : "memory"
    );
}

/**
 * spinlock_try_lock(lock) — Non-blocking attempt. Returns true if acquired.
 */
static __always_inline bool spinlock_try_lock(spinlock_t *l)
{
    u32 ticket  = __atomic_load_n(&l->ticket,  __ATOMIC_RELAXED);
    u32 serving = __atomic_load_n(&l->serving, __ATOMIC_RELAXED);
    if (ticket != serving) return false;
    return __atomic_compare_exchange_n(&l->ticket, &ticket, ticket + 1U,
                                       false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
}
