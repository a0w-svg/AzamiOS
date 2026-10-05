/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — System Call Dispatcher & Core Helpers
 * File: kernel/syscall/syscall.c
 * ============================================================================ */
#include "syscall_internal.h"

/* Static dispatch table initialized from syscalls.tbl */
const syscall_fn_t g_syscall_table[SYSCALL_TABLE_SIZE] = __SYSCALL_TABLE_INITIALIZER;

/* Serialises fd-table slot moves (lookup / allocate / teardown). Held only
 * across pointer assignments — never across a blocking call. */
/* fget() — return the file behind `fd` with its reference count raised by one,
 * or NULL if the fd is not a valid open file. Every successful fget() must be
 * balanced by exactly one fput(), which is what stops a concurrent close() in
 * another thread from freeing the struct while this syscall is still using it. */
file_t *fget(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    fd = (int)array_index_nospec((unsigned long)fd, (unsigned long)PROC_MAX_FDS);
    irqflags_t f = spinlock_lock_irqsave(&proc->fd_lock);
    file_t *file = (file_t *)proc->handle_table[fd];
    if (file && (uintptr_t)file >= 0xFFFF800000000000ULL) {
        __atomic_add_fetch(&file->f_count, 1, __ATOMIC_SEQ_CST);
    } else {
        file = NULL;
    }
    spinlock_unlock_irqrestore(&proc->fd_lock, f);
    return file;
}

/* fput() — drop a reference taken by fget(). Frees the file at zero. */
void fput(file_t *file)
{
    if (file) vfs_close(file);
}

/* fd_table_release() — see syscall.h. Clear each slot under the per-process fd_lock so a
 * concurrent fget() (including a cross-process one from pidfd_getfd) cannot
 * observe a file_t between "still in the table" and "already freed"; the
 * blocking close work is done afterwards with the lock dropped. */
void fd_table_release(process_t *proc)
{
    if (!proc) return;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
        file_t *f = (file_t *)__atomic_exchange_n(&proc->handle_table[i],
                                                  NULL, __ATOMIC_SEQ_CST);
        spinlock_unlock_irqrestore(&proc->fd_lock, fl);
        if (f) vfs_close(f);

        /* Object handles have their own lock; az_handle_close() takes it. */
        if (proc->obj_handle_table[i])
            az_handle_close(proc, i);
    }
}


/* fd_install_from() — atomically claim the lowest free fd >= minfd for `file`.
 * Returns the fd, or -EMFILE if the table is full. Prevents two threads racing
 * on the "find a NULL slot" scan from both grabbing the same descriptor. */
/* RLIMIT_NOFILE (index 7, see the RLIMIT_* block further down) soft limit as an
 * fd-count ceiling, clamped to the table size. */
int fd_limit(process_t *proc)
{
    u64 lim = proc->rlimits[7].rlim_cur;
    return (lim >= PROC_MAX_FDS) ? PROC_MAX_FDS : (int)lim;
}

s64 fd_install_from(process_t *proc, void *file, u8 fd_flags, int minfd)
{
    if (!proc) return -(s64)EPERM;
    if (minfd < 0) minfd = 0;
    int limit = fd_limit(proc);
    irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
    for (int i = minfd; i < limit; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = file;
            proc->fd_flags[i] = fd_flags;
            spinlock_unlock_irqrestore(&proc->fd_lock, fl);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&proc->fd_lock, fl);
    return -(s64)EMFILE;
}

s64 fd_install(process_t *proc, file_t *file, u8 fd_flags)
{
    return fd_install_from(proc, file, fd_flags, 0);
}

s64 syscall_install_fd(process_t *proc, void *file, u8 fd_flags)
{
    return fd_install_from(proc, file, fd_flags, 0);
}

/* fd_install_pair() — atomically claim two fds (for pipe/socketpair). On success
 * writes the descriptors to out0 and out1 and returns 0; returns -EMFILE if two
 * free slots are not available (nothing is installed in that case). */
