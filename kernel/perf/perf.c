/* Counting-only perf ABI. See perf.h for the supported scope. */
#define DEBUG 0
#include <azami/debug.h>
#include "perf.h"
#include "../uaccess.h"
#include "../security/security.h"
#include "../mm/kmalloc.h"
#include "../lib/string.h"
#include "../time/timekeeping.h"
#include "../../arch/x86_64/cpu/pmu.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../fs/vfs.h"
#include "../../include/azami/defs.h"
#include "../../drivers/char/console.h"
#include "../syscall/syscall.h"

#define PERF_MAX_EVENTS 64
#define PERF_NO_CPU ((u32)-1)
#define PERF_POLLHUP 0x0010

typedef struct perf_event {
    struct perf_event *next;
    u64 config, read_format, attr_flags, id;
    u32 target_tid, target_pid, running_cpu, last_cpu;
    s32 cpu;
    int slot;
    bool enabled, dead;
    u64 count, prev, running_ns;
    u64 time_enabled, time_running;
} perf_event_t;

static spinlock_t g_perf_lock = SPINLOCK_INIT;
static perf_event_t *g_perf_list;
static u32 g_perf_count;
static u64 g_perf_next_id = 1;
/* Atomic sysctl access: readers do not need the event-table lock. */
s32 g_perf_event_paranoid = 2;

void perf_init(void)
{
    char buf[64];
    pmu_format(buf, sizeof(buf));
    kprintf("[PERF] scoped counting ready (pmu: %s)\n", buf);
}

static bool targets_thread(const perf_event_t *e, const thread_t *t)
{
    return t && t->proc && t->tid == e->target_tid && t->proc->pid == e->target_pid;
}

static bool eligible(const perf_event_t *e)
{
    return e->running_cpu != PERF_NO_CPU &&
           (e->cpu < 0 || e->running_cpu == (u32)e->cpu);
}
static bool system_event(const perf_event_t *e) { return e->target_tid == 0; }

static bool clock_event(const perf_event_t *e)
{
    return e->slot < 0 && (e->config == PERF_COUNT_SW_CPU_CLOCK ||
                           e->config == PERF_COUNT_SW_TASK_CLOCK);
}

/* Fold only on the CPU that owns the open interval. Callers hold g_perf_lock.
 * Software timestamps can be folded remotely; a raw PMU read never can. */
static void fold_running(perf_event_t *e, u64 now)
{
    if (!e->enabled || e->running_cpu == PERF_NO_CPU) return;
    u64 duration = now >= e->running_ns ? now - e->running_ns : 0;
    e->time_enabled += duration;
    if (eligible(e)) e->time_running += duration;
    if (clock_event(e) && eligible(e)) e->count += duration;
    if (e->slot >= 0 && eligible(e)) {
        u64 raw = pmu_slot_read(e->slot);
        e->count += pmu_counter_delta(raw, e->prev, g_pmu.mask);
        e->prev = raw;
    }
    e->running_ns = now;
}

