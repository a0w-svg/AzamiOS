/* ============================================================================
 * AzamiOS — perf_event_open(2)
 * File: kernel/perf/perf.c
 *
 * See perf.h for the model and for what is deliberately not implemented.
 * ========================================================================= */

#define DEBUG 0
#include <azami/debug.h>

#include "perf.h"
#include "../uaccess.h"
#include "../sched/sched.h"
#include "../security/security.h"
#include "../mm/kmalloc.h"
#include "../lib/string.h"
#include "../../arch/x86_64/cpu/pmu.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../fs/vfs.h"
#include "../../include/azami/defs.h"
#include "../../drivers/char/console.h"
#include "../syscall/syscall.h"

/* The LAPIC preemption timer runs at 100 Hz (see lapic_timer_start in
 * ap_c_entry / kernel_main), so one scheduler tick is 10 ms. Every duration
 * perf reports is a multiple of this. */
#define PERF_TICK_NS  10000000ULL

#define PERF_MAX_EVENTS  64

/* poll(2) revents this fd can report. */
#define POLLIN      0x0001
#define POLLRDNORM  0x0040

typedef struct perf_event {
    struct perf_event *next;

    u32  type;
    u64  config;
    u64  read_format;
    u64  attr_flags;

    u32  target_pid;        /* 0 == system-wide                             */
    s32  cpu;               /* remembered, not enforced — see perf.h        */
    u32  owner_pid;

    int  slot;              /* PMU counter slot, or -1 for a software event */
    u64  evtsel;

    bool enabled;
    bool running;           /* hardware: the target is on a CPU right now   */

    u64  count;             /* accumulated hardware events                  */
    u64  sw_base;           /* software: counter value when last reset      */
    u64  prev[SMP_MAX_CPUS];/* last raw counter read, per core              */

    u64  id;
    u64  enabled_tick;      /* tick at which counting last started          */
    u64  time_enabled;      /* ticks, excluding the currently-open interval */
    u64  time_running;
} perf_event_t;

static spinlock_t    g_perf_lock = SPINLOCK_INIT;
static perf_event_t *g_perf_list;
static u32           g_perf_count;
static u64           g_perf_next_id = 1;

/* Fast-path gate for the context-switch hook: the number of events that need
 * per-switch hardware bookkeeping. Zero on any system that is not being
 * measured, which is nearly always. */
static volatile u32  g_perf_hw_events;

void perf_init(void)
{
    g_perf_list       = NULL;
    g_perf_count      = 0;
    g_perf_hw_events  = 0;
    char buf[64];
    pmu_format(buf, sizeof(buf));
    kprintf("[PERF] perf_event_open ready (pmu: %s)\n", buf);
}

/* ── The context-switch hook ─────────────────────────────────────────────── */

void perf_sched_switch(process_t *prev, process_t *next)
{
    u32 cpu = smp_current_cpu_id();

    /* Maintained unconditionally: these are the source for the software
     * events, and the numbers /proc/<pid>/stat reports. */
    if (next) {
        next->nr_ctx_switches++;
        if (next->last_cpu != (u32)-1 && next->last_cpu != cpu)
            next->nr_migrations++;
        next->last_cpu = cpu;
    }

    if (!__atomic_load_n(&g_perf_hw_events, __ATOMIC_RELAXED)) return;
    if (!g_pmu.present) return;

    /* A slot may have been claimed or released since this core last looked. */
    pmu_sync_local();

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->slot < 0 || !e->enabled) continue;

        u64 now = pmu_slot_read(e->slot);

        if (e->target_pid == 0) {
            /* System-wide: every core folds in the events it saw since its own
             * previous switch, so the total is a genuine sum across cores. */
            e->count += (now - e->prev[cpu]) & g_pmu.mask;
            e->prev[cpu] = now;
            continue;
        }

        if (prev && prev->pid == e->target_pid && e->running) {
            e->count += (now - e->prev[cpu]) & g_pmu.mask;
            e->running = false;
        }
        if (next && next->pid == e->target_pid) {
            e->prev[cpu] = now;
            e->running = true;
        }
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

void perf_process_exit(process_t *p)
{
    if (!p) return;
    if (!__atomic_load_n(&g_perf_count, __ATOMIC_RELAXED)) return;

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->target_pid != p->pid) continue;
        /* The count is already final: the context switch that took the dying
         * process off its CPU folded in the last delta, on that CPU, where the
         * subtraction was still meaningful. Doing it again here — from the
         * reaper, on whatever CPU it happens to be running — would subtract
         * this core's counter from a value read on another and add garbage.
         * So only stop following the pid, which must happen before the number
         * can be reused by a new process. */
        e->running    = false;
        e->enabled    = false;
        e->target_pid = (u32)-1;    /* matches no live process */
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

