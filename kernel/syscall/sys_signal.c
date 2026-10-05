/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Signal Handling Syscalls
 * File: kernel/syscall/sys_signal.c
 * ============================================================================ */
#include "syscall_internal.h"


s64 sys_kill_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    int sig = (int)r->rsi;

    process_t *curr = sched_current_process();
    if (!curr) return -(s64)EPERM;

    /* Reject out-of-range signal numbers up front so no downstream path (group
     * / broadcast especially) hands sched_kill_process a bad sig. sig==0 stays
     * legal — it is the permission/existence probe. */
    if (sig != 0 && (sig < 0 || sig >= _NSIG)) return -(s64)EINVAL;

    if (pid > 0) {
        /* Positive pid: signal that specific process */
        if (pid == (s32)curr->pid) {
            if (sig == 0) return 0;
            if (sig < 0 || sig >= _NSIG) return -(s64)EINVAL;

            /* Job control cannot take the shortcut below. A stop is not a
             * termination, and SIGCONT has an effect whether or not a handler
             * is installed — neither is expressible as "queue it or die", so
             * both go to the common path even when the target is us. This is
             * the raise(SIGSTOP) a tracee does to hand control to its tracer,
             * and before the stop existed it killed the process outright. */
            if (sig == SIGSTOP || sig == SIGTSTP || sig == 21 /*SIGTTIN*/ ||
                sig == 22 /*SIGTTOU*/ || sig == SIGCONT)
                return sched_kill_process((u32)pid, sig);

            sighandler_t h = curr->sigactions[sig].sa_handler;
            if (h != SIG_DFL && h != SIG_IGN && sig != SIGKILL && sig != SIGSTOP) {
                /* Custom handler installed: queue it. signal_deliver_pending()
                 * runs it on the way back out of this syscall. */
                __atomic_or_fetch(&curr->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
                return 0;
            }
            if (h == SIG_IGN) return 0;
            /* SIG_DFL — default action. */
            if (sig == 17 /*SIGCHLD*/ || sig == 23 /*SIGURG*/ ||
                sig == 28 /*SIGWINCH*/ || sig == 18 /*SIGCONT*/)
                return 0;                       /* default: ignore */
            /* POSIX: a blocked signal becomes pending instead of acting. Its
             * default action is taken when the mask is lifted, or the signal
             * is consumed by sigwait()/sigtimedwait(). SIGKILL and SIGSTOP
             * cannot be blocked and are excluded above. */
            if (curr->sig_blocked & (1ULL << sig)) {
                __atomic_or_fetch(&curr->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
                return 0;
            }
            curr->term_signal = sig;
            curr->exit_code   = 128 + sig;
            sys_exit_impl(r);                    /* default: terminate */
            return 0;
        }
        return sched_kill_process_permitted(curr, (u32)pid, sig);
    }

    /* pid == 0    : every process in the caller's process group
     * pid == -1   : every process the caller may signal (all, here)
     * pid <  -1   : every process in process group |pid|            (POSIX-03) */
    u32 target_pgid = (pid == 0) ? curr->pgid : (u32)(-pid);
    s64 ret = -(s64)ESRCH;
    bool self_in_group = false;
    u32 pids[128];
    u32 npids = 0;

    sched_lock();
    for (process_t *p = sched_get_process_list(); p && npids < 128; p = p->next) {
        bool match = (pid == -1) ? true : (p->pgid == target_pgid);
        if (match) {
            if (p == curr) {
                self_in_group = true;   /* deliver to self last */
            } else {
                pids[npids++] = p->pid;
            }
        }
    }
    sched_unlock();

    for (u32 i = 0; i < npids; i++) {
        s64 r2 = sched_kill_process_permitted(curr, pids[i], sig);
        if (r2 == 0) ret = 0;
        else if (ret != 0 && r2 == -(s64)EPERM) ret = -(s64)EPERM;
    }
    if (self_in_group && sig != 0 && sig > 0 && sig < _NSIG) {
        ret = 0;
        sighandler_t h = curr->sigactions[sig].sa_handler;
        if (h != SIG_DFL && h != SIG_IGN && sig != SIGKILL && sig != SIGSTOP) {
            __atomic_or_fetch(&curr->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
        } else if (h != SIG_IGN &&
                   !(sig == 17 || sig == 18 || sig == 23 || sig == 28)) {
            if (curr->sig_blocked & (1ULL << sig)) {
                /* Blocked: queue it rather than acting on it (see above). */
                __atomic_or_fetch(&curr->sig_pending, (1ULL << sig), __ATOMIC_SEQ_CST);
            } else {
                curr->term_signal = sig;
                curr->exit_code   = 128 + sig;
                sys_exit_impl(r);         /* default: terminate */
            }
        }
    } else if (self_in_group) {
        ret = 0;
    }
    return ret;
}

/* ── Signals ─────────────────────────────────────────────────────────────── */

s64 sys_rt_sigaction_impl(pt_regs_t *r)
{
    int signum = (int)r->rdi;
    const sigaction_t *user_act = (const sigaction_t *)r->rsi;
    sigaction_t *user_oldact = (sigaction_t *)r->rdx;
    size_t sigsetsize = (size_t)r->r10;

    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
    if (signum <= 0 || signum >= _NSIG || signum == SIGKILL || signum == SIGSTOP)
        return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (user_oldact) {
        if ((uintptr_t)user_oldact >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_to_user(user_oldact, &proc->sigactions[signum], sizeof(sigaction_t)) != 0)
            return -(s64)EFAULT;
    }

    if (user_act) {
        if ((uintptr_t)user_act >= TASK_SIZE_MAX) return -(s64)EFAULT;
        sigaction_t kact;
        if (copy_from_user(&kact, user_act, sizeof(sigaction_t)) != 0)
            return -(s64)EFAULT;

        /* A non-canonical handler or restorer address would later be loaded into
         * RIP and faulted in ring 0 on the return from delivery. Reject at
         * registration time. SIG_DFL/SIG_IGN (0/1) are the only allowed
         * non-address handler values. */
        u64 h = (u64)(uintptr_t)kact.sa_handler;
        u64 rst = (u64)(uintptr_t)kact.sa_restorer;
        if (h != (u64)(uintptr_t)SIG_DFL && h != (u64)(uintptr_t)SIG_IGN &&
            h >= TASK_SIZE_MAX)
            return -(s64)EFAULT;
        if (rst != 0 && rst >= TASK_SIZE_MAX)
            return -(s64)EFAULT;

        proc->sigactions[signum] = kact;
    }

    return 0;
}

s64 sys_rt_sigprocmask_impl(pt_regs_t *r)
{
    int how = (int)r->rdi;
    const sigset_t *user_set = (const sigset_t *)r->rsi;
    sigset_t *user_oldset = (sigset_t *)r->rdx;
    size_t sigsetsize = (size_t)r->r10;

    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (user_oldset) {
        if ((uintptr_t)user_oldset >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_to_user(user_oldset, &proc->sig_blocked, sizeof(sigset_t)) != 0)
            return -(s64)EFAULT;
    }

    if (user_set) {
        if ((uintptr_t)user_set >= TASK_SIZE_MAX) return -(s64)EFAULT;
        sigset_t kset = 0;
        if (copy_from_user(&kset, user_set, sizeof(sigset_t)) != 0)
            return -(s64)EFAULT;

        /* SIGKILL and SIGSTOP cannot be blocked */
        kset &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

        switch (how) {
        case SIG_BLOCK:
            proc->sig_blocked |= kset;
            break;
        case SIG_UNBLOCK:
            proc->sig_blocked &= ~kset;
            break;
        case SIG_SETMASK:
            proc->sig_blocked = kset;
            break;
        default:
            return -(s64)EINVAL;
        }
    }

    return 0;
}

s64 sys_tkill_impl(pt_regs_t *r)
{
    s32 tid = (s32)r->rdi;
    int sig = (int)r->rsi;
    if (tid <= 0) return -(s64)EINVAL;
    process_t *curr = sched_current_process();
    return sched_kill_process_permitted(curr, (u32)tid, sig);
}

s64 sys_tgkill_impl(pt_regs_t *r)
{
    s32 tgid = (s32)r->rdi;
    s32 tid = (s32)r->rsi;
    int sig = (int)r->rdx;
    if (tid <= 0) return -(s64)EINVAL;
    process_t *curr = sched_current_process();
    if (tgid > 0) return sched_kill_process_permitted(curr, (u32)tgid, sig);
    return sched_kill_process_permitted(curr, (u32)tid, sig);
}

/* rt_sigpending(set, sigsetsize) — signals raised but still blocked. */
s64 sys_rt_sigpending_impl(pt_regs_t *r)
{
    sigset_t *uset = (sigset_t *)r->rdi;
    size_t sz = (size_t)r->rsi;
    if (sz != sizeof(sigset_t)) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!uset || (uintptr_t)uset >= TASK_SIZE_MAX) return -(s64)EFAULT;
    sigset_t pend = proc->sig_pending & proc->sig_blocked;
    if (copy_to_user(uset, &pend, sizeof pend) != 0) return -(s64)EFAULT;
    return 0;
}

/* sigaltstack(new, old) — set and/or get alternate signal stack context. */
s64 sys_sigaltstack_impl(pt_regs_t *r)
{
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;

    const void *unew = (const void *)r->rdi;
    void *uold       = (void *)r->rsi;

    typedef struct {
        void  *ss_sp;
        int    ss_flags;
        size_t ss_size;
    } user_stack_t;

    user_stack_t old_st;
    memset(&old_st, 0, sizeof(old_st));
    old_st.ss_sp    = p->sas_ss_sp;
    old_st.ss_size  = p->sas_ss_size;
    old_st.ss_flags = (p->sas_ss_flags & 2 /* SS_DISABLE */) ? 2 : 0;

    /* Check if currently executing on the registered alternate stack */
    if (p->sas_ss_sp && !(p->sas_ss_flags & 2 /* SS_DISABLE */) &&
        r->rsp >= (u64)(uintptr_t)p->sas_ss_sp &&
        r->rsp < (u64)(uintptr_t)p->sas_ss_sp + p->sas_ss_size) {
        old_st.ss_flags |= 1 /* SS_ONSTACK */;
    }

    if (uold) {
        if ((uintptr_t)uold >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_to_user(uold, &old_st, sizeof(old_st)) != 0) return -(s64)EFAULT;
    }

    if (unew) {
        if ((uintptr_t)unew >= TASK_SIZE_MAX) return -(s64)EFAULT;
        /* Cannot modify alternate signal stack while active on it */
        if (old_st.ss_flags & 1 /* SS_ONSTACK */) return -(s64)EPERM;

        user_stack_t new_st;
        if (copy_from_user(&new_st, unew, sizeof(new_st)) != 0) return -(s64)EFAULT;

        if (new_st.ss_flags & 2 /* SS_DISABLE */) {
            p->sas_ss_flags = 2;
            p->sas_ss_sp    = NULL;
            p->sas_ss_size  = 0;
        } else {
            /* Minimum size validation: MINSIGSTKSZ is 2048 */
            if (new_st.ss_size < 2048) return -(s64)ENOMEM;
            if ((uintptr_t)new_st.ss_sp >= TASK_SIZE_MAX) return -(s64)EFAULT;
            p->sas_ss_sp    = new_st.ss_sp;
            p->sas_ss_size  = new_st.ss_size;
            p->sas_ss_flags = new_st.ss_flags & ~1;
        }
    }
    return 0;
}

/* ============================================================================
 * POSIX signal waiting
 * ========================================================================= */

/* The prefix of siginfo_t that callers actually read. */
struct k_siginfo {
    s32 si_signo;
    s32 si_errno;
    s32 si_code;
    s32 __pad0;
    s32 si_pid;
    u32 si_uid;
    u64 si_value;
    u8  __pad[128 - 32];
};

s64 sys_rt_sigtimedwait_impl(pt_regs_t *r)
{
    const sigset_t *uset = (const sigset_t *)r->rdi;
    void *uinfo          = (void *)r->rsi;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->rdx;
    size_t sigsetsize    = (size_t)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
    if (!uset || (uintptr_t)uset >= TASK_SIZE_MAX) return -(s64)EFAULT;

    sigset_t set;
    if (copy_from_user(&set, uset, sizeof(set)) != 0) return -(s64)EFAULT;
    /* SIGKILL and SIGSTOP can never be waited for. */
    set &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    bool have_timeout = false;
    u64  deadline = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct linux_timespec ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;
        have_timeout = true;
        deadline = sched_get_ticks() + timespec_to_ticks(&ts);
    }

    for (;;) {
        sigset_t ready = proc->sig_pending & set;
        if (ready) {
            u32 sig = 0;
            for (u32 i = 1; i < 64; i++) {
                if (ready & (1ULL << i)) { sig = i; break; }
            }
            /* Accepting the signal consumes it: it must not also be delivered
             * to a handler on the way out. */
            __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);

            if (uinfo && (uintptr_t)uinfo < TASK_SIZE_MAX) {
                struct k_siginfo info;
                __builtin_memset(&info, 0, sizeof(info));
                info.si_signo = (s32)sig;
                info.si_code  = 0;   /* SI_USER */
                info.si_pid   = (s32)proc->pid;
                info.si_uid   = proc->uid;
                if (copy_to_user(uinfo, &info, sizeof(info)) != 0) return -(s64)EFAULT;
            }
            return (s64)sig;
        }

        if (have_timeout && sched_get_ticks() >= deadline) return -(s64)EAGAIN;

        sched_sleep(1);

        /* A signal outside @set that is deliverable ends the wait with EINTR. */
        if (proc->sig_pending & ~proc->sig_blocked & ~set) return -(s64)EINTR;
    }
}