void perf_sched_switch(thread_t *prev, thread_t *next)
{
    u32 cpu = smp_current_cpu_id();
    if (next && next->proc) {
        __atomic_add_fetch(&next->proc->nr_ctx_switches, 1, __ATOMIC_RELAXED);
        if (next->proc->last_cpu != (u32)-1 && next->proc->last_cpu != cpu)
            __atomic_add_fetch(&next->proc->nr_migrations, 1, __ATOMIC_RELAXED);
        next->proc->last_cpu = cpu;
    }
    /* Also disarm released slots when the last event has disappeared. */
    pmu_sync_local();
    u32 last = next ? __atomic_exchange_n(&next->perf_last_cpu,cpu,__ATOMIC_RELAXED) : PERF_NO_CPU;
    if (!__atomic_load_n(&g_perf_count, __ATOMIC_RELAXED)) return;
    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    u64 now = ktime_get_ns();
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->dead) continue;
        if (system_event(e)) {
            if (e->cpu != (s32)cpu) continue;
            fold_running(e, now);
            if (e->enabled && e->slot < 0 && !(e->attr_flags & PERF_ATTR_EXCLUDE_KERNEL)) {
                if (e->config == PERF_COUNT_SW_CONTEXT_SWITCHES && prev != next) e->count++;
                if (e->config == PERF_COUNT_SW_CPU_MIGRATIONS && next &&
                    last != PERF_NO_CPU && last != cpu) e->count++;
            }
            continue;
        }
        if (targets_thread(e, prev) && e->running_cpu == cpu) {
            fold_running(e, now);
            if (e->enabled && e->slot < 0 &&
                e->config == PERF_COUNT_SW_CONTEXT_SWITCHES && prev != next && eligible(e) &&
                !(e->attr_flags & PERF_ATTR_EXCLUDE_KERNEL))
                e->count++;
            e->running_cpu = PERF_NO_CPU;
        }
        if (targets_thread(e, next)) {
            if (e->enabled && e->slot < 0 &&
                e->config == PERF_COUNT_SW_CPU_MIGRATIONS && e->last_cpu != PERF_NO_CPU &&
                e->last_cpu != cpu && (e->cpu < 0 || e->cpu == (s32)cpu) &&
                !(e->attr_flags & PERF_ATTR_EXCLUDE_KERNEL))
                e->count++;
            e->last_cpu = e->running_cpu = cpu;
            e->running_ns = now;
            if (e->slot >= 0 && eligible(e)) e->prev = pmu_slot_read(e->slot);
        }
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

void perf_page_fault(bool user)
{
    if (!__atomic_load_n(&g_perf_count, __ATOMIC_RELAXED)) return;
    thread_t *t = sched_current_thread();
    if (!t) return;
    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->dead || !e->enabled || e->slot >= 0 ||
            (!system_event(e) && !targets_thread(e, t)) ||
            (e->cpu >= 0 && e->cpu != (s32)smp_current_cpu_id())) continue;
        if ((user && (e->attr_flags & PERF_ATTR_EXCLUDE_USER)) ||
            (!user && (e->attr_flags & PERF_ATTR_EXCLUDE_KERNEL))) continue;
        if (e->config == PERF_COUNT_SW_PAGE_FAULTS ||
            e->config == PERF_COUNT_SW_PAGE_FAULTS_MIN) e->count++;
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

static void stop_event(perf_event_t *e, u64 now)
{
    if (e->enabled) {
        fold_running(e, now);
        e->enabled = false;
    }
}

/* Called only after a retiring thread is off its CPU (or on its own CPU).
 * No process/thread pointer survives in an event or crosses a lock boundary. */
void perf_thread_exit(thread_t *t)
{
    if (!t || !__atomic_load_n(&g_perf_count, __ATOMIC_RELAXED)) return;
    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    u64 now = ktime_get_ns();
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->dead || !targets_thread(e, t)) continue;
        stop_event(e, now);
        e->running_cpu = PERF_NO_CPU;
        e->dead = true;
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

void perf_process_exit(process_t *p)
{
    if (!p || !__atomic_load_n(&g_perf_count, __ATOMIC_RELAXED)) return;
    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    u64 now = ktime_get_ns();
    for (perf_event_t *e = g_perf_list; e; e = e->next) {
        if (e->dead || e->target_pid != p->pid) continue;
        stop_event(e, now);
        e->running_cpu = PERF_NO_CPU;
        e->dead = true;
    }
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
}

enum { PERF_OP_READ=0, PERF_OP_ENABLE=1, PERF_OP_DISABLE=2, PERF_OP_RESET=3 };
#define PERF_RPC_TIMEOUT_NS 250000000ULL
/* No pointers travel through a remote request. Event IDs never get reused. */
typedef struct { u64 id, deadline, out[4]; s32 op, rc, cpu; u32 words; } perf_request_t;

/* Caller holds the event lock and has disabled interrupts. No blocking locks
 * may be acquired here: this also runs from the bounded CPU-call interrupt. */
