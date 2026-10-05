/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Timekeeping and Timer Syscalls
 * File: kernel/syscall/sys_time.c
 * ============================================================================ */
#include "syscall_internal.h"


/* sys_rt_sigreturn_impl() lives in kernel/signal.c alongside the frame builder. */

s64 sys_pause_impl(pt_regs_t *r)
{
    (void)r;
    /* B-14: block until awakened by signal */
    sched_block(THREAD_BLOCKED);
    return -(s64)EINTR;
}

/* Largest relative sleep that keeps deadline arithmetic in 64 bits
 * (~292 years); anything longer is "forever" for every practical purpose. */
#define SLEEP_MAX_NS    (0x7FFFFFFFFFFFFFFFULL / 2)

s64 sys_nanosleep_impl(pt_regs_t *r)
{
    const struct linux_timespec *req = (const struct linux_timespec *)r->rdi;
    struct linux_timespec *rem = (struct linux_timespec *)r->rsi;
    if (!req) return -(s64)EFAULT;
    if ((uintptr_t)req >= TASK_SIZE_MAX) return -(s64)EFAULT;

    struct linux_timespec t;
    if (copy_from_user(&t, req, sizeof(t)) != 0) return -(s64)EFAULT;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return -(s64)EINVAL;

    u64 ns = ((u64)t.tv_sec > SLEEP_MAX_NS / NSEC_PER_SEC) ? SLEEP_MAX_NS : timespec_to_ns(&t);
    if (ns == 0) {
        sched_yield();
        return 0;
    }

    u64 left = 0;
    s64 rc = sleep_until_mono(ktime_get_ns() + ns, &left);
    if (rc == -(s64)EINTR && rem && (uintptr_t)rem < TASK_SIZE_MAX) {
        struct linux_timespec rts;
        ns_to_timespec(left, &rts);
        if (copy_to_user(rem, &rts, sizeof(rts)) != 0) return -(s64)EFAULT;
    }
    return rc;
}

/* Wall-clock seconds for inode timestamps, SysV IPC and evdev: the coarse
 * CLOCK_REALTIME second the timekeeper published at the last tick. */
u64 get_cached_unix_time(void)
{
    return ktime_get_real_seconds();
}

/* ── POSIX CPU-time clocks ─────────────────────────────────────────────────
 *
 * CLOCK_PROCESS_CPUTIME_ID / CLOCK_THREAD_CPUTIME_ID and the dynamic ids
 * clock_getcpuclockid(3) / pthread_getcpuclockid(3) build, encoded as Linux
 * does: clockid = (~pid << 3) | perthread << 2 | which, with which = 0 PROF
 * (user+system), 1 VIRT (user), 2 SCHED (precise run time). */
#define CPUCLOCK_PROF       0
#define CPUCLOCK_VIRT       1
#define CPUCLOCK_SCHED      2
#define CPUCLOCK_PERTHREAD  4

/* In-flight part of @t's current run, if it is on a CPU right now. */
static inline u64 thread_running_ns(const thread_t *t, u64 now)
{
    u64 start = __atomic_load_n(&t->exec_start_ns, __ATOMIC_RELAXED);
    return (start && now > start) ? now - start : 0;
}

static u64 thread_cputime_ns(const thread_t *t)
{
    u64 now = ktime_get_ns();
    return __atomic_load_n(&t->sum_exec_ns, __ATOMIC_RELAXED) + thread_running_ns(t, now);
}

/* Caller holds sched_lock (keeps the thread list stable). */
static u64 process_cputime_ns_locked(const process_t *p)
{
    u64 now = ktime_get_ns();
    u64 ns = __atomic_load_n(&p->sum_exec_ns, __ATOMIC_RELAXED);
    for (const thread_t *t = p->threads; t; t = t->proc_next)
        ns += thread_running_ns(t, now);
    return ns;
}

static inline irqflags_t cputime_irq_save(void)
{
    irqflags_t f;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(f) :: "memory");
    return f;
}

static inline void cputime_irq_restore(irqflags_t f)
{
    __asm__ volatile("pushq %0\n\tpopfq" :: "r"(f) : "memory", "cc");
}