s64 fd_install_pair(process_t *proc, void *f0, void *f1, u8 fd_flags,
                           int *out0, int *out1)
{
    if (!proc) return -(s64)EPERM;
    int limit = fd_limit(proc);
    irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
    int a = -1, b = -1;
    for (int i = 0; i < limit; i++) {
        if (!proc->handle_table[i]) {
            if (a < 0) a = i;
            else { b = i; break; }
        }
    }
    if (a < 0 || b < 0) { spinlock_unlock_irqrestore(&proc->fd_lock, fl); return -(s64)EMFILE; }
    proc->handle_table[a] = f0; proc->fd_flags[a] = fd_flags;
    proc->handle_table[b] = f1; proc->fd_flags[b] = fd_flags;
    spinlock_unlock_irqrestore(&proc->fd_lock, fl);
    *out0 = a; *out1 = b;
    return 0;
}

/* Exported to the subsystems that mint their own file objects (POSIX message
 * queues). They are thin aliases rather than a second implementation so the
 * fd table has exactly one set of rules — one lock, one refcount discipline. */
s64 syscall_fd_install(process_t *proc, file_t *file, u8 fd_flags)
{
    return fd_install_from(proc, file, fd_flags, 0);
}

file_t *syscall_fget(process_t *proc, int fd) { return fget(proc, fd); }
void    syscall_fput(file_t *file)            { fput(file); }


/* ── Missing-syscall reporting ────────────────────────────────────────────
 * Porting a stock Linux binary is mostly a matter of finding which call it
 * makes that this kernel does not answer. A silent -ENOSYS turns that into a
 * guessing game: the libc usually swallows the error and fails much later,
 * somewhere unrelated. So the first time each unknown number is asked for, say
 * so on the kernel log — once per number, because a program that gets -ENOSYS
 * in a retry loop would otherwise flood the serial line and change the timing
 * of the very bug being chased.
 * ------------------------------------------------------------------------- */
static u64 g_missing_seen[(SYSCALL_TABLE_SIZE + 63) / 64];
static u8  g_missing_overflow_seen;

void syscall_report_missing(u64 nr, pt_regs_t *regs)
{
    if (nr < SYSCALL_TABLE_SIZE) {
        u64 bit = 1ULL << (nr & 63);
        u64 *word = &g_missing_seen[nr >> 6];
        if (__atomic_fetch_or(word, bit, __ATOMIC_RELAXED) & bit) return;
    } else {
        if (__atomic_exchange_n(&g_missing_overflow_seen, 1, __ATOMIC_RELAXED)) return;
    }

    process_t *p = sched_current_process();
    kprintf("[SYSCALL] unimplemented syscall %llu from '%s' (PID %u) at RIP=0x%016llx\n",
            (unsigned long long)nr,
            p ? p->name : "?", p ? p->pid : 0,
            (unsigned long long)regs->rip);
}


/* Returns 1 if the return to ring 3 must go via IRETQ (full GPR restore +
 * sanitised CS/SS/RFLAGS), 0 for the normal SYSRETQ fast path. Only
 * rt_sigreturn needs it: it rebuilt `regs` from a user-controlled frame, and
 * the interrupted context it resumes may not be at a syscall boundary (live
 * RCX/R11), which SYSRETQ would clobber. */