/* ── Reading a count ─────────────────────────────────────────────────────── */

/* Value of the process_t counter a software event is a difference of. */
static u64 sw_source(const perf_event_t *e, const process_t *t)
{
    switch (e->config) {
    case PERF_COUNT_SW_CPU_CLOCK:
        return sched_get_ticks();
    case PERF_COUNT_SW_TASK_CLOCK:
        return t ? (t->utime_ticks + t->stime_ticks) : 0;
    case PERF_COUNT_SW_PAGE_FAULTS:
        return t ? (t->nr_minor_faults + t->nr_major_faults) : 0;
    case PERF_COUNT_SW_PAGE_FAULTS_MIN:
        return t ? t->nr_minor_faults : 0;
    case PERF_COUNT_SW_PAGE_FAULTS_MAJ:
        return t ? t->nr_major_faults : 0;
    case PERF_COUNT_SW_CONTEXT_SWITCHES:
        return t ? t->nr_ctx_switches : 0;
    case PERF_COUNT_SW_CPU_MIGRATIONS:
        return t ? t->nr_migrations : 0;
    default:
        /* alignment-faults and emulation-faults cannot happen on this kernel:
         * #AC from ring 3 is fatal and nothing is emulated. Reporting a
         * constant zero is the truth, not a stub. */
        return 0;
    }
}

/* The value sw_source() would return for a (type, config) pair against @t,
 * without an event struct. perf_event_open() uses it to take the software
 * baseline while it still holds a reference to the target, rather than keeping
 * a bare process pointer alive across the rest of the call. */
static u64 sw_seed_for(u32 type, u64 config, const process_t *t)
{
    if (type != PERF_TYPE_SOFTWARE) return 0;
    perf_event_t tmp;
    __builtin_memset(&tmp, 0, sizeof(tmp));
    tmp.config = config;
    return sw_source(&tmp, t);
}

/* Caller holds g_perf_lock. */
static u64 event_value_locked(perf_event_t *e)
{
    if (e->slot >= 0) {
        u64 v = e->count;
        /* Fold in the interval that is open right now, so a read from inside
         * the measured process sees its own cycles rather than only those up
         * to the last context switch. */
        if (e->running && e->enabled) {
            process_t *cur = sched_current_process();
            if (cur && cur->pid == e->target_pid) {
                u32 cpu = smp_current_cpu_id();
                v += (pmu_slot_read(e->slot) - e->prev[cpu]) & g_pmu.mask;
            }
        }
        return v;
    }

    process_t *t = e->target_pid && e->target_pid != (u32)-1
                   ? proc_get_by_pid(e->target_pid) : NULL;
    u64 raw = sw_source(e, t);
    if (t) proc_put(t);
    u64 delta = raw >= e->sw_base ? raw - e->sw_base : 0;

    /* The two clock events are tick counts; every other software event is
     * already a plain occurrence count. */
    if (e->config == PERF_COUNT_SW_CPU_CLOCK || e->config == PERF_COUNT_SW_TASK_CLOCK)
        delta *= PERF_TICK_NS;
    return e->count + delta;
}