/* Returns 0 with *@ns filled, or -EINVAL for a malformed id, -ESRCH for a
 * pid/tid that does not exist. */
static s64 cpu_clock_read(s32 clk, u64 *ns)
{
    if (clk == VDSO_CLOCK_THREAD_CPUTIME_ID) {
        /* Our own thread cannot be switched out between reading its total
         * and its run start if interrupts are off — so the reading can
         * never double-count the run that is just ending. */
        irqflags_t f = cputime_irq_save();
        *ns = thread_cputime_ns(sched_current_thread());
        cputime_irq_restore(f);
        return 0;
    }
    if (clk == VDSO_CLOCK_PROCESS_CPUTIME_ID) {
        sched_lock();
        *ns = process_cputime_ns_locked(sched_current_process());
        sched_unlock();
        return 0;
    }
    if (clk >= 0) return -(s64)EINVAL;

    u32 which = (u32)clk & 3;
    bool perthread = ((u32)clk & CPUCLOCK_PERTHREAD) != 0;
    s32 id = ~(clk >> 3);
    if (which > CPUCLOCK_SCHED) return -(s64)EINVAL;   /* 3 = fd-based clock */
    if (id < 0) return -(s64)EINVAL;

    s64 rc = -(s64)ESRCH;
    sched_lock();
    process_t *self = sched_current_process();
    if (perthread) {
        thread_t *tt = NULL;
        if (id == 0) {
            tt = sched_current_thread();
        } else {
            for (process_t *p = sched_get_process_list(); p && !tt; p = p->next)
                for (thread_t *t = p->threads; t; t = t->proc_next)
                    if (t->tid == (u32)id) { tt = t; break; }
            /* Linux only lets a thread clock name a thread of the caller's
             * own thread group. */
            if (tt && tt->proc != self) tt = NULL;
        }
        if (tt) {
            process_t *p = tt->proc;
            if (which == CPUCLOCK_SCHED)
                *ns = thread_cputime_ns(tt);
            else if (which == CPUCLOCK_VIRT)
                *ns = p->utime_ticks * SLEEP_TICK_NS;
            else
                *ns = (p->utime_ticks + p->stime_ticks) * SLEEP_TICK_NS;
            rc = 0;
        }
    } else {
        /* Walked inline, not through sched_get_process_by_pid(): that takes
         * g_sched_lock itself, and it is already held here and not
         * reentrant — clock_gettime(clock_getcpuclockid(pid)) used to spin
         * forever with interrupts off on the CPU that asked. */
        process_t *p = NULL;
        if (id == 0) {
            p = self;
        } else {
            for (process_t *q = sched_get_process_list(); q; q = q->next)
                if (q->pid == (u32)id) { p = q; break; }
        }
        if (p && !p->is_zombie) {
            if (which == CPUCLOCK_SCHED)
                *ns = process_cputime_ns_locked(p);
            else if (which == CPUCLOCK_VIRT)
                *ns = p->utime_ticks * SLEEP_TICK_NS;
            else
                *ns = (p->utime_ticks + p->stime_ticks) * SLEEP_TICK_NS;
            rc = 0;
        }
    }
    sched_unlock();
    return rc;
}

/* Any clock id clock_gettime(2) accepts: 0 and fills @sec/@nsec, or -errno. */
static s64 clock_read_any(s32 clk, s64 *sec, s64 *nsec)
{
    if (clk >= 0 && ktime_get_clock((u32)clk, sec, nsec) == 0) return 0;
    u64 ns;
    s64 rc = cpu_clock_read(clk, &ns);
    if (rc < 0) return rc;
    *sec  = (s64)(ns / NSEC_PER_SEC);
    *nsec = (s64)(ns % NSEC_PER_SEC);
    return 0;
}

