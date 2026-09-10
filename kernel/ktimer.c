/* ============================================================================
 * AzamiOS — POSIX per-process timers implementation
 * File: kernel/ktimer.c
 *
 * See ktimer.h for the interface.  A single fixed table holds every armed
 * timer in the system; each entry remembers the process that owns it, so a
 * process exiting takes its timers with it and a slot is never fired at a
 * stale owner.
 *
 * The expiry thread is what makes signal delivery safe: it runs in ordinary
 * thread context, so it can call sched_kill_process() and get POSIX default
 * actions (a SIGALRM with no handler terminating the process) for free, which
 * firing from the tick handler could not do.
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "ktimer.h"
#include "sched/sched.h"
#include "lib/string.h"
#include "../arch/x86_64/cpu/spinlock.h"

#define SIGALRM_NR   14
#define SIGVTALRM_NR 26
#define SIGPROF_NR   27

typedef struct ktimer {
    bool        used;
    bool        armed;
    struct process *owner;
    u32         owner_pid;
    int         id;             /* handed back to userspace          */
    int         which;          /* ITIMER_* for interval timers, -1 for POSIX */
    int         clockid;
    int         notify;         /* SIGEV_*                            */
    int         signo;
    u64         sigval;
    u64         expiry;         /* absolute tick, or CPU-tick target  */
    u64         interval;       /* 0 = one-shot                       */
    u32         overrun;        /* expiries missed since last fetch   */
} ktimer_t;

static ktimer_t   g_timers[KTIMER_MAX];
static spinlock_t g_ktimer_lock = SPINLOCK_INIT;
static int        g_next_timer_id = 1;

/* CPU time consumed by @proc, in ticks, for the clock a timer counts against. */
static u64 ktimer_cpu_ticks(process_t *proc, int which)
{
    if (which == ITIMER_VIRTUAL) return proc->utime_ticks;
    if (which == ITIMER_PROF)    return proc->utime_ticks + proc->stime_ticks;
    return 0;
}

/* The clock a timer is measured against: wall ticks, or the owner's CPU time. */
static u64 ktimer_now(const ktimer_t *t)
{
    if (t->which == ITIMER_VIRTUAL || t->which == ITIMER_PROF) {
        return ktimer_cpu_ticks((process_t *)t->owner, t->which);
    }
    return sched_get_ticks();
}

/* ── Expiry ──────────────────────────────────────────────────────────────── */

/*
 * One sweep.  Expired timers are collected while the lock is held and the
 * signals raised after it is dropped, so sched_kill_process() never runs with
 * the timer lock held.
 */
static void ktimer_scan(void)
{
    struct { u32 pid; int signo; } fire[KTIMER_MAX];
    u32 nfire = 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    for (u32 i = 0; i < KTIMER_MAX; i++) {
        ktimer_t *t = &g_timers[i];
        if (!t->used || !t->armed) continue;

        u64 now = ktimer_now(t);
        if (now < t->expiry) continue;

        if (t->interval) {
            /* Count every period that elapsed, not just the latest one: a
             * timer that fell behind reports the shortfall as overrun. */
            u64 late = now - t->expiry;
            u64 missed = late / t->interval;
            t->overrun += (u32)missed;
            t->expiry = now + t->interval - (late % t->interval);
        } else {
            t->armed = false;
        }

        if (t->notify != SIGEV_NONE && t->signo && nfire < KTIMER_MAX) {
            fire[nfire].pid   = t->owner_pid;
            fire[nfire].signo = t->signo;
            nfire++;
        }
    }

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);

    for (u32 i = 0; i < nfire; i++) {
        sched_kill_process(fire[i].pid, fire[i].signo);
    }
}

static void ktimer_thread(void *arg)
{
    (void)arg;
    for (;;) {
        sched_sleep(1);
        ktimer_scan();
    }
}

void ktimer_init(void)
{
    memset(g_timers, 0, sizeof(g_timers));

    process_t *kproc = sched_kernel_process();
    if (kproc) {
        thread_create(kproc, (uintptr_t)ktimer_thread, 0, true);
        pr_debug("[KTIMER] POSIX timer engine running (%u slots, 10 ms resolution)\n",
                 (unsigned)KTIMER_MAX);
    }
}

void ktimer_process_exit(struct process *proc)
{
    if (!proc) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        if (g_timers[i].used && g_timers[i].owner == proc) {
            memset(&g_timers[i], 0, sizeof(g_timers[i]));
        }
    }
    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
}

/* ── Lookup helpers (caller holds the lock) ─────────────────────────────── */

static ktimer_t *ktimer_find(struct process *proc, int id)
{
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        ktimer_t *t = &g_timers[i];
        if (t->used && t->owner == proc && t->which < 0 && t->id == id) return t;
    }
    return NULL;
}

static ktimer_t *ktimer_find_itimer(struct process *proc, int which)
{
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        ktimer_t *t = &g_timers[i];
        if (t->used && t->owner == proc && t->which == which) return t;
    }
    return NULL;
}

static ktimer_t *ktimer_alloc(void)
{
    for (u32 i = 0; i < KTIMER_MAX; i++) {
        if (!g_timers[i].used) return &g_timers[i];
    }
    return NULL;
}

/* Ticks left before @t fires, 0 when disarmed. */
static u64 ktimer_remaining(const ktimer_t *t)
{
    if (!t->armed) return 0;
    u64 now = ktimer_now(t);
    return (t->expiry > now) ? (t->expiry - now) : 1;
}

