/* ============================================================================
 * AzamiOS Userspace — Fast Userspace Mutexes (linux/futex.h)
 * File: userland/libc/include/linux/futex.h
 *
 * Both futex interfaces:
 *
 *   futex(2)          the original multiplexer, selected by an opcode
 *   futex_wake(2)     the futex2 calls, one system call per operation, with a
 *   futex_wait(2)     flag word describing the futex itself rather than
 *   futex_requeue(2)  the operation
 *
 * Prefer the futex2 calls in new code. futex_wait() takes an *absolute*
 * deadline against a clock you name, which is what pthread_cond_timedwait()
 * actually needs — FUTEX_WAIT's relative timeout has to be recomputed on every
 * spurious wakeup, and gets it slightly wrong every time.
 * ============================================================================ */
#pragma once

#include "../sys/types.h"
#include "../sys/futex.h"
#include "../sys/syscall.h" /* struct futex_waitv, FUTEX2_* — canonical uapi */
#include "../time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The futex(2) opcodes and the futex() prototype come from <sys/futex.h>,
 * included above; only the bitset wildcard is missing from it. */
#ifndef FUTEX_BITSET_MATCH_ANY
#define FUTEX_BITSET_MATCH_ANY  0xFFFFFFFF
#endif

/* futex2 flag word (FUTEX2_SIZE_U8/U16/U32/U64/NUMA/PRIVATE), FUTEX_WAITV_MAX,
 * and `struct futex_waitv` all come from <sys/syscall.h>'s canonical uapi
 * header (include/azami/uapi/syscall_nr.h) — this used to be a second,
 * hand-duplicated copy of all three, which is exactly the kind of drift
 * scripts/check_uapi_sync.sh exists to catch. */

/**
 * futex_wake() — wake up to @nr waiters on @uaddr whose bitset intersects
 * @mask. Pass ~0 as @mask to wake regardless of bitset. Returns the number
 * woken.
 */
int futex_wake(void *uaddr, unsigned long mask, int nr, unsigned int flags);

/**
 * futex_wait() — block while *@uaddr == @val.
 *
 * @timeout is an absolute deadline on @clockid (CLOCK_REALTIME or
 * CLOCK_MONOTONIC), or NULL to wait indefinitely. Returns 0 when woken,
 * -1/EAGAIN if the value did not match on entry, -1/ETIMEDOUT at the deadline,
 * -1/EINTR on a signal.
 */
int futex_wait(void *uaddr, unsigned long val, unsigned long mask,
               unsigned int flags, struct timespec *timeout, clockid_t clockid);

/**
 * futex_requeue() — wake @nr_wake waiters on waiters[0] and move up to
 * @nr_requeue of the rest onto waiters[1], without ever letting them run in
 * between. This is what makes pthread_cond_broadcast() cost one wakeup instead
 * of a thundering herd on the associated mutex.
 */
int futex_requeue(struct futex_waitv *waiters, unsigned int flags,
                  int nr_wake, int nr_requeue);

/** futex_waitv() — wait on several futexes at once; returns the index woken. */
int futex_waitv(struct futex_waitv *waiters, unsigned int nr_futexes,
                unsigned int flags, struct timespec *timeout, clockid_t clockid);

#ifdef __cplusplus
}
#endif