static s64 perf_read_op(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    perf_event_t *e = (perf_event_t *)filp->private_data;
    if (!e) return -(s64)EBADF;
    if (!buf) return -(s64)EFAULT;

    u64 out[4];
    u32 n = 0;

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    out[n++] = event_value_locked(e);

    u64 ticks = sched_get_ticks();
    u64 open_interval = (e->enabled && ticks > e->enabled_tick)
                        ? (ticks - e->enabled_tick) : 0;
    if (e->read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
        out[n++] = (e->time_enabled + open_interval) * PERF_TICK_NS;
    if (e->read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
        out[n++] = (e->time_running + open_interval) * PERF_TICK_NS;
    if (e->read_format & PERF_FORMAT_ID)
        out[n++] = e->id;
    spinlock_unlock_irqrestore(&g_perf_lock, fl);

    size_t need = (size_t)n * sizeof(u64);
    /* Linux returns -ENOSPC for a buffer too small to hold the whole record;
     * a short read would silently truncate a multi-word format. */
    if (len < need) return -(s64)ENOSPC;

    memcpy(buf, out, need);
    return (s64)need;
}

static s64 perf_ioctl_op(file_t *filp, u32 cmd, u64 arg)
{
    (void)arg;
    perf_event_t *e = (perf_event_t *)filp->private_data;
    if (!e) return -(s64)EBADF;

    /* Accept both the bare request number and the full _IO('$', n) encoding
     * glibc's <linux/perf_event.h> produces. */
    u32 nr = cmd & 0xFF;
    u32 magic = (cmd >> 8) & 0xFF;
    if (cmd > 0xFF && magic != (u32)PERF_IOC_MAGIC) return -(s64)ENOTTY;

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    u64 ticks = sched_get_ticks();
    s64 rc = 0;

    switch (nr) {
    case PERF_IOC_ENABLE:
        if (!e->enabled) {
            e->enabled = true;
            e->enabled_tick = ticks;
            /* Re-open the hardware interval from wherever the counter is now,
             * so the time the event spent disabled is not credited to it. */
            if (e->slot >= 0) {
                u32 cpu = smp_current_cpu_id();
                e->prev[cpu] = pmu_slot_read(e->slot);
                process_t *cur = sched_current_process();
                e->running = (e->target_pid == 0) ||
                             (cur && cur->pid == e->target_pid);
            }
        }
        break;

    case PERF_IOC_DISABLE:
        if (e->enabled) {
            if (e->slot >= 0 && e->running) {
                process_t *cur = sched_current_process();
                if (e->target_pid == 0 || (cur && cur->pid == e->target_pid)) {
                    u32 cpu = smp_current_cpu_id();
                    e->count += (pmu_slot_read(e->slot) - e->prev[cpu]) & g_pmu.mask;
                }
                e->running = false;
            }
            if (ticks > e->enabled_tick) {
                e->time_enabled += ticks - e->enabled_tick;
                e->time_running += ticks - e->enabled_tick;
            }
            e->enabled = false;
        }
        break;

    case PERF_IOC_RESET:
        e->count = 0;
        if (e->slot >= 0) {
            u32 cpu = smp_current_cpu_id();
            e->prev[cpu] = pmu_slot_read(e->slot);
        } else {
            process_t *t = e->target_pid && e->target_pid != (u32)-1
                           ? proc_get_by_pid(e->target_pid) : NULL;
            e->sw_base = sw_source(e, t);
            if (t) proc_put(t);
        }
        e->time_enabled = 0;
        e->time_running = 0;
        e->enabled_tick = ticks;
        break;

    case PERF_IOC_ID:
        spinlock_unlock_irqrestore(&g_perf_lock, fl);
        if (!arg || arg >= 0x0000800000000000ULL) return -(s64)EFAULT;
        if (copy_to_user((void *)(uintptr_t)arg, &e->id, sizeof(u64)) != 0)
            return -(s64)EFAULT;
        return 0;

    case PERF_IOC_REFRESH:
    case PERF_IOC_PERIOD:
        /* Both only mean anything for a sampling event, and there are none. */
        rc = -(s64)EINVAL;
        break;

    default:
        rc = -(s64)ENOTTY;
        break;
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
    return rc;
}

static int perf_poll_op(file_t *filp)
{
    (void)filp;
    /* A perf fd only becomes readable through sampling, which this kernel does
     * not do; read(2) always succeeds immediately, so report readable and never
     * leave a poller waiting for an event that cannot arrive. */
    return POLLIN | POLLRDNORM;
}

static s64 perf_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    perf_event_t *e = filp ? (perf_event_t *)filp->private_data : NULL;
    if (!e) return 0;
    filp->private_data = NULL;

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    perf_event_t **pp = &g_perf_list;
    while (*pp) {
        if (*pp == e) { *pp = e->next; break; }
        pp = &(*pp)->next;
    }
    if (g_perf_count) g_perf_count--;
    if (e->slot >= 0 && g_perf_hw_events) g_perf_hw_events--;
    spinlock_unlock_irqrestore(&g_perf_lock, fl);

    if (e->slot >= 0) pmu_slot_release(e->slot);
    kfree(e);
    return 0;
}

static file_operations_t g_perf_fops = {
    .read    = perf_read_op,
    .ioctl   = perf_ioctl_op,
    .poll    = perf_poll_op,
    .release = perf_release_op,
};

/* ── perf_event_open(2) ──────────────────────────────────────────────────── */

/* Copy the caller's attr, honouring its self-declared size. Every version of
 * the struct is a prefix of the next, so a short one zero-extends; a longer one
 * from a newer libc is only accepted if the part we do not understand is zero,
 * which is what stops a flag we would silently ignore from being set. */
static s64 copy_attr(struct perf_event_attr *out, const void *uattr)
{
    u8 raw[PERF_ATTR_SIZE_MAX];
    memset(raw, 0, sizeof(raw));

    u32 size = 0;
    if (copy_from_user(&size, (const u8 *)uattr + 4, sizeof(size)) != 0)
        return -(s64)EFAULT;
    if (size == 0) size = PERF_ATTR_SIZE_VER0;
    if (size < PERF_ATTR_SIZE_VER0) return -(s64)EINVAL;

    u32 take = size > PERF_ATTR_SIZE_MAX ? PERF_ATTR_SIZE_MAX : size;
    if (copy_from_user(raw, uattr, take) != 0) return -(s64)EFAULT;

    if (size > PERF_ATTR_SIZE_MAX) {
        /* Verify the tail we cannot interpret is all zero, a byte at a time —
         * E2BIG is exactly what Linux returns here, and libc retries smaller. */
        for (u32 off = PERF_ATTR_SIZE_MAX; off < size; off++) {
            u8 b;
            if (copy_from_user(&b, (const u8 *)uattr + off, 1) != 0)
                return -(s64)EFAULT;
            if (b) return -(s64)E2BIG;
        }
    }

    memcpy(out, raw, sizeof(*out));
    out->size = size;
    return 0;
}

s64 sys_perf_event_open_impl(pt_regs_t *r)
{
    const void *uattr = (const void *)r->rdi;
    s32   pid       = (s32)r->rsi;
    s32   cpu       = (s32)r->rdx;
    s32   group_fd  = (s32)r->r10;
    u64   flags     = r->r8;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;
    if (!uattr || (uintptr_t)uattr >= 0x0000800000000000ULL) return -(s64)EFAULT;
    if (flags & ~(u64)(PERF_FLAG_FD_NO_GROUP | PERF_FLAG_FD_OUTPUT |
                       PERF_FLAG_FD_CLOEXEC))
        return -(s64)EINVAL;
    if (flags & PERF_FLAG_PID_CGROUP) return -(s64)EINVAL;

    struct perf_event_attr attr;
    s64 rc = copy_attr(&attr, uattr);
    if (rc < 0) return rc;

    /* No ring buffer, so no sampling. Say so rather than accept an event that
     * would never deliver a record. */
    if (attr.sample_period != 0) return -(s64)EOPNOTSUPP;
    if (attr.sample_type != 0)   return -(s64)EOPNOTSUPP;
    if (attr.read_format & PERF_FORMAT_GROUP) return -(s64)EOPNOTSUPP;
    if (attr.read_format & ~(u64)(PERF_FORMAT_TOTAL_TIME_ENABLED |
                                  PERF_FORMAT_TOTAL_TIME_RUNNING |
                                  PERF_FORMAT_ID))
        return -(s64)EINVAL;

    /* Groups are not scheduled as a unit here; every event is independent, so
     * a leader fd is accepted only in its "no group" form. */
    if (group_fd >= 0 && !(flags & PERF_FLAG_FD_NO_GROUP))
        return -(s64)EOPNOTSUPP;

    /* ── Target and permission ──────────────────────────────────────────── */
    /* The target process is only needed here (credential check) and for a
     * one-time software-counter baseline. Both are taken now, while a counted
     * reference keeps it alive; nothing downstream keeps the pointer, so it can
     * never be dereferenced after the process is reaped on another CPU. */
    u32  target_pid;
    bool target_is_self = false;
    u64  sw_seed        = 0;
    if (pid == -1) {
        /* System-wide measurement sees every process on the machine, so it is
         * gated the way Linux gates perf_event_paranoid > 0. */
        if (!security_check_permission(me, CAP_PERFMON) &&
            !security_check_permission(me, CAP_SYS_ADMIN) && me->euid != 0)
            return -(s64)EACCES;
        if (cpu < 0) return -(s64)EINVAL;   /* Linux requires a CPU here */
        target_pid = 0;
        sw_seed    = sw_seed_for(attr.type, attr.config, NULL);
    } else if (pid == 0) {
        target_pid     = me->pid;
        target_is_self = true;
        sw_seed        = sw_seed_for(attr.type, attr.config, me);
    } else if (pid > 0) {
        process_t *t = proc_get_by_pid((u32)pid);
        if (!t) return -(s64)ESRCH;
        /* Same credential rule as ptrace(2) and process_vm_readv(2): counting
         * another process's instructions is an observation channel into it. */
        bool allowed = security_check_permission(me, CAP_PERFMON) ||
                       me->euid == 0 ||
                       (me->uid == t->uid && me->uid == t->euid &&
                        me->uid == t->suid);
        target_pid     = t->pid;
        target_is_self = (t == me);
        sw_seed        = sw_seed_for(attr.type, attr.config, t);
        proc_put(t);
        if (!allowed) return -(s64)EACCES;
    } else {
        return -(s64)EINVAL;
    }

    /* ── Resolve the event ──────────────────────────────────────────────── */
    int slot = -1;
    u64 evtsel = 0;

    switch (attr.type) {
    case PERF_TYPE_HARDWARE:
        if (attr.config >= PERF_COUNT_HW_MAX) return -(s64)EINVAL;
        if (!g_pmu.present) return -(s64)ENOENT;
        evtsel = pmu_hw_event(attr.config);
        if (!evtsel) return -(s64)ENOENT;   /* this CPU cannot count it */
        break;

    case PERF_TYPE_RAW:
        if (!g_pmu.present) return -(s64)ENOENT;
        if (attr.config & ~PMU_EVTSEL_RAW_MASK) return -(s64)EINVAL;
        if (!attr.config) return -(s64)EINVAL;
        evtsel = attr.config;
        break;

    case PERF_TYPE_SOFTWARE:
        if (attr.config >= PERF_COUNT_SW_MAX) return -(s64)EINVAL;
        /* Everything but the wall clock is a per-process counter, and there is
         * no machine-wide total to subtract from for a system-wide event. */
        if (target_pid == 0 && attr.config != PERF_COUNT_SW_CPU_CLOCK &&
            attr.config != PERF_COUNT_SW_DUMMY)
            return -(s64)EOPNOTSUPP;
        break;

    default:
        /* Tracepoints need a trace event registry, hw-cache needs a per-model
         * table, and breakpoints need the debug registers this kernel does not
         * context-switch. None of them are half-implemented. */
        return -(s64)EOPNOTSUPP;
    }

    if (evtsel) {
        /* exclude_user/exclude_kernel are the counter's own ring filter, so
         * they cost nothing and are exact. Default (neither set) counts both. */
        if (!(attr.flags & PERF_ATTR_EXCLUDE_USER))   evtsel |= PMU_EVTSEL_USR;
        if (!(attr.flags & PERF_ATTR_EXCLUDE_KERNEL)) evtsel |= PMU_EVTSEL_OS;
        if (!(evtsel & (PMU_EVTSEL_USR | PMU_EVTSEL_OS))) return -(s64)EINVAL;
        evtsel |= PMU_EVTSEL_EN;

        slot = pmu_slot_acquire(evtsel);
        if (slot < 0) return -(s64)ENOSPC;   /* every counter is taken */
    }

    /* ── Build the event ────────────────────────────────────────────────── */
    perf_event_t *e = (perf_event_t *)kzalloc(sizeof(perf_event_t));
    if (!e) { if (slot >= 0) pmu_slot_release(slot); return -(s64)ENOMEM; }

    e->type        = attr.type;
    e->config      = attr.config;
    e->read_format = attr.read_format;
    e->attr_flags  = attr.flags;
    e->target_pid  = target_pid;
    e->cpu         = cpu;
    e->owner_pid   = me->pid;
    e->slot        = slot;
    e->evtsel      = evtsel;
    e->enabled     = !(attr.flags & PERF_ATTR_DISABLED);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(e);
        if (slot >= 0) pmu_slot_release(slot);
        return -(s64)ENOMEM;
    }
    f->f_op         = &g_perf_fops;
    f->private_data = e;
    f->f_mode       = 0600;
    f->f_count      = 1;
    f->f_fd_flags   = (flags & PERF_FLAG_FD_CLOEXEC) ? FD_CLOEXEC : 0;

    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    if (g_perf_count >= PERF_MAX_EVENTS) {
        spinlock_unlock_irqrestore(&g_perf_lock, fl);
        kfree(f);
        kfree(e);
        if (slot >= 0) pmu_slot_release(slot);
        return -(s64)ENOSPC;
    }
    e->id = g_perf_next_id++;
    e->enabled_tick = sched_get_ticks();
    if (slot >= 0) {
        u64 now = pmu_slot_read(slot);
        for (u32 i = 0; i < SMP_MAX_CPUS; i++) e->prev[i] = now;
        e->running = e->enabled && (target_pid == 0 || target_is_self);
        g_perf_hw_events++;
    } else {
        e->sw_base = sw_seed;
    }
    e->next = g_perf_list;
    g_perf_list = e;
    g_perf_count++;
    spinlock_unlock_irqrestore(&g_perf_lock, fl);

    s64 fd = syscall_install_fd(me, f, f->f_fd_flags);
    if (fd < 0) {
        perf_release_op(NULL, f);
        kfree(f);
        return fd;
    }
    return fd;
}
