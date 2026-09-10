/* ============================================================================
 * AzamiOS — POSIX per-process timers
 * File: kernel/ktimer.h
 *
 * One expiry engine behind three POSIX interfaces:
 *
 *   alarm(2)                    one-shot SIGALRM, seconds
 *   setitimer(2)/getitimer(2)   ITIMER_REAL / _VIRTUAL / _PROF
 *   timer_create(2) and family  arbitrarily many per-process timers
 *
 * Expiry is driven by a kernel thread that wakes on every scheduler tick
 * rather than by the tick handler itself, so firing a timer can take locks
 * and go through the normal signal path — including default actions, which a
 * SIGALRM with no handler installed depends on.  Resolution is therefore one
 * tick (10 ms), the same granularity as every other timed wait here.
 *
 * ITIMER_VIRTUAL and ITIMER_PROF decrement against the owning process's
 * accumulated CPU time (process_t::utime_ticks / stime_ticks), not wall time,
 * which is what distinguishes them from ITIMER_REAL.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"

struct process;

/* which[] values for the interval timers */
#define ITIMER_REAL     0
#define ITIMER_VIRTUAL  1
#define ITIMER_PROF     2

/* timer_settime() flags */
#define TIMER_ABSTIME   1

/* sigevent notification types */
#define SIGEV_SIGNAL    0
#define SIGEV_NONE      1
#define SIGEV_THREAD    2

#define KTIMER_MAX      64      /* armed timers, system-wide */

/** ktimer_init() — reset the table and start the expiry thread. */
void ktimer_init(void);

/** ktimer_process_exit(proc) — drop every timer owned by a dying process. */
void ktimer_process_exit(struct process *proc);

/* ── timer_create(2) family ──────────────────────────────────────────────── */

/**
 * ktimer_create(proc, clockid, notify, signo, sigval) → timer id, or negative.
 *
 * The timer is created disarmed; ktimer_settime() starts it.
 */
s64 ktimer_create(struct process *proc, int clockid, int notify, int signo, u64 sigval);

/**
 * ktimer_settime(proc, id, abstime, value, interval, old_value, old_interval)
 *
 * @value of 0 disarms.  All tick counts are relative unless @abstime is set,
 * in which case @value is an absolute tick.  Returns 0 or negative errno.
 */
s64 ktimer_settime(struct process *proc, int id, bool abstime,
                   u64 value, u64 interval, u64 *old_value, u64 *old_interval);

s64 ktimer_gettime(struct process *proc, int id, u64 *value, u64 *interval);
s64 ktimer_getoverrun(struct process *proc, int id);
s64 ktimer_delete(struct process *proc, int id);

/* ── alarm(2) and the interval timers ───────────────────────────────────── */

/** ktimer_alarm(proc, ticks) → ticks remaining on the previous alarm. */
u64 ktimer_alarm(struct process *proc, u64 ticks);

s64 ktimer_setitimer(struct process *proc, int which, u64 value, u64 interval,
                     u64 *old_value, u64 *old_interval);
s64 ktimer_getitimer(struct process *proc, int which, u64 *value, u64 *interval);