int syscall_dispatch(pt_regs_t *regs)
{
    process_t *sp = sched_current_process();

    /* PTRACE_SYSCALL entry stop — before the number is even validated, because
     * the tracer is allowed to rewrite RAX and the argument registers here.
     * Storing RAX into a register that is out of range (Linux's convention is
     * -1) is how a tracer cancels a call outright: the bounds check below then
     * turns it into -ENOSYS, and the exit stop still fires so the tracer can
     * substitute a return value. */
    bool traced = unlikely(sp != NULL && (sp->ptrace_flags & PT_SYSCALL_TRACE) != 0);
    if (traced) ptrace_syscall_stop(regs, true);

    u64 nr = regs->rax;

    /* Spectre-v1: the bounds check below is a conditional branch, and a
     * mispredicted one still speculatively loads g_syscall_table[nr] for an
     * attacker-chosen nr — leaking whatever lies past the table through the
     * cache. Masking the index makes the speculative path use 0 instead, with
     * no branch for the predictor to get wrong. The check itself still stands;
     * this only constrains what happens before it resolves. */
    nr = array_index_nospec(nr, (u64)SYSCALL_TABLE_SIZE);

    if (unlikely(regs->rax >= SYSCALL_TABLE_SIZE) || !g_syscall_table[nr]) {
        /* A tracer-cancelled call is not a missing syscall; logging it would
         * make every strace run look like a porting failure. */
        if (!traced) syscall_report_missing(regs->rax, regs);
        regs->rax = (u64)(-(s64)ENOSYS);
        if (traced) ptrace_syscall_stop(regs, false);
        signal_deliver_pending(regs, -1);
        return 0;
    }

    /* seccomp gate. Checked before the handler runs so a confined process
     * cannot reach any syscall side effect. STRICT answers a violation with
     * an immediate SIGKILL, per Linux — no errno the process could branch
     * on and retry. FILTER's actions are richer (seccomp_denied is set for
     * every one of them except ALLOW/LOG, which fall through to the real
     * handler below exactly as an unfiltered syscall would); either way,
     * once regs->rax is decided the normal tail of this function still
     * runs — ptrace's exit stop, and signal_deliver_pending() so a signal
     * a denial just raised (SIGSYS from TRAP) actually gets delivered
     * before returning to ring 3. */
    bool seccomp_denied = false;
    if (unlikely(sp && sp->seccomp_mode != SECCOMP_MODE_DISABLED)) {
        if (sp->seccomp_mode == SECCOMP_MODE_STRICT) {
            if (!security_seccomp_check(sp, nr)) {
                sched_kill_process(sp->pid, SIGKILL);
                regs->rax = (u64)(-(s64)EPERM);
                return 0;
            }
        } else {
            u32 action = seccomp_filter_run(sp, nr, regs);
            switch (action & SECCOMP_RET_ACTION_FULL) {
            case SECCOMP_RET_KILL_PROCESS:
            case SECCOMP_RET_KILL_THREAD:
                /* Unmaskable, uncatchable — like STRICT's violation, this
                 * skips straight past any handler the process installed. */
                sched_kill_process(sp->pid, SIGSYS);
                regs->rax = (u64)(-(s64)EPERM);
                return 0;
            case SECCOMP_RET_TRAP: {
                sighandler_t h = sp->sigactions[SIGSYS].sa_handler;
                if (h != SIG_DFL && h != SIG_IGN)
                    __atomic_or_fetch(&sp->sig_pending, (1ULL << SIGSYS), __ATOMIC_SEQ_CST);
                else
                    sched_kill_process(sp->pid, SIGSYS);
                regs->rax = (u64)(-(s64)ENOSYS);
                seccomp_denied = true;
                break;
            }
            case SECCOMP_RET_ERRNO:
                regs->rax = (u64)(-(s64)(action & SECCOMP_RET_DATA));
                seccomp_denied = true;
                break;
            case SECCOMP_RET_TRACE:
                /* No ptrace-seccomp (PTRACE_EVENT_SECCOMP) integration yet
                 * — see the comment on SECCOMP_GET_ACTION_AVAIL above. Fail
                 * closed rather than silently allow. */
                regs->rax = (u64)(-(s64)ENOSYS);
                seccomp_denied = true;
                break;
            case SECCOMP_RET_LOG:
                kprintf("[SECCOMP] pid %u: syscall %llu\n", sp->pid, (unsigned long long)nr);
                break; /* falls through to ALLOW */
            case SECCOMP_RET_ALLOW:
            default:
                break;
            }
        }
    }

    if (!seccomp_denied)
        regs->rax = (u64)g_syscall_table[nr](regs);

    /* Syscall-exit stop. ptrace_syscall_stop() re-tests PT_SYSCALL_TRACE, so a
     * tracer that answered the entry stop with PTRACE_CONT gets no exit stop. */
    if (traced) ptrace_syscall_stop(regs, false);

    /* On the way back to ring 3, run any pending user signal handler. Pass the
     * syscall number so an interrupted call can honour SA_RESTART — except for
     * rt_sigreturn, which has already rewritten `regs` with a restored frame. */
    signal_deliver_pending(regs, nr == SYS_rt_sigreturn ? -1 : (s64)nr);

    return (nr == SYS_rt_sigreturn) ? 1 : 0;
}