static void execute_request(perf_request_t *r)
{
    perf_event_t *e=g_perf_list;
    while(e && e->id!=r->id) e=e->next;
    if(!e) { r->rc=-EBADF; return; }
    u64 now;
    if(!ktime_try_get_ns(&now)) { r->rc=-EAGAIN; return; }
    if(now>=r->deadline) { r->rc=-ETIMEDOUT; return; }
    /* CPU-wide software events also execute on their CPU, giving the software
     * suite real cross-CPU delivery/timeout coverage without a usable PMU. */
    bool raw_needed=e->slot>=0 && eligible(e) &&
                    (e->enabled || (r->op==PERF_OP_ENABLE && !e->dead));
    u32 cpu=system_event(e) ? (u32)e->cpu : e->running_cpu;
    if((system_event(e) || raw_needed) && cpu!=smp_current_cpu_id()) {
        r->cpu=(s32)cpu; r->rc=-EAGAIN; return;
    }
    if(raw_needed && !pmu_try_sync_local()) { r->rc=-EAGAIN; return; }
    if(r->op==PERF_OP_ENABLE) {
        if(!e->enabled && !e->dead) {
            e->enabled=true;
            e->running_ns=now;
            if(e->slot>=0 && eligible(e)) e->prev=pmu_slot_read(e->slot);
        }
    } else if(r->op==PERF_OP_DISABLE) stop_event(e,now);
    else {
        fold_running(e,now);
        if(r->op==PERF_OP_RESET) e->count=0;
    }
    if(r->op==PERF_OP_READ) {
        u32 n=0;
        r->out[n++]=e->count;
        if(e->read_format & PERF_FORMAT_TOTAL_TIME_ENABLED) r->out[n++]=e->time_enabled;
        if(e->read_format & PERF_FORMAT_TOTAL_TIME_RUNNING) r->out[n++]=e->time_running;
        if(e->read_format & PERF_FORMAT_ID) r->out[n++]=e->id;
        r->words=n;
    }
    r->rc=0;
}
static void remote_request(void *value)
{
    perf_request_t *r=value;
    if(!spinlock_try_lock(&g_perf_lock)) { r->rc=-EAGAIN; return; }
    execute_request(r);
    spinlock_unlock(&g_perf_lock);
}
static s64 perform_request(u64 id, s32 op, perf_request_t *result)
{
    perf_request_t r={.id=id,.op=op};
    for(u32 attempts=0; attempts<100000; attempts++) {
        u64 now;
        if(!ktime_try_get_ns(&now)) { cpu_pause(); continue; }
        if(!r.deadline) r.deadline=now+PERF_RPC_TIMEOUT_NS;
        if(now>=r.deadline) return -(s64)ETIMEDOUT;
        r.cpu=-1; r.rc=-EAGAIN;
        irqflags_t fl=spinlock_irq_save();
        remote_request(&r);
        spinlock_irq_restore(fl);
        if(r.rc==-EAGAIN && r.cpu>=0) {
            int rc=smp_rpc_call((u32)r.cpu,remote_request,&r,sizeof(r),r.deadline);
            if(rc && rc!=-EBUSY && rc!=-EAGAIN) return rc;
            if(rc) r.rc=-EAGAIN;
        }
        if(r.rc!=-EAGAIN) { if(result) *result=r; return r.rc; }
        cpu_pause(); /* migration or bounded lock/slot contention: retry */
    }
    return -(s64)ETIMEDOUT;
}