s64 sys_clock_gettime_impl(pt_regs_t *r)
{
    s32 clk_id = (s32)r->rdi;
    void *tp   = (void *)r->rsi;

    s64 sec, nsec;
    s64 rc = clock_read_any(clk_id, &sec, &nsec);
    if (rc < 0) return rc;
    if (!tp || (uintptr_t)tp >= TASK_SIZE_MAX) return -(s64)EFAULT;

    struct linux_timespec ts = { .tv_sec = (long)sec, .tv_nsec = (long)nsec };
    if (copy_to_user(tp, &ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_gettimeofday_impl(pt_regs_t *r)
{
    struct linux_timeval *user_tv = (struct linux_timeval *)r->rdi;
    if (user_tv) {
        if ((uintptr_t)user_tv >= TASK_SIZE_MAX) return -(s64)EFAULT;
        s64 sec, nsec;
        ktime_get_clock(VDSO_CLOCK_REALTIME, &sec, &nsec);
        struct linux_timeval tv = { (long)sec, (long)(nsec / 1000) };
        if (copy_to_user(user_tv, &tv, sizeof(tv)) != 0) return -(s64)EFAULT;
    }

    struct { int tz_minuteswest; int tz_dsttime; } *user_tz = (void *)r->rsi;
    if (user_tz) {
        if ((uintptr_t)user_tz >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct { int tz_minuteswest; int tz_dsttime; } tz;
        timekeeping_get_tz(&tz.tz_minuteswest, &tz.tz_dsttime);
        if (copy_to_user(user_tz, &tz, sizeof(tz)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_time_impl(pt_regs_t *r)
{
    long *user_tloc = (long *)r->rdi;
    s64 sec, nsec;
    ktime_get_clock(VDSO_CLOCK_REALTIME, &sec, &nsec);

    if (user_tloc) {
        if ((uintptr_t)user_tloc >= TASK_SIZE_MAX) return -(s64)EFAULT;
        long s = (long)sec;
        if (copy_to_user(user_tloc, &s, sizeof(long)) != 0) return -(s64)EFAULT;
    }
    return sec;
}

struct tms {
    u64 tms_utime;
    u64 tms_stime;
    u64 tms_cutime;
    u64 tms_cstime;
};

s64 sys_times_impl(pt_regs_t *r)
{
    struct tms *buf = (struct tms *)r->rdi;
    process_t *proc = sched_current_process();
    if (buf && (uintptr_t)buf < TASK_SIZE_MAX) {
        /* Clock ticks are the scheduler's, which is what sysconf(_SC_CLK_TCK)
         * reports; children's times are the ones already reaped. */
        struct tms ktms = {
            .tms_utime  = proc ? (long)proc->utime_ticks  : 0,
            .tms_stime  = proc ? (long)proc->stime_ticks  : 0,
            .tms_cutime = proc ? (long)proc->cutime_ticks : 0,
            .tms_cstime = proc ? (long)proc->cstime_ticks : 0,
        };
        if (copy_to_user(buf, &ktms, sizeof(struct tms)) != 0) return -(s64)EFAULT;
    }
    return (s64)sched_get_ticks();
}

typedef struct {
    u32  channel_id;
    u32  interval_ticks;
    int  one_shot;
} az_timer_ctx_t;

static void az_timer_thread(void *arg)
{
    az_timer_ctx_t *ctx = (az_timer_ctx_t *)arg;
    if (!ctx) {
        sched_exit_thread();
        __builtin_unreachable();
    }

    ipc_msg_t kmsg;
    __builtin_memset(&kmsg, 0, sizeof(kmsg));
    kmsg.sender_pid = 51; /* AZ_WM_TIMER_TICK (offset 0 -> msg.type) */
    kmsg.msg_type   = 51; /* AZ_WM_TIMER_TICK */
    kmsg.length     = 0;

    for (;;) {
        sched_sleep(ctx->interval_ticks);

        ipc_channel_t *chan = ipc_channel_find(ctx->channel_id);
        if (!chan) {
            kfree(ctx);
            sched_exit_thread();
            __builtin_unreachable();
        }
        ipc_channel_send(chan, &kmsg, false);
        ipc_channel_put(chan);

        if (ctx->one_shot) {
            kfree(ctx);
            sched_exit_thread();
            __builtin_unreachable();
        }
    }
}

s64 sys_az_set_timer_impl(pt_regs_t *r)
{
    u32 channel_id  = (u32)r->rdi;
    u64 interval_ms = (u64)r->rsi;
    int one_shot    = (int)(s32)r->rdx;

    if (interval_ms < 10)    interval_ms = 10;
    if (interval_ms > 60000) interval_ms = 60000;

    u64 ticks = (interval_ms + 9) / 10;

    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;
    ipc_channel_put(chan);

    az_timer_ctx_t *ctx = (az_timer_ctx_t *)kmalloc(sizeof(az_timer_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->channel_id     = (u32)channel_id;
    ctx->interval_ticks = (u32)(ticks > 0xFFFFFFFFU ? 0xFFFFFFFFU : ticks);
    ctx->one_shot       = one_shot;

    thread_t *t = thread_create(NULL, (uintptr_t)az_timer_thread, (uintptr_t)ctx, true);
    if (!t) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    return 0;
}

s64 sys_clock_getres_impl(pt_regs_t *r)
{
    s32 clk_id = (s32)r->rdi;
    struct linux_timespec *res = (struct linux_timespec *)r->rsi;

    long ns;
    if (clk_id >= 0 && timekeeping_clock_res_ns((u32)clk_id)) {
        ns = (long)timekeeping_clock_res_ns((u32)clk_id);
    } else {
        /* CPU-time clocks: validate the id exactly as clock_gettime would.
         * They count in nanoseconds (CPUCLOCK_SCHED) — Linux reports 1 ns. */
        u64 dummy;
        s64 rc = cpu_clock_read(clk_id, &dummy);
        if (rc < 0) return rc;
        ns = 1;
    }
    if (res) {
        if ((uintptr_t)res >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct linux_timespec ts = { .tv_sec = 0, .tv_nsec = ns };
        if (copy_to_user(res, &ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_clock_settime_impl(pt_regs_t *r)
{
    s32 clk_id = (s32)r->rdi;
    const struct linux_timespec *user_ts = (const struct linux_timespec *)r->rsi;

    /* Only CLOCK_REALTIME can be set; Linux answers EINVAL for every other
     * valid clock before even looking at the caller's privileges. */
    if (clk_id != VDSO_CLOCK_REALTIME) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_TIME)) return -(s64)EPERM;
    if (!user_ts) return -(s64)EFAULT;
    struct linux_timespec ts;
    if (copy_from_user(&ts, user_ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;
    return timekeeping_settime(ts.tv_sec, ts.tv_nsec);
}

#define TIMER_ABSTIME_FLAG  1

/* clock_nanosleep(2): relative or TIMER_ABSTIME sleeps on REALTIME,
 * MONOTONIC, BOOTTIME, TAI and the process CPU clock. Unlike nanosleep()
 * it returns the error rather than setting errno, which is the raw
 * syscall's convention anyway. */
s64 sys_clock_nanosleep_impl(pt_regs_t *r)
{
    s32 clk = (s32)r->rdi;
    int flags = (int)r->rsi;
    const struct linux_timespec *req = (const struct linux_timespec *)r->rdx;
    struct linux_timespec *rem = (struct linux_timespec *)r->r10;

    if (flags & ~TIMER_ABSTIME_FLAG) return -(s64)EINVAL;

    switch (clk) {
    case VDSO_CLOCK_REALTIME:
    case VDSO_CLOCK_MONOTONIC:
    case VDSO_CLOCK_BOOTTIME:
    case VDSO_CLOCK_TAI:
    case VDSO_CLOCK_REALTIME_ALARM:
    case VDSO_CLOCK_BOOTTIME_ALARM:
        break;
    case VDSO_CLOCK_THREAD_CPUTIME_ID:
        return -(s64)EINVAL;            /* Linux: a thread cannot sleep on itself */
    case VDSO_CLOCK_MONOTONIC_RAW:
    case VDSO_CLOCK_REALTIME_COARSE:
    case VDSO_CLOCK_MONOTONIC_COARSE:
        return -(s64)ENOTSUP;
    default:
        if (clk != VDSO_CLOCK_PROCESS_CPUTIME_ID && clk >= 0) return -(s64)EINVAL;
        break;
    }

    if (!req || (uintptr_t)req >= TASK_SIZE_MAX) return -(s64)EFAULT;
    struct linux_timespec t;
    if (copy_from_user(&t, req, sizeof(t)) != 0) return -(s64)EFAULT;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return -(s64)EINVAL;
    u64 req_ns = ((u64)t.tv_sec > SLEEP_MAX_NS / NSEC_PER_SEC) ? SLEEP_MAX_NS : timespec_to_ns(&t);
    bool abs = (flags & TIMER_ABSTIME_FLAG) != 0;

    /* CPU-time clocks advance only while the process runs, so the wait is
     * re-derived after every wake-up rather than converted once. */
    if (clk < 0 || clk == VDSO_CLOCK_PROCESS_CPUTIME_ID) {
        u64 start;
        s64 rc = cpu_clock_read(clk, &start);
        if (rc < 0) return rc;
        u64 target = abs ? req_ns : start + req_ns;
        process_t *proc = sched_current_process();
        for (;;) {
            u64 now;
            if (cpu_clock_read(clk, &now) < 0) return -(s64)EINVAL;
            if (now >= target) return 0;
            if (sleep_signal_pending(proc)) {
                if (!abs && rem && (uintptr_t)rem < TASK_SIZE_MAX) {
                    struct linux_timespec rts;
                    ns_to_timespec(target - now, &rts);
                    if (copy_to_user(rem, &rts, sizeof(rts)) != 0) return -(s64)EFAULT;
                }
                return -(s64)EINTR;
            }
            sched_sleep(1);
        }
    }

    /* Wall-clock family: an absolute deadline on REALTIME/TAI is converted to
     * a monotonic one and re-checked on each wake, so a clock_settime() that
     * moves REALTIME past the deadline ends the sleep, as POSIX requires. */
    u32 base = (clk == VDSO_CLOCK_REALTIME_ALARM) ? VDSO_CLOCK_REALTIME
             : (clk == VDSO_CLOCK_BOOTTIME_ALARM) ? VDSO_CLOCK_BOOTTIME : (u32)clk;
    if (!abs) {
        u64 left = 0;
        s64 rc = sleep_until_mono(ktime_get_ns() + req_ns, &left);
        if (rc == -(s64)EINTR && rem && (uintptr_t)rem < TASK_SIZE_MAX) {
            struct linux_timespec rts;
            ns_to_timespec(left, &rts);
            if (copy_to_user(rem, &rts, sizeof(rts)) != 0) return -(s64)EFAULT;
        }
        return rc;
    }

    if (base == VDSO_CLOCK_MONOTONIC || base == VDSO_CLOCK_MONOTONIC_RAW || base == VDSO_CLOCK_BOOTTIME) {
        return sleep_until_mono(req_ns, NULL);
    }

    for (;;) {
        s64 cs, cn;
        ktime_get_clock(base, &cs, &cn);
        u64 now = (u64)cs * NSEC_PER_SEC + (u64)cn;
        if (now >= req_ns) return 0;
        u64 wait = req_ns - now;
        /* Wall clocks can be stepped: never sleep more than a second at a
         * time against a converted deadline. */
        bool wall = (base == VDSO_CLOCK_REALTIME || base == VDSO_CLOCK_TAI);
        if (wall && wait > NSEC_PER_SEC) wait = NSEC_PER_SEC;
        s64 rc = sleep_until_mono(ktime_get_ns() + wait, NULL);
        if (rc < 0) return rc;          /* TIMER_ABSTIME: rem is not written */
        if (!wall) return 0;
    }
}

/* struct timex is copied up to (not including) the trailing reserved
 * int[11] real Linux's struct carries: the AzamiOS libc's own struct timex
 * (userland/libc/include/sys/timex.h) ends at `tai`, and copying the full
 * Linux size would write past the end of a native caller's buffer. The
 * fields themselves are byte-identical in both. */
#define TIMEX_COPY_SIZE  __builtin_offsetof(struct kernel_timex, _reserved)

static s64 adjtimex_core(process_t *proc, struct kernel_timex *user_buf)
{
    if (!user_buf || (uintptr_t)user_buf >= TASK_SIZE_MAX) return -(s64)EFAULT;
    struct kernel_timex tx;
    memset(&tx, 0, sizeof(tx));
    if (copy_from_user(&tx, user_buf, TIMEX_COPY_SIZE) != 0) return -(s64)EFAULT;

    bool may_set = security_check_permission(proc, CAP_SYS_TIME);
    int rc = timekeeping_adjtimex(&tx, may_set);
    if (rc < 0) return rc;

    if (copy_to_user(user_buf, &tx, TIMEX_COPY_SIZE) != 0) return -(s64)EFAULT;
    return rc;
}

s64 sys_adjtimex_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    return adjtimex_core(proc, (struct kernel_timex *)r->rdi);
}

s64 sys_clock_adjtime_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    s32 clk_id = (s32)r->rdi;
    /* Only CLOCK_REALTIME is disciplined; the monotonic clocks follow its
     * rate but cannot be steered on their own (Linux: EOPNOTSUPP). */
    if (clk_id != VDSO_CLOCK_REALTIME) {
        if (clk_id >= 0 && clk_id < VDSO_NR_CLOCKS && timekeeping_clock_res_ns((u32)clk_id))
            return -(s64)EOPNOTSUPP;
        return -(s64)EINVAL;
    }
    return adjtimex_core(proc, (struct kernel_timex *)r->rsi);
}

/* settimeofday(tv, tz): CAP_SYS_TIME, microsecond precision. A non-NULL tz
 * is recorded (gettimeofday() and the vDSO hand it back) but, exactly as on
 * Linux, never changes how the clock itself is kept. */
s64 sys_settimeofday_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_TIME)) return -(s64)EPERM;

    const struct linux_timeval *user_tv = (const struct linux_timeval *)r->rdi;
    const struct { int tz_minuteswest; int tz_dsttime; } *user_tz = (const void *)r->rsi;

    struct linux_timeval tv;
    if (user_tv) {
        if (copy_from_user(&tv, user_tv, sizeof(tv)) != 0) return -(s64)EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000L) return -(s64)EINVAL;
    }
    if (user_tz) {
        struct { int tz_minuteswest; int tz_dsttime; } tz;
        if (copy_from_user(&tz, user_tz, sizeof(tz)) != 0) return -(s64)EFAULT;
        if (tz.tz_minuteswest < -15 * 60 || tz.tz_minuteswest > 15 * 60) return -(s64)EINVAL;
        timekeeping_set_tz(tz.tz_minuteswest, tz.tz_dsttime);
    }
    if (user_tv) return timekeeping_settime(tv.tv_sec, tv.tv_usec * 1000L);
    return 0;
}

/* The leading fields of struct sigevent; the rest is padding we never read. */
struct k_sigevent {
    u64 sigev_value;
    s32 sigev_signo;
    s32 sigev_notify;
};

s64 sys_timer_create_impl(pt_regs_t *r)
{
    int clockid = (int)r->rdi;
    const void *usev = (const void *)r->rsi;
    void *utimerid   = (void *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!utimerid || (uintptr_t)utimerid >= TASK_SIZE_MAX) return -(s64)EFAULT;

    int notify = SIGEV_SIGNAL;
    int signo  = SIGALRM;
    u64 sigval = 0;

    if (usev) {
        if ((uintptr_t)usev >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct k_sigevent sev;
        if (copy_from_user(&sev, usev, sizeof(sev)) != 0) return -(s64)EFAULT;
        notify = sev.sigev_notify;
        signo  = sev.sigev_signo;
        sigval = sev.sigev_value;
    }

    s64 id = ktimer_create(proc, clockid, notify, signo, sigval);
    if (id < 0) return id;

    /* timer_t is a pointer-sized opaque handle; the small integer id goes in. */
    s32 idv = (s32)id;
    if (copy_to_user(utimerid, &idv, sizeof(idv)) != 0) {
        ktimer_delete(proc, (int)id);
        return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_timer_settime_impl(pt_regs_t *r)
{
    int id     = (int)r->rdi;
    int flags  = (int)r->rsi;
    const struct itimerspec *unew = (const struct itimerspec *)r->rdx;
    struct itimerspec *uold       = (struct itimerspec *)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!unew || (uintptr_t)unew >= TASK_SIZE_MAX) return -(s64)EFAULT;

    struct itimerspec nv;
    if (copy_from_user(&nv, unew, sizeof(nv)) != 0) return -(s64)EFAULT;
    if (nv.it_value.tv_nsec < 0 || nv.it_value.tv_nsec >= 1000000000L ||
        nv.it_interval.tv_nsec < 0 || nv.it_interval.tv_nsec >= 1000000000L) {
        return -(s64)EINVAL;
    }

    u64 old_value = 0, old_interval = 0;
    s64 ret = ktimer_settime(proc, id, (flags & TIMER_ABSTIME) != 0,
                             timespec_to_ticks(&nv.it_value),
                             timespec_to_ticks(&nv.it_interval),
                             &old_value, &old_interval);
    if (ret < 0) return ret;

    if (uold && (uintptr_t)uold < TASK_SIZE_MAX) {
        struct itimerspec ov;
        ticks_to_timespec(old_value, &ov.it_value);
        ticks_to_timespec(old_interval, &ov.it_interval);
        if (copy_to_user(uold, &ov, sizeof(ov)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_timer_gettime_impl(pt_regs_t *r)
{
    int id = (int)r->rdi;
    struct itimerspec *ucur = (struct itimerspec *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!ucur || (uintptr_t)ucur >= TASK_SIZE_MAX) return -(s64)EFAULT;

    u64 value = 0, interval = 0;
    s64 ret = ktimer_gettime(proc, id, &value, &interval);
    if (ret < 0) return ret;

    struct itimerspec cur;
    ticks_to_timespec(value, &cur.it_value);
    ticks_to_timespec(interval, &cur.it_interval);
    return copy_to_user(ucur, &cur, sizeof(cur)) == 0 ? 0 : -(s64)EFAULT;
}

s64 sys_timer_getoverrun_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return ktimer_getoverrun(proc, (int)r->rdi);
}

s64 sys_timer_delete_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return ktimer_delete(proc, (int)r->rdi);
}

s64 sys_setitimer_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    const struct k_itimerval *unew = (const struct k_itimerval *)r->rsi;
    struct k_itimerval *uold       = (struct k_itimerval *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 value = 0, interval = 0;
    if (unew) {
        if ((uintptr_t)unew >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct k_itimerval nv;
        if (copy_from_user(&nv, unew, sizeof(nv)) != 0) return -(s64)EFAULT;
        if (nv.it_value.tv_usec < 0 || nv.it_value.tv_usec >= 1000000L ||
            nv.it_interval.tv_usec < 0 || nv.it_interval.tv_usec >= 1000000L) {
            return -(s64)EINVAL;
        }
        value    = timeval_to_ticks(&nv.it_value);
        interval = timeval_to_ticks(&nv.it_interval);
    }

    u64 old_value = 0, old_interval = 0;
    s64 ret = ktimer_setitimer(proc, which, value, interval, &old_value, &old_interval);
    if (ret < 0) return ret;

    if (uold && (uintptr_t)uold < TASK_SIZE_MAX) {
        struct k_itimerval ov;
        ticks_to_timeval(old_value, &ov.it_value);
        ticks_to_timeval(old_interval, &ov.it_interval);
        if (copy_to_user(uold, &ov, sizeof(ov)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_getitimer_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    struct k_itimerval *ucur = (struct k_itimerval *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!ucur || (uintptr_t)ucur >= TASK_SIZE_MAX) return -(s64)EFAULT;

    u64 value = 0, interval = 0;
    s64 ret = ktimer_getitimer(proc, which, &value, &interval);
    if (ret < 0) return ret;

    struct k_itimerval cur;
    ticks_to_timeval(value, &cur.it_value);
    ticks_to_timeval(interval, &cur.it_interval);
    return copy_to_user(ucur, &cur, sizeof(cur)) == 0 ? 0 : -(s64)EFAULT;
}

s64 sys_alarm_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return (s64)ktimer_alarm(proc, (u64)(u32)r->rdi * 100);
}