void syscall_init(void)
{
    pr_debug("[SYSCALL] Dispatch table ready (%d entries)\n", SYSCALL_TABLE_SIZE);
}

/* ── Path Resolution Helper ──────────────────────────────────────────────── */

s64 copy_str_from_user(char *dst, const char *user_src, size_t max_len)
{
    if (!dst || !user_src || max_len == 0) return -(s64)EINVAL;
    if ((uintptr_t)user_src >= TASK_SIZE_MAX) return -(s64)EFAULT;

    size_t copied = 0;
    while (copied < max_len - 1) {
        size_t chunk = max_len - 1 - copied;
        if (chunk > 128) chunk = 128;
        /* Never straddle a page boundary in one copy_from_user(): a fault
         * partway through would discard the bytes already read. Clamping to
         * the current page guarantees a fault only ever lands at chunk start. */
        size_t to_page = 0x1000 - (((uintptr_t)user_src + copied) & 0xFFF);
        if (chunk > to_page) chunk = to_page;

        size_t not_copied = copy_from_user(dst + copied, user_src + copied, chunk);
        size_t got = chunk - not_copied;
        for (size_t i = 0; i < got; i++) {
            if (dst[copied + i] == '\0') return (s64)(copied + i);
        }
        copied += got;
        if (not_copied) return -(s64)EFAULT;   /* faulted before a NUL was found */
    }
    dst[max_len - 1] = '\0';
    return (s64)(max_len - 1);
}

/* proc_root(proc) — the process's filesystem root, defaulting to "/" for any
 * process created before chroot(2) existed or whose field is somehow blank. */
static inline const char *proc_root(const process_t *proc)
{
    return (proc && proc->root[0]) ? proc->root : "/";
}

/* proc_is_confined(proc) — true once chroot(2) has moved the root off "/". */
bool proc_is_confined(const process_t *proc)
{
    const char *r = proc_root(proc);
    return !(r[0] == '/' && r[1] == '\0');
}

/*
 * vpath_to_real() — turn a path as the process sees it into the real path the
 * VFS is asked for, by prefixing the process's root.
 *
 * This is the whole of chroot(2)'s enforcement, and it works because the
 * caller has already run the path through vfs_resolve_path(): that normalises
 * "." and "..", and its ".." handling stops at depth 0, so no amount of
 * "../../.." in a user path can produce a virtual path that does not begin at
 * "/". Prefixing an escape-proof virtual path with the jail root yields a real
 * path that is always inside the jail.
 *
 * Before this existed, chroot(2) only overwrote proc->cwd. An absolute path
 * bypassed it outright — a "confined" process could open /etc/shadow by name —
 * and a single chdir("..") walked back out. The call reported success while
 * confining nothing.
 */