static s64 perf_read_op(file_t *f, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    perf_event_t *e=f->private_data;
    if(!e) return -(s64)EBADF;
    perf_request_t r;
    s64 rc=perform_request(e->id,PERF_OP_READ,&r);
    if(rc) return rc;
    size_t need=r.words*sizeof(u64);
    if(len<need) return -(s64)ENOSPC;
    memcpy(buf,r.out,need);
    return (s64)need;
}
static s64 perf_ioctl_op(file_t *f, u32 cmd, u64 arg)
{
    perf_event_t *e=f->private_data;
    if(!e) return -(s64)EBADF;
    if(cmd==0x80082407U) {
        u64 id=e->id;
        return copy_to_user((void *)(uintptr_t)arg,&id,sizeof(id)) ? -(s64)EFAULT : 0;
    }
    if(cmd!=0x2400U && cmd!=0x2401U && cmd!=0x2403U)
        return (cmd==0x2402U || cmd==0x40082404U) ? -(s64)EINVAL : -(s64)ENOTTY;
    if(arg&~1ULL) return -(s64)EINVAL;
    s32 op=cmd==0x2400U ? PERF_OP_ENABLE : cmd==0x2401U ? PERF_OP_DISABLE : PERF_OP_RESET;
    return perform_request(e->id,op,NULL);
}

static int perf_poll_op(file_t *f)
{
    (void)f;
    /* Linux reports HUP when an event has no sampling ring buffer, including
     * a live counting-only event. read(2) still returns the current count. */
    return PERF_POLLHUP;
}

static s64 perf_release_op(inode_t *inode, file_t *f)
{
    (void)inode;
    perf_event_t *e = f ? f->private_data : NULL;
    if (!e) return 0;
    irqflags_t fl = spinlock_lock_irqsave(&g_perf_lock);
    perf_event_t **pp = &g_perf_list;
    while (*pp && *pp != e) pp = &(*pp)->next;
    if (*pp) {
        *pp = e->next;
        __atomic_sub_fetch(&g_perf_count, 1, __ATOMIC_RELAXED);
    }
    f->private_data = NULL;
    spinlock_unlock_irqrestore(&g_perf_lock, fl);
    if (e->slot >= 0) pmu_slot_release(e->slot);
    kfree(e);
    return 0;
}

static file_operations_t g_perf_fops = {
    .read = perf_read_op, .ioctl = perf_ioctl_op,
    .poll = perf_poll_op, .release = perf_release_op,
};

static s64 attr_too_big(void *uattr)
{
    u32 supported = PERF_ATTR_SIZE_MAX;
    if (copy_to_user((u8 *)uattr + 4, &supported, sizeof(supported)))
        return -(s64)EFAULT;
    return -(s64)E2BIG;
}

static s64 copy_attr(struct perf_event_attr *out, void *uattr)
{
    u8 raw[4096];
    u32 size;
    if (copy_from_user(&size, (u8 *)uattr + 4, sizeof(size))) return -(s64)EFAULT;
    if (!size) size = PERF_ATTR_SIZE_VER0;
    if (size < PERF_ATTR_SIZE_VER0 || size > sizeof(raw)) return attr_too_big(uattr);
    memset(raw, 0, sizeof(raw));
    if (copy_from_user(raw, uattr, size)) return -(s64)EFAULT;
    for (u32 i = PERF_ATTR_SIZE_MAX; i < size; i++)
        if (raw[i]) return attr_too_big(uattr);
    /* Fields after flags belong to sampling, breakpoints, or newer features.
     * Do not silently ignore a nonzero field even within a known attr size. */
    for (u32 i = 48; i < size && i < PERF_ATTR_SIZE_MAX; i++)
        if (raw[i]) return -(s64)EOPNOTSUPP;
    memcpy(out, raw, sizeof(*out));
    out->size = size;
    return 0;
}

/* Linux READ_REALCREDS fallback, without Yama's ATTACH-only restriction.
 * CAP_SYS_PTRACE permits observing another credential set, but does not bypass
 * perf paranoid policy. CAP_PERFMON/CAP_SYS_ADMIN bypass both scope checks. */
static bool can_observe(process_t *me, process_t *target, bool privileged)
{
    if(privileged || me==target) return true;
    if(security_check_permission(me,CAP_SYS_PTRACE)) return true;
    return me->uid==target->uid && me->uid==target->euid && me->uid==target->suid &&
           me->gid==target->gid && me->gid==target->egid && me->gid==target->sgid &&
           __atomic_load_n(&target->dumpable,__ATOMIC_ACQUIRE) &&
           !(target->cap_permitted & ~me->cap_permitted);
}