/* ── timer_create(2) family ──────────────────────────────────────────────── */

s64 ktimer_create(struct process *proc, int clockid, int notify, int signo, u64 sigval)
{
    if (!proc) return -(s64)EPERM;
    if (notify != SIGEV_SIGNAL && notify != SIGEV_NONE && notify != SIGEV_THREAD) {
        return -(s64)EINVAL;
    }
    if (notify == SIGEV_SIGNAL && (signo < 1 || signo >= 64)) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_alloc();
    if (!t) {
        spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
        return -(s64)EAGAIN;
    }

    memset(t, 0, sizeof(*t));
    t->used      = true;
    t->armed     = false;
    t->owner     = proc;
    t->owner_pid = ((process_t *)proc)->pid;
    t->id        = g_next_timer_id++;
    t->which     = -1;
    t->clockid   = clockid;
    t->notify    = notify;
    /* SIGEV_THREAD has no thread pool behind it here; it degrades to the
     * signal the caller named, which is what sigev_signo carries anyway. */
    t->signo     = (notify == SIGEV_NONE) ? 0 : (signo ? signo : SIGALRM_NR);
    t->sigval    = sigval;

    int id = t->id;
    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return id;
}

s64 ktimer_settime(struct process *proc, int id, bool abstime,
                   u64 value, u64 interval, u64 *old_value, u64 *old_interval)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find(proc, id);
    if (!t) {
        spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
        return -(s64)EINVAL;
    }

    if (old_value)    *old_value    = ktimer_remaining(t);
    if (old_interval) *old_interval = t->interval;

    if (value == 0) {
        t->armed    = false;
        t->interval = 0;
    } else {
        u64 now = ktimer_now(t);
        t->expiry   = abstime ? value : now + value;
        t->interval = interval;
        t->armed    = true;
        t->overrun  = 0;
    }

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return 0;
}

s64 ktimer_gettime(struct process *proc, int id, u64 *value, u64 *interval)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find(proc, id);
    if (!t) {
        spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
        return -(s64)EINVAL;
    }
    if (value)    *value    = ktimer_remaining(t);
    if (interval) *interval = t->interval;

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return 0;
}

s64 ktimer_getoverrun(struct process *proc, int id)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find(proc, id);
    if (!t) {
        spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
        return -(s64)EINVAL;
    }
    /* POSIX: the count is reported for the most recent expiry and is not
     * cleared by reading it; it is reset when the timer is re-armed. */
    s64 n = t->overrun;
    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return n;
}

s64 ktimer_delete(struct process *proc, int id)
{
    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find(proc, id);
    if (!t) {
        spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
        return -(s64)EINVAL;
    }
    memset(t, 0, sizeof(*t));

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return 0;
}

/* ── alarm(2) and the interval timers ───────────────────────────────────── */

static int itimer_signal(int which)
{
    switch (which) {
    case ITIMER_VIRTUAL: return SIGVTALRM_NR;
    case ITIMER_PROF:    return SIGPROF_NR;
    default:             return SIGALRM_NR;
    }
}

s64 ktimer_setitimer(struct process *proc, int which, u64 value, u64 interval,
                     u64 *old_value, u64 *old_interval)
{
    if (!proc) return -(s64)EPERM;
    if (which != ITIMER_REAL && which != ITIMER_VIRTUAL && which != ITIMER_PROF) {
        return -(s64)EINVAL;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find_itimer(proc, which);
    if (!t) {
        if (value == 0) {
            /* Disarming a timer that was never armed: report zeros. */
            if (old_value)    *old_value    = 0;
            if (old_interval) *old_interval = 0;
            spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
            return 0;
        }
        t = ktimer_alloc();
        if (!t) {
            spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
            return -(s64)EAGAIN;
        }
        memset(t, 0, sizeof(*t));
        t->used      = true;
        t->owner     = proc;
        t->owner_pid = ((process_t *)proc)->pid;
        t->which     = which;
        t->notify    = SIGEV_SIGNAL;
        t->signo     = itimer_signal(which);
    }

    if (old_value)    *old_value    = ktimer_remaining(t);
    if (old_interval) *old_interval = t->interval;

    if (value == 0) {
        t->armed    = false;
        t->interval = 0;
    } else {
        t->expiry   = ktimer_now(t) + value;
        t->interval = interval;
        t->armed    = true;
        t->overrun  = 0;
    }

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return 0;
}

s64 ktimer_getitimer(struct process *proc, int which, u64 *value, u64 *interval)
{
    if (!proc) return -(s64)EPERM;
    if (which != ITIMER_REAL && which != ITIMER_VIRTUAL && which != ITIMER_PROF) {
        return -(s64)EINVAL;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_ktimer_lock);

    ktimer_t *t = ktimer_find_itimer(proc, which);
    if (value)    *value    = t ? ktimer_remaining(t) : 0;
    if (interval) *interval = t ? t->interval : 0;

    spinlock_unlock_irqrestore(&g_ktimer_lock, flags);
    return 0;
}

u64 ktimer_alarm(struct process *proc, u64 ticks)
{
    u64 old_value = 0, old_interval = 0;
    /* alarm(2) is setitimer(ITIMER_REAL) with no repeat, and returns the
     * seconds remaining on the alarm it replaced, rounded up. */
    ktimer_setitimer(proc, ITIMER_REAL, ticks, 0, &old_value, &old_interval);
    return (old_value + 99) / 100;
}