s64 vpath_to_real(const process_t *proc, const char *vpath, char *out, size_t out_len)
{
    const char *root = proc_root(proc);

    if (!proc_is_confined(proc)) {
        size_t n = strlen(vpath);
        if (n + 1 > out_len) return -(s64)ENAMETOOLONG;
        memcpy(out, vpath, n + 1);
        return 0;
    }

    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen - 1] == '/') rlen--;   /* no doubled slash */

    /* The jail root itself is "/" from the inside. */
    bool bare_root = (vpath[0] == '/' && vpath[1] == '\0');
    size_t vlen = bare_root ? 0 : strlen(vpath);

    if (rlen + vlen + 1 > out_len) return -(s64)ENAMETOOLONG;
    memcpy(out, root, rlen);
    memcpy(out + rlen, vpath, vlen);
    out[rlen + vlen] = '\0';
    return 0;
}

/* real_to_vpath() — the inverse, for the one place that starts from a real
 * path: an *at() call resolving against a directory fd, whose dentry chain
 * names the real filesystem. A dirfd that lies outside the jail (it can only
 * have been opened before the chroot) resolves against "/" rather than leaking
 * its location into the process's view. */
const char *real_to_vpath(const process_t *proc, const char *real)
{
    if (!proc_is_confined(proc)) return real;

    const char *root = proc_root(proc);
    size_t rlen = strlen(root);
    while (rlen > 1 && root[rlen - 1] == '/') rlen--;

    if (strncmp(real, root, rlen) != 0) return "/";
    if (real[rlen] == '\0') return "/";
    if (real[rlen] != '/')   return "/";
    return real + rlen;
}

/* Resolve a user path to the process's *virtual* namespace: normalised, with
 * ".." clamped at its root, but without the jail prefix. chdir(2) and
 * getcwd(2) work in these terms; everything else wants the real path that
 * copy_user_path_resolve_at() returns. */