s64 sys_perf_event_open_impl(pt_regs_t *r)
{
    process_t *me = sched_current_process();
    thread_t *self = sched_current_thread();
    void *uattr = (void *)r->rdi;
    s32 pid = (s32)r->rsi, cpu = (s32)r->rdx, group = (s32)r->r10;
    u64 flags = r->r8;
    if (!me || !self) return -(s64)EPERM;
    if (flags & ~(u64)(PERF_FLAG_FD_NO_GROUP | PERF_FLAG_FD_OUTPUT |
                         PERF_FLAG_PID_CGROUP | PERF_FLAG_FD_CLOEXEC)) return -(s64)EINVAL;
    struct perf_event_attr a;
    s64 rc = copy_attr(&a, uattr);
    if (rc < 0) return rc;
    if (a.flags >> 38) return -(s64)EINVAL;
    /* AzamiOS has no guest execution domain. exclude_guest is therefore
     * satisfied for every event; exclude_host still remains unsupported.
     * IDENTIFIER only describes sample records and is inert at period zero. */
    u64 supported = PERF_ATTR_DISABLED | PERF_ATTR_EXCLUDE_USER |
                    PERF_ATTR_EXCLUDE_KERNEL | PERF_ATTR_EXCLUDE_HV |
                    PERF_ATTR_EXCLUDE_GUEST;
    if (a.flags & ~supported || a.sample_period || (a.sample_type & ~PERF_SAMPLE_IDENTIFIER) ||
        (flags & (PERF_FLAG_FD_OUTPUT | PERF_FLAG_PID_CGROUP))) return -(s64)EOPNOTSUPP;
    if (a.read_format & ~31ULL) return -(s64)EINVAL;
    if (a.read_format & (PERF_FORMAT_GROUP | 16)) return -(s64)EOPNOTSUPP;
    if (group < -1 || pid < -1 || cpu < -1) return -(s64)EINVAL;
    if (group >= 0) {
        file_t *leader = syscall_fget(me, group);
        if (!leader) return -(s64)EBADF;
        bool valid = leader->f_op == &g_perf_fops;
        syscall_fput(leader);
        if (!valid) return -(s64)EBADF;
        if (!(flags & PERF_FLAG_FD_NO_GROUP)) return -(s64)EOPNOTSUPP;
    }
    if (pid == -1 && cpu == -1) return -(s64)EINVAL;
    if (cpu >= 0 && ((u32)cpu >= SMP_MAX_CPUS || !(smp_online_mask() & (1ULL << cpu))))
        return -(s64)ENODEV;
    bool privileged = security_check_permission(me, CAP_PERFMON) ||
                      security_check_permission(me, CAP_SYS_ADMIN);
    s32 paranoid = __atomic_load_n(&g_perf_event_paranoid, __ATOMIC_RELAXED);
    if (!privileged && ((paranoid >= 2 && !(a.flags & PERF_ATTR_EXCLUDE_KERNEL)) ||
                        (paranoid >= 1 && pid == -1))) return -(s64)EACCES;
    u64 selector = 0;
    if (a.type == PERF_TYPE_SOFTWARE) {
        if (a.config >= PERF_COUNT_SW_MAX) return -(s64)EINVAL;
        /* No #AC/emulation event hooks exist; do not fabricate zero totals. */
        if (a.config == PERF_COUNT_SW_ALIGNMENT_FAULTS ||
            a.config == PERF_COUNT_SW_EMULATION_FAULTS) return -(s64)ENOENT;
    } else if (a.type == PERF_TYPE_HARDWARE || a.type == PERF_TYPE_RAW) {
        if (a.type == PERF_TYPE_HARDWARE && a.config >= PERF_COUNT_HW_MAX) return -(s64)EINVAL;
        if (a.type == PERF_TYPE_RAW && (!a.config || a.config & ~PMU_EVTSEL_RAW_MASK))
            return -(s64)EINVAL;
        if (!g_pmu.present) return -(s64)ENOENT;
        selector = a.type == PERF_TYPE_RAW ? a.config : pmu_hw_event(a.config);
        if (!selector) return -(s64)ENOENT;
        if (!(a.flags & PERF_ATTR_EXCLUDE_USER)) selector |= PMU_EVTSEL_USR;
        if (!(a.flags & PERF_ATTR_EXCLUDE_KERNEL)) selector |= PMU_EVTSEL_OS;
        if (!(selector & (PMU_EVTSEL_USR | PMU_EVTSEL_OS))) return -(s64)EINVAL;
        selector |= PMU_EVTSEL_EN;
    } else return -(s64)EOPNOTSUPP;

    perf_event_t *e = kzalloc(sizeof(*e));
    file_t *f = kzalloc(sizeof(*f));
    if (!e || !f) { kfree(e); kfree(f); return -(s64)ENOMEM; }
    f->f_op=&g_perf_fops;
    f->private_data=e;
    f->f_mode=0600;
    f->f_count=1;
    f->f_fd_flags=flags & PERF_FLAG_FD_CLOEXEC ? FD_CLOEXEC : 0;
    e->cpu=cpu;
    e->slot=-1;
    e->config=a.config;
    e->read_format=a.read_format;
    e->attr_flags=a.flags;
    e->running_cpu=e->last_cpu=PERF_NO_CPU;
    /* Lock order: scheduler -> perf -> PMU. Validate and publish
     * atomically against retirement; no thread pointer leaves this section. */
    sched_lock();
    thread_t *target=NULL;
    if(pid!=-1) {
        u32 tid=pid==0 ? self->tid : (u32)pid;
        for(process_t *p=sched_get_process_list(); p && !target; p=p->next)
            for(thread_t *t=p->threads; t; t=t->proc_next)
                if(t->tid==tid && t->state!=THREAD_ZOMBIE && t->state!=THREAD_DYING) { target=t; break; }
        if(!target) rc=-(s64)ESRCH;
        else {
            process_t *p=target->proc;
            if(!can_observe(me,p,privileged)) rc=-(s64)EACCES;
            else {
                e->target_tid=target->tid; e->target_pid=p->pid;
                e->last_cpu=__atomic_load_n(&target->perf_last_cpu,__ATOMIC_RELAXED);
                for(u32 c=0; c<SMP_MAX_CPUS; c++) {
                    cpu_info_t *info=smp_cpu_info(c);
                    if(info && info->current_thread==target) { e->running_cpu=c; break; }
                }
            }
        }
    } else e->running_cpu=(u32)cpu;
    if(rc<0) { sched_unlock(); kfree(e); kfree(f); return rc; }
    irqflags_t fl=spinlock_lock_irqsave(&g_perf_lock);
    e->slot=selector ? pmu_slot_acquire(selector) : -1;
    if((selector && e->slot<0) || g_perf_count>=PERF_MAX_EVENTS) {
        if(e->slot>=0) pmu_slot_release(e->slot);
        spinlock_unlock_irqrestore(&g_perf_lock,fl);
        sched_unlock(); kfree(e); kfree(f); return -(s64)ENOSPC;
    }
    e->running_ns=ktime_get_ns();
    e->id=g_perf_next_id++;
    e->next=g_perf_list; g_perf_list=e;
    __atomic_add_fetch(&g_perf_count,1,__ATOMIC_RELAXED);
    spinlock_unlock_irqrestore(&g_perf_lock,fl);
    sched_unlock();
    /* Initialize under our sole file reference before publishing a descriptor.
     * Callbacks resolve the internal ID; retirement may safely freeze it here. */
    if (!(a.flags & PERF_ATTR_DISABLED)) rc=perform_request(e->id,PERF_OP_ENABLE,NULL);
    s64 fd=rc<0 ? rc : syscall_install_fd(me,f,f->f_fd_flags);
    if(fd<0) syscall_fput(f);
    return fd;
}