s64 sys_rt_sigsuspend_impl(pt_regs_t *r)
{
    const sigset_t *umask = (const sigset_t *)r->rdi;
    size_t sigsetsize     = (size_t)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
    if (!umask || (uintptr_t)umask >= TASK_SIZE_MAX) return -(s64)EFAULT;

    sigset_t mask;
    if (copy_from_user(&mask, umask, sizeof(mask)) != 0) return -(s64)EFAULT;
    mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    sigset_t saved = proc->sig_blocked;
    proc->sig_blocked = mask;

    /* Wait until something outside the temporary mask becomes pending.  The
     * handler runs after the old mask is restored rather than under the
     * suspend mask, which is the one place this departs from POSIX. */
    while (!(proc->sig_pending & ~proc->sig_blocked)) {
        sched_sleep(1);
        if (proc->is_zombie) break;
    }

    proc->sig_blocked = saved;
    return -(s64)EINTR;
}

s64 sys_rt_sigqueueinfo_impl(pt_regs_t *r)
{
    u32 pid  = (u32)r->rdi;
    int sig  = (int)r->rsi;
    void *ui = (void *)r->rdx;

    if (sig < 0 || sig >= 64) return -(s64)EINVAL;
    if (ui && (uintptr_t)ui >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* The accompanying siginfo is validated but not queued: signals here are
     * a pending bitmask, so a value cannot be carried alongside one. */
    if (ui) {
        struct k_siginfo info;
        if (copy_from_user(&info, ui, sizeof(info)) != 0) return -(s64)EFAULT;
    }
    if (sig == 0) return 0;
    process_t *curr = sched_current_process();
    return sched_kill_process_permitted(curr, pid, sig);
}

s64 sys_rt_tgsigqueueinfo_impl(pt_regs_t *r)
{
    u32 tgid = (u32)r->rdi;
    int sig  = (int)r->rdx;
    void *ui = (void *)r->r10;

    if (sig < 0 || sig >= 64) return -(s64)EINVAL;
    if (ui && (uintptr_t)ui >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (sig == 0) return 0;
    process_t *curr = sched_current_process();
    return sched_kill_process_permitted(curr, tgid, sig);
}

/* ============================================================================
 * restart_syscall(2)
 *
 * Never called by a program: the kernel plants it as the return address when a
 * restartable call is interrupted by a handler installed with SA_RESTART. This
 * kernel restarts such calls in signal_deliver_pending() by rewinding RIP, so
 * there is nothing left for the entry point to resume — but the number must
 * still resolve, because a stray call to it has a defined answer (-EINTR) and
 * -ENOSYS would be the wrong one.
 * ========================================================================= */

s64 sys_restart_syscall_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)EINTR;
}