s64 copy_user_vpath_resolve_at(int dirfd, char *vpath, size_t max_len, const char *user_path)
{
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char raw[512];
    __builtin_memset(raw, 0, sizeof(raw));
    s64 slen = copy_str_from_user(raw, user_path, sizeof(raw));
    if (slen < 0) return slen;

    /* An absolute path is absolute *within the process's root*, which is what
     * makes chroot(2) confine anything at all. */
    if (raw[0] == '/') {
        return vfs_resolve_path("/", raw, vpath, max_len);
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (dirfd == AT_FDCWD) {
        const char *cwd = (proc->cwd[0]) ? proc->cwd : "/";
        return vfs_resolve_path(cwd, raw, vpath, max_len);
    }

    if (dirfd < 0 || dirfd >= PROC_MAX_FDS || !proc->handle_table[dirfd]) return -(s64)EBADF;
    file_t *df = (file_t *)proc->handle_table[dirfd];
    if (!df || !df->f_dentry || !df->f_inode) return -(s64)EBADF;
    if (!S_ISDIR(df->f_inode->i_mode)) return -(s64)ENOTDIR;

    char dir_path[512];
    __builtin_memset(dir_path, 0, sizeof(dir_path));
    dentry_build_path(df->f_dentry, dir_path, sizeof(dir_path));

    /* dentry_build_path() names the real filesystem; bring it back into the
     * process's view before resolving against it. */
    return vfs_resolve_path(real_to_vpath(proc, dir_path), raw, vpath, max_len);
}

/* Resolve a user path all the way to the real path the VFS takes. */
s64 copy_user_path_resolve_at(int dirfd, char *kpath, size_t max_len, const char *user_path)
{
    process_t *proc = sched_current_process();
    if (!proc_is_confined(proc))
        return copy_user_vpath_resolve_at(dirfd, kpath, max_len, user_path);

    char vpath[512];
    s64 err = copy_user_vpath_resolve_at(dirfd, vpath, sizeof(vpath), user_path);
    if (err < 0) return err;
    return vpath_to_real(proc, vpath, kpath, max_len);
}

s64 copy_user_path_resolve(char *kpath, size_t max_len, const char *user_path)
{
    return copy_user_path_resolve_at(AT_FDCWD, kpath, max_len, user_path);
}



/* Detach the file at `fd` from the table under the per-process fd_lock so the unref is
 * ordered against a concurrent fget() (see fget()). Returns the detached file
 * (caller must vfs_close() it outside the lock) or NULL. */
file_t *fd_detach(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
    file_t *f = (file_t *)proc->handle_table[fd];
    proc->handle_table[fd] = NULL;
    proc->fd_flags[fd] = 0;
    spinlock_unlock_irqrestore(&proc->fd_lock, fl);
    if (f && (uintptr_t)f < 0xFFFF800000000000ULL) f = NULL;
    return f;
}

/* ── Sleeping on the timekeeper ────────────────────────────────────────────
 *
 * Every timed sleep ends at an absolute CLOCK_MONOTONIC deadline, checked
 * against the real clock rather than counted in ticks: a tick count is only
 * ever an approximation of elapsed time (a sleep begun just before a tick
 * would otherwise return up to a whole tick early, which POSIX forbids).
 *
 * The scheduler only wakes sleepers on tick boundaries, so each wait is
 * sized to the first tick at or after the deadline, using the phase of the
 * last tick (CLOCK_MONOTONIC_COARSE is stamped exactly when it fired).
 * What remains after that — normally nothing, at most a tick's jitter — is
 * spun off if it is shorter than SLEEP_SPIN_NS, which is what lets a short
 * usleep() return in microseconds rather than in a full 10 ms tick. */
#define SLEEP_TICK_NS   (NSEC_PER_SEC / TK_HZ)
#define SLEEP_SPIN_NS   200000ULL           /* 200 µs */
#define SLEEP_HR_SPIN_NS 20000ULL           /* 20 µs, with one-shot timers */

bool sleep_signal_pending(process_t *proc)
{
    return proc && (proc->sig_pending & ~proc->sig_blocked);
}

/* Returns 0 once CLOCK_MONOTONIC >= @deadline, or -EINTR (with the time
 * still to go in *@left, when non-NULL) if an unblocked signal arrives. */
s64 sleep_until_mono(u64 deadline, u64 *left)
{
    process_t *proc = sched_current_process();
    for (;;) {
        u64 now = ktime_get_ns();
        if (now >= deadline) return 0;
        if (sleep_signal_pending(proc)) {
            if (left) *left = deadline - now;
            return -(s64)EINTR;
        }
        u64 rem = deadline - now;
        /* With one-shot LAPIC interrupts the scheduler can wake this thread
         * at the deadline itself, so only the last few microseconds — less
         * than an interrupt plus a context switch costs — are spun. */
        bool hr = sched_hrsleep_available();
        if (rem <= (hr ? SLEEP_HR_SPIN_NS : SLEEP_SPIN_NS)) {
            while (ktime_get_ns() < deadline) {
                if (sleep_signal_pending(proc)) {
                    u64 n2 = ktime_get_ns();
                    if (left) *left = n2 < deadline ? deadline - n2 : 0;
                    return n2 < deadline ? -(s64)EINTR : 0;
                }
                cpu_pause();
            }
            return 0;
        }
        if (hr) {
            /* May come back early (a signal, an unblock); the loop re-checks. */
            sched_sleep_until_ns(deadline);
            continue;
        }
        /* Ticks until the first tick boundary at or after the deadline. */
        s64 cs = 0, cn = 0;
        ktime_get_clock(VDSO_CLOCK_MONOTONIC_COARSE, &cs, &cn);
        u64 last_tick = (u64)cs * NSEC_PER_SEC + (u64)cn;
        u64 span = deadline > last_tick ? deadline - last_tick : rem;
        u64 ticks = (span + SLEEP_TICK_NS - 1) / SLEEP_TICK_NS;
        /* Leave the sub-tick tail to the spin above when it is short. */
        if (ticks > 1 && (span % SLEEP_TICK_NS) != 0 &&
            (span % SLEEP_TICK_NS) <= SLEEP_SPIN_NS)
            ticks--;
        if (ticks == 0) ticks = 1;
        sched_sleep(ticks);
    }
}
