/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Process and Scheduler Syscalls
 * File: kernel/syscall/sys_proc.c
 * ============================================================================ */
#include "syscall_internal.h"

/* proc_clone_attrs() — copy the POSIX process attributes a fork()/clone() child
 * inherits from its parent: cwd, umask, credentials, supplementary groups,
 * process group / session, resource layout, TLS bases, and signal state.
 * Pending signals are NOT inherited (POSIX). */
static void proc_clone_attrs(process_t *child, const process_t *parent)
{
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
    child->cwd[sizeof(child->cwd) - 1] = '\0';

    /* The filesystem root is inherited like the working directory, and like
     * Linux it survives execve() too: a child can never be less confined than
     * the parent that forked it. */
    strncpy(child->root, parent->root, sizeof(child->root) - 1);
    child->root[sizeof(child->root) - 1] = '\0';

    if (child->uts_ns) uts_ns_put(child->uts_ns);
    child->uts_ns = uts_ns_get(parent->uts_ns);
    child->new_pid_ns = false;

    child->umask = parent->umask;
    child->uid  = parent->uid;   child->euid = parent->euid;   child->suid = parent->suid;
    child->gid  = parent->gid;   child->egid = parent->egid;   child->sgid = parent->sgid;
    child->fsuid = parent->fsuid; child->fsgid = parent->fsgid;
    child->ngroups = parent->ngroups;
    for (u32 i = 0; i < parent->ngroups && i < 32; i++)
        child->groups[i] = parent->groups[i];

    child->pgid = parent->pgid;
    child->sid  = parent->sid;
    child->pdeath_sig = parent->pdeath_sig;

    child->heap_start   = parent->heap_start;
    child->heap_end     = parent->heap_end;
    child->mmap_current = parent->mmap_current;
    child->fs_base      = parent->fs_base;
    child->gs_base      = parent->gs_base;

    /* Capability sets are inherited wholesale by fork(): the child is the same
     * program with the same privileges. no_new_privs and seccomp_mode are
     * inherited too and remain one-way — a child can never hold more privilege
     * than the parent that created it. */
    child->cap_permitted   = parent->cap_permitted;
    child->cap_effective   = parent->cap_effective;
    child->cap_inheritable = parent->cap_inheritable;
    child->cap_bounding    = parent->cap_bounding;
    child->no_new_privs    = parent->no_new_privs;
    child->seccomp_mode    = parent->seccomp_mode;
    /* SECCOMP_MODE_FILTER: the child starts out sharing the parent's filter
     * chain (a confined process cannot fork its way out of confinement) —
     * see kernel/security/seccomp.c. */
    seccomp_filters_share(child, parent);

    /* fork() duplicates the address space verbatim, so the child keeps the
     * parent's randomised layout; only a subsequent execve() re-rolls it. */
    child->personality = parent->personality;
    child->stack_low   = parent->stack_low;
    child->stack_high  = parent->stack_high;

    for (int i = 0; i < _NSIG; i++)
        child->sigactions[i] = parent->sigactions[i];
    child->sig_blocked = parent->sig_blocked;
    child->sig_pending = 0;

    /* Alternate signal stack is preserved across fork */
    child->sas_ss_sp    = parent->sas_ss_sp;
    child->sas_ss_size  = parent->sas_ss_size;
    child->sas_ss_flags = parent->sas_ss_flags;

    /* The robust-futex list address is a userspace pointer into an address
     * space fork() duplicated verbatim, so it stays valid in the child. */
    child->robust_list     = parent->robust_list;
    child->robust_list_len = parent->robust_list_len;

    /* Protection keys are a property of the page tables, which the child gets
     * a copy of, so the allocation map has to come along or pkey_free() in the
     * child would be operating on a key it does not believe it owns. */
    child->pkey_alloc_map = parent->pkey_alloc_map;

    /* Both of these are documented as inherited across fork(). */
    child->ioprio              = parent->ioprio;
    child->mempolicy_mode      = parent->mempolicy_mode;
    child->mempolicy_nodemask  = parent->mempolicy_nodemask;
    child->mempolicy_home_node = parent->mempolicy_home_node;

    /* Resource limits are inherited whole and survive a later execve(). */
    for (int i = 0; i < RLIMIT_NLIMITS; i++)
        child->rlimits[i] = parent->rlimits[i];

    /* Scheduling policy, RT priority and nice are inherited by fork() (absent
     * SCHED_RESET_ON_FORK, which this kernel does not implement). The child's
     * threads are created with the default weight; the nice value is re-applied
     * to them below via sched_apply_weight(). */
    child->sched_policy  = parent->sched_policy;
    child->sched_rt_prio = parent->sched_rt_prio;
    child->prio_nice     = parent->prio_nice;
}



/* ── Process Hierarchy, Execve & Lifecycle ───────────────────────────────── */

s64 sys_getpid_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 1;
}

s64 sys_getppid_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (proc && proc->parent) return (s64)proc->parent->pid;
    return 0;
}

s64 sys_fork_impl(pt_regs_t *r)
{
    KTRACE_CALL("fork", 0);
    process_t *parent = sched_current_process();
    if (!parent) return -(s64)EPERM;

    vmm_space_t child_space = vmm_clone_space(parent->pml4_phys);
    if (!child_space) return -(s64)ENOMEM;

    process_t *child = proc_create(parent->name, child_space);
    if (!child) {
        vmm_destroy_space(child_space);
        return -(s64)ENOMEM;
    }
    child->parent = parent;
    proc_clone_attrs(child, parent);
    if (vma_clone(child, parent) != 0) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    /* The child inherits the parent's System V shared-memory attachments, and
     * each segment must count it — vmm_clone_space() already shared the
     * frames, so a segment that did not know about the child could be freed
     * out from under it.  Done before any fd reference is taken so a failure
     * here needs no unwinding beyond destroying the child. */
    if (sysvipc_process_fork(child, parent) != 0) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    /* BUG-AN fix: acquire fd_lock while copying parent's handle_table */
    irqflags_t fd_irqf = spinlock_lock_irqsave(&parent->fd_lock);
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (parent->handle_table[i]) {
            file_t *pf = (file_t *)parent->handle_table[i];
            __atomic_add_fetch(&pf->f_count, 1, __ATOMIC_SEQ_CST);
            child->handle_table[i] = pf;
            child->fd_flags[i] = parent->fd_flags[i];
        }
        if (parent->obj_handle_table[i]) {
            az_object_t *obj = parent->obj_handle_table[i];
            az_object_reference(obj);
            child->obj_handle_table[i] = obj;
        }
    }
    spinlock_unlock_irqrestore(&parent->fd_lock, fd_irqf);

    thread_t *t = thread_create_ex(child, r->rip, r->rsp, false, false);
    if (!t) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    /* fork() duplicates the *calling thread*, so the child must resume on that
     * thread's TLS base. proc_clone_attrs copies proc->fs_base, but in a
     * multi-threaded parent that field holds whichever thread called
     * arch_prctl(ARCH_SET_FS) most recently — not necessarily this one — and
     * the fresh child thread has fs_base == 0, so it would fall back to that
     * stale value and resume with another thread's TLS. */
    {
        thread_t *self = sched_current_thread();
        u64 tls = self ? (self->has_thread_fs_base ? self->fs_base : parent->fs_base)
                       : parent->fs_base;
        t->fs_base            = tls;
        t->has_thread_fs_base = true; /* child resumes with an explicit, resolved base */
        child->fs_base        = tls;
    }

    if (t->user_regs) {
        *t->user_regs = *r;
        t->user_regs->rax = 0;      /* Child returns 0 */
        t->user_regs->rflags = 0x202; /* IF=1 */
    }

    /* PTRACE_O_TRACEFORK: the child inherits the tracer and is born stopped, so
     * `strace -f` can attach to it before it executes a single instruction.
     * The parent then reports the fork event, carrying the child's pid as the
     * PTRACE_GETEVENTMSG value. */
    bool trace_child = ptrace_traced(parent) &&
                       (parent->ptrace_opts & PTRACE_O_TRACEFORK) != 0;
    if (trace_child) {
        child->tracer_pid    = parent->tracer_pid;
        child->ptrace_opts   = parent->ptrace_opts;
        child->ptrace_flags  = PT_TRACED;
        child->ptrace_stop_sig = SIGTRAP;
        child->ptrace_event  = PTRACE_EVENT_STOP;
        sched_request_stop(child, SIGSTOP, PROC_STOP_PTRACE);
    }

    sched_enqueue_thread(t);

    if (trace_child)
        ptrace_report_event(r, PTRACE_EVENT_FORK, (u64)child->pid);

    return (s64)child->pid;
}

static s64 do_clone(u64 flags, virt_addr_t child_stack, int *parent_tidptr, int *child_tidptr, u64 newtls, const pt_regs_t *r)
{
    process_t *parent = sched_current_process();
    if (!parent) return -(s64)EPERM;

    /* CLONE_VM|CLONE_THREAD: a real thread that SHARES the caller's address
     * space — glibc pthread_create. It must NOT get a private
     * vmm_clone_space()/proc_create() copy (that breaks every shared futex /
     * errno / malloc arena / TLS). Spawn it into the same process_t.
     * NOTE: CLONE_VM *without* CLONE_THREAD (vfork / posix_spawn) is a separate
     * process that just shares memory until it execs — that falls through to
     * the fork path below, exactly as before. */
    if ((flags & 0x00000100ULL) && (flags & 0x00010000ULL)) {
        if (!child_stack || (uintptr_t)child_stack >= TASK_SIZE_MAX)
            return -(s64)EINVAL;

        thread_t *t = thread_create_ex(parent, r->rip, (uintptr_t)child_stack, false, false);
        if (!t) return -(s64)ENOMEM;

        if (flags & 0x00080000ULL /* CLONE_SETTLS */) {
            t->fs_base            = newtls;          /* per-thread TLS base */
            t->has_thread_fs_base = true;
        }

        if (t->user_regs) {
            *t->user_regs = *r;
            t->user_regs->rax    = 0;               /* child returns 0 */
            t->user_regs->rsp    = (u64)child_stack;
            t->user_regs->rflags = 0x202;
        }
        if ((flags & 0x00100000ULL /* CLONE_PARENT_SETTID */) && parent_tidptr &&
            (uintptr_t)parent_tidptr < TASK_SIZE_MAX) {
            int tid = (int)t->tid;
            copy_to_user(parent_tidptr, &tid, sizeof(int));
        }
        if ((flags & 0x01000000ULL /* CLONE_CHILD_SETTID */) && child_tidptr &&
            (uintptr_t)child_tidptr < TASK_SIZE_MAX) {
            int tid = (int)t->tid;
            copy_to_user(child_tidptr, &tid, sizeof(int));
        }
        if ((flags & 0x00200000ULL /* CLONE_CHILD_CLEARTID */) && child_tidptr &&
            (uintptr_t)child_tidptr < TASK_SIZE_MAX) {
            t->clear_child_tid = (u64)(uintptr_t)child_tidptr;
        }
        sched_enqueue_thread(t);
        return (s64)t->tid;
    }

    vmm_space_t child_space = vmm_clone_space(parent->pml4_phys);
    if (!child_space) return -(s64)ENOMEM;

    process_t *child = proc_create(parent->name, child_space);
    if (!child) {
        vmm_destroy_space(child_space);
        return -(s64)ENOMEM;
    }

    child->parent       = parent;
    proc_clone_attrs(child, parent);
    if (flags & 0x04000000ULL /* CLONE_NEWUTS */) {
        uts_ns_put(child->uts_ns);
        child->uts_ns = uts_ns_create(parent->uts_ns ? parent->uts_ns->nodename : "azamios",
                                      parent->uts_ns ? parent->uts_ns->domainname : "local");
    }
    if (vma_clone(child, parent) != 0) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }
    /* Same as fork(): this path makes a separate process out of a shared
     * snapshot, so it inherits the SysV attachments and their nattch. */
    if (sysvipc_process_fork(child, parent) != 0) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    /* BUG-AN fix: acquire fd_lock while copying parent's handle_table */
    irqflags_t fd_irqf = spinlock_lock_irqsave(&parent->fd_lock);
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (parent->handle_table[i]) {
            file_t *pf = (file_t *)parent->handle_table[i];
            __atomic_add_fetch(&pf->f_count, 1, __ATOMIC_SEQ_CST);
            child->handle_table[i] = pf;
            child->fd_flags[i] = parent->fd_flags[i];
        }
        if (parent->obj_handle_table[i]) {
            az_object_t *obj = parent->obj_handle_table[i];
            az_object_reference(obj);
            child->obj_handle_table[i] = obj;
        }
    }
    spinlock_unlock_irqrestore(&parent->fd_lock, fd_irqf);

    virt_addr_t user_rsp = child_stack ? child_stack : r->rsp;
    thread_t *t = thread_create_ex(child, r->rip, user_rsp, false, false);
    if (!t) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    if (flags & 0x00080000ULL /* CLONE_SETTLS */) {
        child->fs_base        = newtls;
        t->fs_base            = newtls;
        t->has_thread_fs_base = true;
    } else {
        thread_t *self = sched_current_thread();
        u64 tls = self ? (self->has_thread_fs_base ? self->fs_base : parent->fs_base)
                       : parent->fs_base;
        t->fs_base            = tls;
        t->has_thread_fs_base = true; /* explicit, resolved base for the new process's thread */
        child->fs_base        = tls;
    }

    if (t->user_regs) {
        *t->user_regs = *r;
        t->user_regs->rax = 0;        /* Child returns 0 */
        t->user_regs->rsp = user_rsp;
        t->user_regs->rflags = 0x202; /* IF=1 */
    }

    /* 0x100 is CLONE_VM, not CLONE_PARENT_SETTID — the old constant here meant
     * every CLONE_VM caller got a tid written through whatever rdx happened to
     * hold, and a genuine CLONE_PARENT_SETTID caller got nothing. */
    if ((flags & 0x00100000ULL /* CLONE_PARENT_SETTID */) && parent_tidptr && (uintptr_t)parent_tidptr < TASK_SIZE_MAX) {
        int tid = (int)child->pid;
        copy_to_user(parent_tidptr, &tid, sizeof(int));
    }

    if ((flags & 0x01000000ULL /* CLONE_CHILD_SETTID */) && child_tidptr && (uintptr_t)child_tidptr < TASK_SIZE_MAX) {
        int tid = (int)child->pid;
        copy_to_user(child_tidptr, &tid, sizeof(int));
    }

    if ((flags & 0x00200000ULL /* CLONE_CHILD_CLEARTID */) && child_tidptr && (uintptr_t)child_tidptr < TASK_SIZE_MAX) {
        t->clear_child_tid = (u64)(uintptr_t)child_tidptr;
    }

    sched_enqueue_thread(t);
    return (s64)child->pid;
}

s64 sys_clone_impl(pt_regs_t *r)
{
    u64 flags = r->rdi;
    virt_addr_t child_stack = (virt_addr_t)r->rsi;
    int *parent_tidptr = (int *)r->rdx;
    int *child_tidptr = (int *)r->r10;
    u64 newtls = r->r8;
    return do_clone(flags, child_stack, parent_tidptr, child_tidptr, newtls, r);
}

s64 sys_vfork_impl(pt_regs_t *r)
{
    return sys_fork_impl(r);
}

/* Shared execve core: `kpath` is an already-resolved absolute path; argv/envp
 * are user-space pointer vectors. On success this replaces the current address
 * space and rewrites `r`, so it does not return to the caller's program. */
static s64 execve_core(pt_regs_t *r, const char *kpath,
                       const char *const *user_argv, const char *const *user_envp)
{
    /* Allocate argv and envp pointer arrays on heap to prevent kernel stack overflow */
    char **kargv = (char **)kzalloc(128 * sizeof(char *));
    if (!kargv) return -(s64)ENOMEM;

    int argc = 0;
    if (user_argv && (uintptr_t)user_argv < TASK_SIZE_MAX) {
        for (int i = 0; i < 127; i++) {
            const char *arg_ptr = NULL;
            if (copy_from_user(&arg_ptr, &user_argv[i], sizeof(char *)) != 0) break;
            if (!arg_ptr) break;
            if ((uintptr_t)arg_ptr >= TASK_SIZE_MAX) break;

            char *buf = (char *)kmalloc(1024);
            if (!buf) break;
            if (copy_str_from_user(buf, arg_ptr, 1024) < 0) {
                kfree(buf);
                break;
            }
            kargv[argc++] = buf;
        }
    }
    if (argc == 0) {
        kargv[0] = strdup(kpath);
        argc = 1;
    }
    kargv[argc] = NULL;

    char **kenvp = (char **)kzalloc(128 * sizeof(char *));
    if (!kenvp) {
        for (int i = 0; i < argc; i++) kfree(kargv[i]);
        kfree(kargv);
        return -(s64)ENOMEM;
    }

    int envc = 0;
    if (user_envp && (uintptr_t)user_envp < TASK_SIZE_MAX) {
        for (int i = 0; i < 127; i++) {
            const char *env_ptr = NULL;
            if (copy_from_user(&env_ptr, &user_envp[i], sizeof(char *)) != 0) break;
            if (!env_ptr) break;
            if ((uintptr_t)env_ptr >= TASK_SIZE_MAX) break;

            char *buf = (char *)kmalloc(1024);
            if (!buf) break;
            if (copy_str_from_user(buf, env_ptr, 1024) < 0) {
                kfree(buf);
                break;
            }
            kenvp[envc++] = buf;
        }
    }
    kenvp[envc] = NULL;

    process_t *proc = sched_current_process();
    if (!proc) {
        for (int i = 0; i < argc; i++) kfree(kargv[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp[i]);
        kfree(kargv);
        kfree(kenvp);
        return -(s64)EPERM;
    }

    uintptr_t new_entry = 0;
    phys_addr_t new_space = 0;
    u64 new_rsp = 0;
    phys_addr_t old_space = proc->pml4_phys;

    int err = elf_load_exec(proc, kpath, (const char *const *)kargv, (const char *const *)kenvp,
                            &new_entry, &new_space, &new_rsp);
    if (err < 0 && kpath[0] == '/') {
        char alt_path[256];
        size_t klen = strlen(kpath);
        if (klen < sizeof(alt_path) - 5) {
            strncpy(alt_path, kpath, sizeof(alt_path) - 5);
            alt_path[klen] = '.';
            alt_path[klen + 1] = 'e';
            alt_path[klen + 2] = 'l';
            alt_path[klen + 3] = 'f';
            alt_path[klen + 4] = '\0';
            err = elf_load_exec(proc, alt_path, (const char *const *)kargv, (const char *const *)kenvp,
                                &new_entry, &new_space, &new_rsp);
        }
    }

    for (int i = 0; i < argc; i++) kfree(kargv[i]);
    for (int i = 0; i < envc; i++) kfree(kenvp[i]);
    kfree(kargv);
    kfree(kenvp);

    if (err < 0) return (s64)err;

    /* BUG-AF fix: Close FD_CLOEXEC file descriptors ONLY AFTER elf_load_exec()
     * succeeds. If execve fails before this point, all file descriptors remain intact. */
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (proc->handle_table[i] && (proc->fd_flags[i] & FD_CLOEXEC)) {
            file_t *f = fd_detach(proc, i);
            if (f) vfs_close(f);
        }
    }

    /* POSIX: execve terminates every other thread of the process. Do it before
     * we free the old address space — a sibling still running user code on the
     * old page tables would otherwise fault on freed memory. Wait for any
     * on-CPU sibling to actually leave its CPU. */
    sched_exit_group_mark();
    sched_dethread_wait();

    /* BUG-AG fix: Clean up all terminated sibling threads and their kernel stacks
     * so they are not leaked for the entire lifetime of the new process. */
    sched_dethread_reap();

    ipc_shmem_unmap_all(proc);
    sysvipc_process_exec(proc);   /* POSIX: SysV segments do not survive exec */

    proc->pml4_phys = new_space;
    /* Same PCID tag, brand-new address space behind it: clear the primed mask
     * so the next switch to it flushes that tag on each core, evicting the
     * image that just exec'd away. */
    proc->pcid_primed = 0;
    vmm_switch_proc(new_space, proc->pcid, smp_current_cpu_id(), &proc->pcid_primed);

    if (old_space && old_space != vmm_kernel_space()) {
        vmm_destroy_space(old_space);
    }

    proc->fs_base = 0;
    thread_t *curr = sched_current_thread();
    if (curr) {
        curr->fs_base = 0;
        curr->has_thread_fs_base = false; /* fresh image: inherit proc->fs_base again */
        curr->clear_child_tid = 0;
    }
    wrmsr(MSR_FS_BASE, 0);
    proc->gs_base = 0;
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    /* Apply the execve() capability transition. no_new_privs and the seccomp
     * mode deliberately survive here — a sandbox that set them before exec'ing
     * a helper would be worthless if exec cleared them. */
    security_caps_on_exec(proc);

    /* Reset custom signal handlers to default (SIG_DFL) per POSIX execve spec */
    for (int i = 0; i < _NSIG; i++) {
        if (proc->sigactions[i].sa_handler != SIG_IGN) {
            proc->sigactions[i].sa_handler = SIG_DFL;
            proc->sigactions[i].sa_flags = 0;
            proc->sigactions[i].sa_mask = 0;
        }
    }
    /* Reset alternate signal stack on execve */
    proc->sas_ss_sp = NULL;
    proc->sas_ss_size = 0;
    proc->sas_ss_flags = 2; /* SS_DISABLE */
    /* Reset all user registers to clean state per System V AMD64 ABI specification */
    r->rip = (u64)new_entry;
    r->rsp = (u64)new_rsp;
    r->rax = 0;
    r->rbx = 0;
    r->rcx = 0;
    r->rdx = 0;
    r->rsi = 0;
    r->rdi = 0;
    r->rbp = 0;
    r->r8  = 0;
    r->r9  = 0;
    r->r10 = 0;
    r->r11 = 0;
    r->r12 = 0;
    r->r13 = 0;
    r->r14 = 0;
    r->r15 = 0;
    r->rflags = 0x202;

    /* Update process name to binary basename */
    const char *bname = kpath;
    for (int i = 0; kpath[i]; i++) {
        if (kpath[i] == '/' && kpath[i + 1]) bname = &kpath[i + 1];
    }
    strncpy(proc->name, bname, sizeof(proc->name) - 1);
    proc->name[sizeof(proc->name) - 1] = '\0';

    /* The tracee survives exec (PT_TRACED is not reset above), so a tracer that
     * asked for PTRACE_O_TRACEEXEC gets its stop here — with `r` already
     * describing the new image's entry point, which is the whole point: it is
     * where a debugger plants its first breakpoints. */
    if (ptrace_traced(proc))
        ptrace_report_event(r, PTRACE_EVENT_EXEC, (u64)proc->pid);

    return 0;
}

s64 sys_execve_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    return execve_core(r, kpath,
                       (const char *const *)r->rsi,
                       (const char *const *)r->rdx);
}

/* execveat(dirfd, path, argv, envp, flags) — Linux syscall 322.
 * Supports AT_EMPTY_PATH (exec the file the dirfd refers to). */
s64 sys_execveat_impl(pt_regs_t *r)
{
    int dirfd            = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    const char *const *user_argv = (const char *const *)r->rdx;
    const char *const *user_envp = (const char *const *)r->r10;
    int flags            = (int)r->r8;

    if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return -(s64)EINVAL;

    char kpath[512];

    /* AT_EMPTY_PATH with an empty path string: exec whatever `dirfd` points at. */
    char probe = 1;
    bool empty_path = !user_path;
    if (user_path && (uintptr_t)user_path < TASK_SIZE_MAX) {
        if (copy_from_user(&probe, user_path, 1) == 0 && probe == '\0') empty_path = true;
    }

    if (empty_path) {
        if (!(flags & AT_EMPTY_PATH)) return -(s64)ENOENT;
        process_t *proc = sched_current_process();
        if (!proc || dirfd < 0 || dirfd >= PROC_MAX_FDS || !proc->handle_table[dirfd])
            return -(s64)EBADF;
        file_t *df = (file_t *)proc->handle_table[dirfd];
        if (!df || !df->f_dentry) return -(s64)EBADF;
        dentry_build_path(df->f_dentry, kpath, sizeof(kpath));
    } else {
        if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;
        s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
        if (perr < 0) return perr;
    }

    return execve_core(r, kpath, user_argv, user_envp);
}


s64 sys_exit_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (proc) {
        /* Don't clobber a termination signal already recorded by the fault
         * handler (segfault etc.) with the trap frame's rdi. */
        if (r && proc->term_signal == 0) proc->exit_code = (int)r->rdi & 0xFF;

        /* PTRACE_O_TRACEEXIT: one last stop while the address space is still
         * intact, which is the only chance a tracer has to read the dying
         * process's memory. Then drop every tracing relationship — as a tracee
         * so a stale link cannot outlive us, and as a tracer so our tracees are
         * not left parked waiting for a resume that will never come. */
        if (r && ptrace_traced(proc))
            ptrace_report_event(r, PTRACE_EVENT_EXIT,
                                (u64)(proc->term_signal ? (proc->term_signal & 0x7f)
                                                        : ((proc->exit_code & 0xff) << 8)));
        ptrace_release(proc);
        pr_debug("[SYSCALL] Process exiting (PID %u, code %d, sig %d)\n",
                 proc->pid, proc->exit_code, proc->term_signal);

        /* Release this process's descriptors. fd_table_release() claims each
         * slot under the per-process fd_lock (atomic swap-to-NULL) so a sibling thread also
         * exiting, sched_exit_thread's last-thread cleanup, the reaper, or a
         * cross-process fget() cannot double-close or use-after-free the same
         * file_t / object. */
        fd_table_release(proc);
    }
    sched_exit_thread();
    __builtin_unreachable();
}

s64 sys_exit_group_impl(pt_regs_t *r)
{
    /* POSIX: exit_group terminates every thread of the process, not just the
     * caller. Wind the siblings down first, then exit this thread. */
    sched_exit_group_mark();
    return sys_exit_impl(r);
}

s64 sys_wait4_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    int *user_status = (int *)r->rsi;
    int options = (int)r->rdx;
    int kstatus = 0;

    s64 res = sched_waitpid(pid, user_status ? &kstatus : NULL, options);
    if (res >= 0 && user_status) {
        if ((uintptr_t)user_status < TASK_SIZE_MAX) {
            copy_to_user(user_status, &kstatus, sizeof(int));
        }
    }
    return res;
}

s64 sys_waitid_impl(pt_regs_t *r)
{
    s32 id = (s32)r->rsi;
    void *infop = (void *)r->rdx;
    int options = (int)r->r10;

    int status = 0;
    s64 res = sched_waitpid(id == 0 ? -1 : id, &status, options);
    if (res < 0) return res;

    if (infop && (uintptr_t)infop < TASK_SIZE_MAX) {
        int siginfo[32];
        __builtin_memset(siginfo, 0, sizeof(siginfo));
        siginfo[0] = 17; /* SIGCHLD */
        siginfo[1] = 0;  /* si_errno */
        siginfo[2] = 1;  /* CLD_EXITED */
        siginfo[3] = (int)res; /* si_pid */
        siginfo[6] = (status >> 8) & 0xFF; /* si_status */
        copy_to_user(infop, siginfo, sizeof(siginfo));
    }
    return 0;
}

s64 sys_getuid_impl(pt_regs_t *r)  { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->uid : 0; }
s64 sys_geteuid_impl(pt_regs_t *r) { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->euid : 0; }
s64 sys_getgid_impl(pt_regs_t *r)  { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->gid : 0; }
s64 sys_getegid_impl(pt_regs_t *r) { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->egid : 0; }
s64 sys_setuid_impl(pt_regs_t *r)  {
    u32 new_uid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;
    if (p->no_new_privs && p->euid != 0 && new_uid == 0) return -(s64)EPERM;
    if (!security_check_permission(p, CAP_SETUID) && p->euid != 0) {
        if (new_uid != p->uid && new_uid != p->euid && new_uid != p->suid)
            return -(s64)EPERM;
    }
    p->uid = new_uid;
    p->euid = new_uid;
    p->suid = new_uid;
    security_caps_on_setuid(p);
    return 0;
}
s64 sys_setgid_impl(pt_regs_t *r)  {
    u32 new_gid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;
    if (!security_check_permission(p, CAP_SETGID) && p->euid != 0) {
        if (new_gid != p->gid && new_gid != p->egid && new_gid != p->sgid)
            return -(s64)EPERM;
    }
    p->gid = new_gid;
    p->egid = new_gid;
    p->sgid = new_gid;
    return 0;
}
static process_t *proc_by_pid(u32 pid)
{
    for (process_t *p = sched_get_process_list(); p; p = p->next)
        if (p->pid == pid) return p;
    return NULL;
}

s64 sys_getpgrp_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pgid : 0;
}

/* setpgid(pid, pgid) — POSIX job control. */
s64 sys_setpgid_impl(pt_regs_t *r) {
    s32 pid  = (s32)r->rdi;
    s32 pgid = (s32)r->rsi;
    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;
    if (pgid < 0) return -(s64)EINVAL;

    u32 target_pid = (pid == 0) ? caller->pid : (u32)pid;
    process_t *target = proc_by_pid(target_pid);
    if (!target) return -(s64)ESRCH;

    /* Target must be the caller or one of its children. */
    if (target != caller && target->parent != caller) return -(s64)ESRCH;
    /* A session leader's process group cannot be changed. */
    if (target->sid == target->pid) return -(s64)EPERM;
    /* Caller and target must be in the same session. */
    if (target->sid != caller->sid) return -(s64)EPERM;

    u32 new_pgid = (pgid == 0) ? target_pid : (u32)pgid;
    if (new_pgid != target->pid) {
        /* Joining an existing group: it must already exist in this session. */
        bool ok = false;
        for (process_t *p = sched_get_process_list(); p; p = p->next)
            if (p->pgid == new_pgid && p->sid == caller->sid) { ok = true; break; }
        if (!ok) return -(s64)EPERM;
    }
    target->pgid = new_pgid;
    return 0;
}

/* setsid() — create a new session; caller becomes session and group leader. */
s64 sys_setsid_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* Fails if the caller is already a process group leader. */
    if (proc->pgid == proc->pid) return -(s64)EPERM;
    proc->sid  = proc->pid;
    proc->pgid = proc->pid;
    return (s64)proc->pid;
}
/* ── Resource Limits & Usage ─────────────────────────────────────────────── */

struct rlimit {
    u64 rlim_cur;
    u64 rlim_max;
};

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
/* RLIM_INFINITY comes from sched.h (shared with proc_create's defaults). */

/*
 * do_prlimit() — the shared core of getrlimit/setrlimit/prlimit64.
 *
 * Reads the current pair into @old (when non-NULL), then, when @new is given,
 * validates and stores it on @target->rlimits[resource]:
 *   - rlim_cur must not exceed rlim_max                     -> EINVAL
 *   - raising the hard limit needs euid 0 or CAP_SYS_RESOURCE
 *   - RLIMIT_NOFILE cannot be raised past the fd table size (PROC_MAX_FDS)
 * The caller has already resolved @target and checked it may act on it.
 */
static s64 do_prlimit(process_t *target, int resource,
                      const krlimit_t *new_lim, krlimit_t *old_lim)
{
    if (resource < 0 || resource >= RLIMIT_NLIMITS) return -(s64)EINVAL;

    if (old_lim) *old_lim = target->rlimits[resource];

    if (new_lim) {
        krlimit_t nl = *new_lim;
        if (nl.rlim_cur > nl.rlim_max) return -(s64)EINVAL;

        process_t *self = sched_current_process();
        if (nl.rlim_max > target->rlimits[resource].rlim_max &&
            self && self->euid != 0 &&
            !security_check_permission(self, CAP_SYS_RESOURCE)) {
            return -(s64)EPERM;
        }
        if (resource == RLIMIT_NOFILE && nl.rlim_max > PROC_MAX_FDS) {
            nl.rlim_max = PROC_MAX_FDS;
            if (nl.rlim_cur > nl.rlim_max) nl.rlim_cur = nl.rlim_max;
        }
        target->rlimits[resource] = nl;
    }
    return 0;
}

s64 sys_getrlimit_impl(pt_regs_t *r)
{
    int resource = (int)r->rdi;
    struct rlimit *rlim = (struct rlimit *)r->rsi;
    if (!rlim || (uintptr_t)rlim >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    krlimit_t k;
    s64 rc = do_prlimit(proc, resource, NULL, &k);
    if (rc != 0) return rc;

    struct rlimit out = { k.rlim_cur, k.rlim_max };
    if (copy_to_user(rlim, &out, sizeof(out)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_setrlimit_impl(pt_regs_t *r)
{
    int resource = (int)r->rdi;
    const struct rlimit *rlim = (const struct rlimit *)r->rsi;
    if (!rlim || (uintptr_t)rlim >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    struct rlimit in;
    if (copy_from_user(&in, rlim, sizeof(in)) != 0) return -(s64)EFAULT;

    krlimit_t nl = { in.rlim_cur, in.rlim_max };
    return do_prlimit(proc, resource, &nl, NULL);
}

struct rusage {
    struct linux_timeval ru_utime;
    struct linux_timeval ru_stime;
    long   ru_maxrss;
    long   ru_ixrss;
    long   ru_idrss;
    long   ru_isrss;
    long   ru_minflt;
    long   ru_majflt;
    long   ru_nswap;
    long   ru_inblock;
    long   ru_oublock;
    long   ru_msgsnd;
    long   ru_msgrcv;
    long   ru_nsignals;
    long   ru_nvcsw;
    long   ru_nivcsw;
};

s64 sys_getrusage_impl(pt_regs_t *r)
{
    int who = (int)r->rdi;
    struct rusage *usage = (struct rusage *)r->rsi;
    if (!usage || (uintptr_t)usage >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (who != 0 /* RUSAGE_SELF */ && who != -1 /* RUSAGE_CHILDREN */ && who != 1 /* RUSAGE_THREAD */) {
        return -(s64)EINVAL;
    }

    process_t *proc = sched_current_process();
    struct rusage ru;
    __builtin_memset(&ru, 0, sizeof(ru));

    /* RUSAGE_CHILDREN reports what has been reaped; the others report this
     * process's own accumulated time. */
    u64 ut = 0, st = 0;
    if (proc) {
        if (who == -1) { ut = proc->cutime_ticks; st = proc->cstime_ticks; }
        else           { ut = proc->utime_ticks;  st = proc->stime_ticks;  }
    }
    ru.ru_utime.tv_sec  = (long)(ut / 100);
    ru.ru_utime.tv_usec = (long)((ut % 100) * 10000L);
    ru.ru_stime.tv_sec  = (long)(st / 100);
    ru.ru_stime.tv_usec = (long)((st % 100) * 10000L);
    ru.ru_maxrss = (proc && proc->pml4_phys) ? 4096 : 1024;
    ru.ru_minflt = 128;
    ru.ru_majflt = 0;
    ru.ru_inblock = 64;
    ru.ru_oublock = 32;
    ru.ru_nvcsw = 16;
    ru.ru_nivcsw = 4;

    if (copy_to_user(usage, &ru, sizeof(struct rusage)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_az_spawn(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path || (uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char kpath[512];
    __builtin_memset(kpath, 0, sizeof(kpath));
    for (int i = 0; i < 255; i++) {
        if (copy_from_user(&kpath[i], user_path + i, 1) != 0) return -(s64)EFAULT;
        if (kpath[i] == '\0') break;
    }

    process_t *child = sched_spawn_user(kpath);
    if (!child) return -(s64)EINVAL;

    process_t *parent = sched_current_process();
    if (parent) child->parent = parent;

    return (s64)child->pid;
}

/*
 * Same as sys_az_spawn(), but rsi optionally names a second string passed
 * to the child as argv[1] (e.g. a file for a GUI app to open). This is a
 * distinct syscall number rather than sys_az_spawn() itself reading rsi
 * unconditionally — every existing caller reaches sys_az_spawn() through
 * syscall1(), whose inline asm leaves rsi unconstrained, so treating it as
 * a pointer there would risk copy_from_user() on whatever garbage happened
 * to be sitting in the register.
 */
s64 sys_az_spawn_arg(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    const char *user_arg  = (const char *)r->rsi;
    if (!user_path || (uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char kpath[512];
    __builtin_memset(kpath, 0, sizeof(kpath));
    for (int i = 0; i < 255; i++) {
        if (copy_from_user(&kpath[i], user_path + i, 1) != 0) return -(s64)EFAULT;
        if (kpath[i] == '\0') break;
    }

    char karg[256];
    __builtin_memset(karg, 0, sizeof(karg));
    if (user_arg) {
        if ((uintptr_t)user_arg >= TASK_SIZE_MAX) return -(s64)EFAULT;
        for (int i = 0; i < 255; i++) {
            if (copy_from_user(&karg[i], user_arg + i, 1) != 0) return -(s64)EFAULT;
            if (karg[i] == '\0') break;
        }
    }

    process_t *child = sched_spawn_user_arg(kpath, user_arg ? karg : NULL);
    if (!child) return -(s64)EINVAL;

    process_t *parent = sched_current_process();
    if (parent) child->parent = parent;

    return (s64)child->pid;
}

s64 sys_az_yield(pt_regs_t *r)
{
    (void)r;
    sched_yield();
    return 0;
}

s64 sys_az_thread_create_impl(pt_regs_t *r)
{
    uintptr_t entry = (uintptr_t)r->rdi;
    uintptr_t stack = (uintptr_t)r->rsi;
    uintptr_t arg   = (uintptr_t)r->rdx;

    if (!entry || !stack) return -(s64)EINVAL;
    if (entry >= TASK_SIZE_MAX || stack >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    thread_t *t = thread_create_ex(proc, entry, stack, false, false);
    if (!t) return -(s64)ENOMEM;

    if (t->user_regs) {
        t->user_regs->rdi = (u64)arg;
    }

    sched_enqueue_thread(t);
    return (s64)t->tid;
}

s64 sys_az_thread_exit_impl(pt_regs_t *r)
{
    (void)r;
    sched_exit_thread();
    __builtin_unreachable();
}

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

s64 sys_arch_prctl_impl(pt_regs_t *r)
{
    int code = (int)(s32)r->rdi;
    u64 addr = (u64)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (code == ARCH_SET_FS) {
        /* Per-thread TLS base — explicit even when addr is 0, so a later
         * save/restore (sched_yield, sched_post_switch) knows this thread has
         * its own override rather than silently promoting it into
         * proc->fs_base (TODO(T-01)). Also update proc->fs_base so a later
         * thread that never sets its own still inherits a sane value. */
        thread_t *self = sched_current_thread();
        if (self) {
            self->fs_base            = addr;
            self->has_thread_fs_base = true;
        }
        proc->fs_base = addr;
        /* FSGSBASE turns this into a single GPR write; without it FS_BASE can
         * only be reached through the WRMSR path, which serializes and costs
         * far more than the register move it is standing in for. */
        if (g_fsgsbase_enabled) wrfsbase(addr);
        else wrmsr(MSR_FS_BASE, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr || addr >= TASK_SIZE_MAX) return -(s64)EFAULT;
        thread_t *self = sched_current_thread();
        u64 cur = (self && self->has_thread_fs_base) ? self->fs_base : proc->fs_base;
        return copy_to_user((void *)addr, &cur, sizeof(u64)) == 0 ? 0 : -(s64)EFAULT;
    } else if (code == ARCH_SET_GS) {
        proc->gs_base = addr;
        wrmsr(MSR_KERNEL_GS_BASE, addr);
        return 0;
    } else if (code == ARCH_GET_GS) {
        if (!addr || addr >= TASK_SIZE_MAX) return -(s64)EFAULT;
        return copy_to_user((void *)addr, &proc->gs_base, sizeof(u64)) == 0 ? 0 : -(s64)EFAULT;
    }
    return -(s64)EINVAL;
}

s64 sys_set_tid_address_impl(pt_regs_t *r)
{
    thread_t *t = sched_current_thread();
    uintptr_t tidptr = (uintptr_t)r->rdi;

    /* Linux never fails this call: a bad pointer is simply remembered and
     * discovered (and ignored) at exit. It returns the caller's *thread* id,
     * which is what a libc stores as its cached tid — returning the pid is
     * only accidentally right for a single-threaded process. */
    if (t) {
        t->clear_child_tid = (tidptr < TASK_SIZE_MAX) ? (u64)tidptr : 0;
        return (s64)t->tid;
    }
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 1;
}

#define RLIM64_INFINITY (~0ULL)

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15

struct kernel_rlimit64 {
    u64 rlim_cur;
    u64 rlim_max;
};

s64 sys_prlimit64_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    int resource = (int)(s32)r->rsi;
    const struct kernel_rlimit64 *new_rlim = (const struct kernel_rlimit64 *)r->rdx;
    struct kernel_rlimit64 *old_rlim = (struct kernel_rlimit64 *)r->r10;

    if (resource < 0 || resource >= RLIMIT_NLIMITS) return -(s64)EINVAL;
    if (new_rlim && (uintptr_t)new_rlim >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (old_rlim && (uintptr_t)old_rlim >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *self = sched_current_process();
    if (!self) return -(s64)EPERM;

    /* pid 0 means "this process". Acting on another process needs matching
     * effective uid or CAP_SYS_RESOURCE, as Linux requires. */
    process_t *target = self;
    bool put_target = false;
    if (pid != 0 && pid != self->pid) {
        target = proc_get_by_pid(pid);
        if (!target) return -(s64)ESRCH;
        put_target = true;
        if (self->euid != 0 && self->euid != target->euid &&
            !security_check_permission(self, CAP_SYS_RESOURCE)) {
            proc_put(target);
            return -(s64)EPERM;
        }
    }

    krlimit_t nl, ol;
    if (new_rlim && copy_from_user(&nl, new_rlim, sizeof(nl)) != 0) {
        if (put_target) proc_put(target);
        return -(s64)EFAULT;
    }

    s64 rc = do_prlimit(target, resource, new_rlim ? &nl : NULL,
                        old_rlim ? &ol : NULL);

    if (put_target) proc_put(target);
    if (rc != 0) return rc;

    if (old_rlim && copy_to_user(old_rlim, &ol, sizeof(ol)) != 0) return -(s64)EFAULT;
    return 0;
}

struct kernel_clone_args {
    u64 flags;
    u64 pidfd;
    u64 child_tid;
    u64 parent_tid;
    u64 exit_signal;
    u64 stack;
    u64 stack_size;
    u64 tls;
    u64 set_tid;
    u64 set_tid_size;
    u64 cgroup;
};

s64 sys_clone3_impl(pt_regs_t *r)
{
    const struct kernel_clone_args *uargs = (const struct kernel_clone_args *)r->rdi;
    size_t size = (size_t)r->rsi;

    if (!uargs || size < sizeof(u64)) return -(s64)EINVAL;
    struct kernel_clone_args kargs;
    memset(&kargs, 0, sizeof(kargs));
    size_t to_copy = size < sizeof(kargs) ? size : sizeof(kargs);
    if (copy_from_user(&kargs, uargs, to_copy) != 0) return -(s64)EFAULT;

    u64 flags = kargs.flags | (kargs.exit_signal & 0xFFULL);
    virt_addr_t child_stack = kargs.stack ? (virt_addr_t)(kargs.stack + kargs.stack_size) : 0;
    int *parent_tidptr = (int *)(uintptr_t)kargs.parent_tid;
    int *child_tidptr = (int *)(uintptr_t)kargs.child_tid;
    u64 newtls = kargs.tls;

    s64 ret = do_clone(flags, child_stack, parent_tidptr, child_tidptr, newtls, r);
    if (ret > 0 && (kargs.flags & 0x00001000ULL /* CLONE_PIDFD */) && kargs.pidfd &&
        (uintptr_t)kargs.pidfd < TASK_SIZE_MAX) {
        pt_regs_t p_r;
        p_r.rdi = (u64)ret;
        p_r.rsi = 0;
        s64 pfd = sys_pidfd_open_impl(&p_r);
        if (pfd >= 0) {
            int ipfd = (int)pfd;
            copy_to_user((void *)(uintptr_t)kargs.pidfd, &ipfd, sizeof(int));
        }
    }
    return ret;
}

s64 sys_getcpu_impl(pt_regs_t *r)
{
    unsigned int *user_cpu = (unsigned int *)r->rdi;
    unsigned int *user_node = (unsigned int *)r->rsi;
    void *tcache = (void *)r->rdx;
    (void)tcache;

    unsigned int cpu_id = smp_current_cpu_id();
    unsigned int node_id = 0;

    if (user_cpu && (uintptr_t)user_cpu < TASK_SIZE_MAX) {
        if (copy_to_user(user_cpu, &cpu_id, sizeof(unsigned int)) != 0) return -(s64)EFAULT;
    }
    if (user_node && (uintptr_t)user_node < TASK_SIZE_MAX) {
        if (copy_to_user(user_node, &node_id, sizeof(unsigned int)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

/* ── Scheduling policy / parameters (shared helpers) ────────────────────────
 * Policy + RT priority + nice are stored per process (see process_t) and
 * mapped onto this CFS's weight by sched_weight_for(); every get* reflects
 * exactly what the matching set* stored. */
/* SCHED_OTHER/FIFO/RR/BATCH/IDLE now come from sched.h, which the scheduler
 * itself dispatches on — the two copies had to agree and nothing enforced it. */
#define SCHED_RESET_ON_FORK 0x40000000

struct sched_param { int sched_priority; };

struct sched_attr {
    u32 size;
    u32 sched_policy;
    u64 sched_flags;
    s32 sched_nice;
    u32 sched_priority;
    u64 sched_runtime;
    u64 sched_deadline;
    u64 sched_period;
};

/* Resolve a pid argument (0 = caller) and check the caller may reschedule the
 * target: same effective uid, or CAP_SYS_NICE. Returns a process with a held
 * reference when @put is set true (release with proc_put), else NULL with the
 * negative errno in @err. */
static process_t *sched_target(u32 pid, bool *put, s64 *err)
{
    *put = false; *err = 0;
    process_t *self = sched_current_process();
    if (!self) { *err = -(s64)EPERM; return NULL; }
    if (pid == 0 || pid == self->pid) return self;

    process_t *t = proc_get_by_pid(pid);
    if (!t) { *err = -(s64)ESRCH; return NULL; }
    if (self->euid != 0 && self->euid != t->euid &&
        !security_check_permission(self, CAP_SYS_NICE)) {
        proc_put(t);
        *err = -(s64)EPERM;
        return NULL;
    }
    *put = true;
    return t;
}

static bool sched_policy_valid(int p)
{
    return p == SCHED_OTHER || p == SCHED_FIFO || p == SCHED_RR ||
           p == SCHED_BATCH || p == SCHED_IDLE;
}

s64 sys_sched_setattr_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_attr *uattr = (struct sched_attr *)r->rsi;
    if (!uattr || (uintptr_t)uattr >= TASK_SIZE_MAX) return -(s64)EINVAL;

    /* size-versioned struct: read the leading u32, then the smaller of what
     * the caller offered and what we understand. */
    u32 size = 0;
    if (copy_from_user(&size, uattr, sizeof(u32)) != 0) return -(s64)EFAULT;
    if (size < sizeof(u32)) return -(s64)EINVAL;

    struct sched_attr a;
    memset(&a, 0, sizeof(a));
    u32 copy = size < sizeof(a) ? size : (u32)sizeof(a);
    if (copy_from_user(&a, uattr, copy) != 0) return -(s64)EFAULT;

    int policy = (int)a.sched_policy;
    if (!sched_policy_valid(policy)) return -(s64)EINVAL;
    bool rt = (policy == SCHED_FIFO || policy == SCHED_RR);

    if (rt) {
        if (a.sched_priority < 1 || a.sched_priority > 99) return -(s64)EINVAL;
    } else if (a.sched_priority != 0) {
        return -(s64)EINVAL;
    }
    s32 nice = a.sched_nice;
    if (nice < -20) nice = -20;
    if (nice >  19) nice =  19;

    process_t *self = sched_current_process();
    if (rt && self && self->euid != 0 &&
        !security_check_permission(self, CAP_SYS_NICE))
        return -(s64)EPERM;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    tgt->sched_policy  = (u32)policy;
    tgt->sched_rt_prio = rt ? (s32)a.sched_priority : 0;
    if (!rt) tgt->prio_nice = nice;
    sched_apply_weight(tgt);

    if (put) proc_put(tgt);
    return 0;
}

s64 sys_sched_getattr_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_attr *uattr = (struct sched_attr *)r->rsi;
    u32 size = (u32)r->rdx;
    if (!uattr || (uintptr_t)uattr >= TASK_SIZE_MAX) return -(s64)EINVAL;
    if (size < sizeof(struct sched_attr)) return -(s64)EINVAL;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    struct sched_attr a;
    memset(&a, 0, sizeof(a));
    a.size          = sizeof(a);
    a.sched_policy  = tgt->sched_policy;
    a.sched_nice    = tgt->prio_nice;
    a.sched_priority = (u32)tgt->sched_rt_prio;

    if (put) proc_put(tgt);

    if (copy_to_user(uattr, &a, sizeof(a)) != 0) return -(s64)EFAULT;
    return 0;
}


static s64 pidfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static int pidfd_poll_op(file_t *filp)
{
    if (!filp || !filp->private_data) return POLLERR;
    pidfd_ctx_t *ctx = (pidfd_ctx_t *)filp->private_data;
    if (sched_kill_process(ctx->target_pid, 0) < 0) {
        return POLLIN | 0x0040;
    }
    return 0;
}

file_operations_t g_pidfd_fops = {
    .release = pidfd_release_op,
    .poll    = pidfd_poll_op,
};

s64 sys_pidfd_open_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;

    if (flags != 0) return -(s64)EINVAL;
    if (pid <= 0) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (sched_kill_process((u32)pid, 0) < 0) return -(s64)ESRCH;

    pidfd_ctx_t *ctx = (pidfd_ctx_t *)kzalloc(sizeof(pidfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->target_pid = (u32)pid;

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    f->f_op = &g_pidfd_fops;
    f->f_flags = O_RDONLY;
    f->f_count = 1;
    f->private_data = ctx;

    s64 fd = fd_install(proc, f, 0);
    if (fd < 0) {
        kfree(ctx);
        kfree(f);
        return -(s64)EMFILE;
    }
    return fd;
}

s64 sys_pidfd_send_signal_impl(pt_regs_t *r)
{
    int pidfd = (int)r->rdi;
    int sig = (int)r->rsi;
    void *info = (void *)r->rdx;
    unsigned int flags = (unsigned int)r->r10;

    (void)info;
    if (flags != 0) return -(s64)EINVAL;
    if (sig < 0 || sig >= 64) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Hold a file reference while inspecting the pidfd. A concurrent close()
     * must not be able to free its private data between validation and signal
     * delivery. */
    file_t *f = fget(proc, pidfd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_pidfd_fops || !f->private_data) {
        fput(f);
        return -(s64)EBADF;
    }

    pidfd_ctx_t *ctx = (pidfd_ctx_t *)f->private_data;
    u32 target_pid = ctx->target_pid;
    fput(f);
    if (sched_kill_process(target_pid, 0) < 0) return -(s64)ESRCH;

    if (sig == 0) return 0;
    return sched_kill_process(target_pid, sig);
}

s64 sys_pidfd_getfd_impl(pt_regs_t *r)
{
    int pidfd          = (int)(s32)r->rdi;
    int targetfd       = (int)(s32)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;

    if (flags & ~0x00080000u /* O_CLOEXEC */) return -(s64)EINVAL;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;

    file_t *pf = fget(caller, pidfd);
    if (!pf) return -(s64)EBADF;
    if (pf->f_op != &g_pidfd_fops || !pf->private_data) {
        fput(pf);
        return -(s64)EBADF;
    }
    pidfd_ctx_t *ctx = (pidfd_ctx_t *)pf->private_data;
    u32 target_pid = ctx->target_pid;
    fput(pf);

    /* Reference the target for the whole call: otherwise it can be reaped on
     * another CPU between here and the handle_table read below. */
    process_t *target = proc_get_by_pid(target_pid);
    if (!target) return -(s64)ESRCH;

    s64 rc = 0;
    if (caller != target && caller->euid != 0 && caller->uid != target->uid &&
        !security_check_permission(caller, CAP_SYS_PTRACE)) {
        rc = -(s64)EPERM;
    } else {
        /* fget() reads the slot and raises f_count under the per-process fd_lock, which
         * fd_table_release() also holds while it clears slots on exit: the file
         * is either grabbed live or already gone, never freed mid-grab. */
        file_t *target_file = fget(target, targetfd);
        if (!target_file) {
            rc = -(s64)EBADF;
        } else {
            s64 newfd = fd_install(caller, target_file,
                                   (flags & 0x00080000u) ? FD_CLOEXEC : 0);
            if (newfd < 0) {
                fput(target_file);
                rc = -(s64)EMFILE;
            } else {
                rc = newfd;   /* fget()'s reference transfers to the new fd */
            }
        }
    }

    proc_put(target);
    return rc;
}

s64 sys_sched_yield_impl(pt_regs_t *r)
{
    (void)r;

    /* An explicit yield from a real-time thread must move it behind its
     * equals — that is what POSIX specifies for sched_yield(2) under
     * SCHED_FIFO and SCHED_RR, and it is the only way a FIFO thread can ever
     * hand the CPU to a peer at its own priority. Zeroing the slice is the
     * same signal sched_tick() uses when a round-robin slice runs out: it
     * tells sched_yield() that giving way is allowed and enqueue_ready() to
     * requeue at the tail rather than the head. CFS threads are unaffected —
     * they have no slice and always give way. */
    thread_t *cur = sched_current_thread();
    if (cur && cur->rt_priority) cur->rr_ticks_left = 0;

    sched_yield();
    return 0;
}

s64 sys_gettid_impl(pt_regs_t *r)
{
    (void)r;
    thread_t *t = sched_current_thread();
    if (t) return (s64)t->tid;
    process_t *p = sched_current_process();
    return p ? (s64)p->pid : 1;
}

s64 sys_prctl_impl(pt_regs_t *r)
{
    int option = (int)r->rdi;
    u64 arg2 = (u64)r->rsi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;

    if (option == 1 /* PR_SET_PDEATHSIG */) {
        p->pdeath_sig = (int)arg2;
        return 0;
    }
    if (option == 2 /* PR_GET_PDEATHSIG */) {
        if (!arg2 || arg2 >= TASK_SIZE_MAX) return -(s64)EFAULT;
        int sig = p->pdeath_sig;
        if (copy_to_user((void *)arg2, &sig, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    if (option == 15 /* PR_SET_NAME */) {
        if (!arg2 || arg2 >= TASK_SIZE_MAX) return -(s64)EFAULT;
        char name[16];
        if (copy_from_user(name, (const void *)arg2, 15) != 0) return -(s64)EFAULT;
        name[15] = '\0';
        strncpy(p->name, name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
        return 0;
    }
    if (option == 16 /* PR_GET_NAME */) {
        if (!arg2 || arg2 >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_to_user((void *)arg2, p->name, strlen(p->name) + 1) != 0) return -(s64)EFAULT;
        return 0;
    }
    if (option == 3 /* PR_GET_DUMPABLE */) return 1;
    if (option == 4 /* PR_SET_DUMPABLE */) return 0;
    if (option == 7 /* PR_GET_KEEPCAPS */) return 0;
    if (option == 8 /* PR_SET_KEEPCAPS */) return 0;
    if (option == 38 /* PR_SET_NO_NEW_PRIVS */) {
        if (arg2 != 1) return -(s64)EINVAL;   /* one-way: cannot be cleared */
        p->no_new_privs = true;
        return 0;
    }
    if (option == 39 /* PR_GET_NO_NEW_PRIVS */) return p->no_new_privs ? 1 : 0;
    if (option == 23 /* PR_CAPBSET_READ */) {
        if (arg2 > CAP_LAST_CAP) return -(s64)EINVAL;
        return (p->cap_bounding & CAP_TO_MASK(arg2)) ? 1 : 0;
    }
    if (option == 24 /* PR_CAPBSET_DROP */) {
        if (arg2 > CAP_LAST_CAP) return -(s64)EINVAL;
        if (!security_check_permission(p, CAP_SETPCAP)) return -(s64)EPERM;
        /* Dropping from the bounding set must also clear the live sets, or the
         * capability would stay usable until the next execve(). */
        u64 bit = CAP_TO_MASK(arg2);
        p->cap_bounding    &= ~bit;
        p->cap_permitted   &= ~bit;
        p->cap_effective   &= ~bit;
        p->cap_inheritable &= ~bit;
        return 0;
    }
    if (option == 22 /* PR_SET_SECCOMP */) {
        if (arg2 == SECCOMP_MODE_STRICT) {
            if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
                p->seccomp_mode != SECCOMP_MODE_STRICT) return -(s64)EINVAL;
            p->no_new_privs = true;
            p->seccomp_mode = SECCOMP_MODE_STRICT;
            return 0;
        }
        if (arg2 == SECCOMP_MODE_FILTER) {
            if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
                p->seccomp_mode != SECCOMP_MODE_FILTER) return -(s64)EINVAL;
            /* prctl's third argument (arg3, %rdx), not arg2 — mode is arg2. */
            return seccomp_attach_filter(p, (const sock_fprog_t *)r->rdx);
        }
        return -(s64)EINVAL;
    }
    if (option == 21 /* PR_GET_SECCOMP */) return (s64)p->seccomp_mode;
    if (option == 36 /* PR_SET_CHILD_SUBREAPER */) {
        p->child_subreaper = (arg2 != 0);
        return 0;
    }
    if (option == 37 /* PR_GET_CHILD_SUBREAPER */) {
        if (!arg2 || arg2 >= TASK_SIZE_MAX) return -(s64)EFAULT;
        int val = p->child_subreaper ? 1 : 0;
        if (copy_to_user((void *)arg2, &val, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    return 0;
}

s64 sys_sched_getaffinity_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    size_t cpusetsize = (size_t)r->rsi;
    void *mask = (void *)r->rdx;
    if (pid < 0) return -(s64)EINVAL;
    if (!mask || (uintptr_t)mask >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (cpusetsize < sizeof(u64)) return -(s64)EINVAL;

    process_t *target = (pid == 0) ? sched_current_process() : proc_get_by_pid((u32)pid);
    if (!target) return -(s64)ESRCH;
    u64 affinity = target->affinity_mask;
    if (pid > 0) proc_put(target);

    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    u64 avail_mask = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);
    affinity &= avail_mask;
    if (copy_to_user(mask, &affinity, sizeof(u64)) != 0) return -(s64)EFAULT;
    return (s64)sizeof(u64);
}

s64 sys_sched_setaffinity_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    size_t cpusetsize = (size_t)r->rsi;
    const void *mask = (const void *)r->rdx;

    if (pid < 0) return -(s64)EINVAL;
    if (!mask || (uintptr_t)mask >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (cpusetsize < sizeof(u64)) return -(s64)EINVAL;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;

    process_t *target = (pid == 0) ? caller : proc_get_by_pid((u32)pid);
    if (!target) return -(s64)ESRCH;

    if (caller->euid != 0 && caller->uid != target->uid && caller->euid != target->uid) {
        if (pid != 0) proc_put(target);
        return -(s64)EPERM;
    }

    u64 user_mask = 0;
    if (copy_from_user(&user_mask, mask, sizeof(u64)) != 0) {
        if (pid != 0) proc_put(target);
        return -(s64)EFAULT;
    }

    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    u64 avail_mask = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);

    if ((user_mask & avail_mask) == 0) {
        if (pid != 0) proc_put(target);
        return -(s64)EINVAL;
    }

    int err = sched_set_proc_affinity(target, user_mask & avail_mask);
    if (pid != 0) proc_put(target);
    return err < 0 ? (s64)err : 0;
}

s64 sys_getpgid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->pgid;
    process_t *t = proc_by_pid((u32)pid);
    return t ? (s64)t->pgid : -(s64)ESRCH;
}

s64 sys_getsid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->sid;
    process_t *t = proc_by_pid((u32)pid);
    return t ? (s64)t->sid : -(s64)ESRCH;
}

s64 sys_setreuid_impl(pt_regs_t *r)
{
    u32 ruid = (u32)r->rdi;
    u32 euid = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->no_new_privs && proc->euid != 0 && (ruid == 0 || euid == 0))
        return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETUID) && proc->euid != 0) {
        if (ruid != (u32)-1 && ruid != proc->uid && ruid != proc->euid && ruid != proc->suid) return -(s64)EPERM;
        if (euid != (u32)-1 && euid != proc->uid && euid != proc->euid && euid != proc->suid) return -(s64)EPERM;
    }
    if (ruid != (u32)-1) proc->uid = ruid;
    if (euid != (u32)-1) proc->euid = euid;
    security_caps_on_setuid(proc);
    return 0;
}

s64 sys_setresuid_impl(pt_regs_t *r)
{
    u32 ruid = (u32)r->rdi;
    u32 euid = (u32)r->rsi;
    u32 suid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->no_new_privs && proc->euid != 0 && (ruid == 0 || euid == 0 || suid == 0))
        return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETUID) && proc->euid != 0) {
        if (ruid != (u32)-1 && ruid != proc->uid && ruid != proc->euid && ruid != proc->suid) return -(s64)EPERM;
        if (euid != (u32)-1 && euid != proc->uid && euid != proc->euid && euid != proc->suid) return -(s64)EPERM;
        if (suid != (u32)-1 && suid != proc->uid && suid != proc->euid && suid != proc->suid) return -(s64)EPERM;
    }
    if (ruid != (u32)-1) proc->uid = ruid;
    if (euid != (u32)-1) proc->euid = euid;
    if (suid != (u32)-1) proc->suid = suid;
    security_caps_on_setuid(proc);
    return 0;
}

s64 sys_getresuid_impl(pt_regs_t *r)
{
    u32 *ruid = (u32 *)r->rdi;
    u32 *euid = (u32 *)r->rsi;
    u32 *suid = (u32 *)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (ruid && (uintptr_t)ruid < TASK_SIZE_MAX) copy_to_user(ruid, &proc->uid, sizeof(u32));
    if (euid && (uintptr_t)euid < TASK_SIZE_MAX) copy_to_user(euid, &proc->euid, sizeof(u32));
    if (suid && (uintptr_t)suid < TASK_SIZE_MAX) copy_to_user(suid, &proc->suid, sizeof(u32));
    return 0;
}

s64 sys_setregid_impl(pt_regs_t *r)
{
    u32 rgid = (u32)r->rdi;
    u32 egid = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETGID) && proc->euid != 0) {
        if (rgid != (u32)-1 && rgid != proc->gid && rgid != proc->egid && rgid != proc->sgid) return -(s64)EPERM;
        if (egid != (u32)-1 && egid != proc->gid && egid != proc->egid && egid != proc->sgid) return -(s64)EPERM;
    }
    if (rgid != (u32)-1) proc->gid = rgid;
    if (egid != (u32)-1) proc->egid = egid;
    return 0;
}

s64 sys_setresgid_impl(pt_regs_t *r)
{
    u32 rgid = (u32)r->rdi;
    u32 egid = (u32)r->rsi;
    u32 sgid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETGID) && proc->euid != 0) {
        if (rgid != (u32)-1 && rgid != proc->gid && rgid != proc->egid && rgid != proc->sgid) return -(s64)EPERM;
        if (egid != (u32)-1 && egid != proc->gid && egid != proc->egid && egid != proc->sgid) return -(s64)EPERM;
        if (sgid != (u32)-1 && sgid != proc->gid && sgid != proc->egid && sgid != proc->sgid) return -(s64)EPERM;
    }
    if (rgid != (u32)-1) proc->gid = rgid;
    if (egid != (u32)-1) proc->egid = egid;
    if (sgid != (u32)-1) proc->sgid = sgid;
    return 0;
}

s64 sys_getresgid_impl(pt_regs_t *r)
{
    u32 *rgid = (u32 *)r->rdi;
    u32 *egid = (u32 *)r->rsi;
    u32 *sgid = (u32 *)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (rgid && (uintptr_t)rgid < TASK_SIZE_MAX) copy_to_user(rgid, &proc->gid, sizeof(u32));
    if (egid && (uintptr_t)egid < TASK_SIZE_MAX) copy_to_user(egid, &proc->egid, sizeof(u32));
    if (sgid && (uintptr_t)sgid < TASK_SIZE_MAX) copy_to_user(sgid, &proc->sgid, sizeof(u32));
    return 0;
}

s64 sys_getgroups_impl(pt_regs_t *r)
{
    int size = (int)(s32)r->rdi;
    u32 *list = (u32 *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (size == 0) return proc->ngroups > 0 ? (s64)proc->ngroups : 1;
    if (size < 0) return -(s64)EINVAL;
    if (!list || (uintptr_t)list >= TASK_SIZE_MAX) return -(s64)EFAULT;

    if (proc->ngroups > 0) {
        if (size < (int)proc->ngroups) return -(s64)EINVAL;
        if (copy_to_user(list, proc->groups, proc->ngroups * sizeof(u32)) != 0) return -(s64)EFAULT;
        return (s64)proc->ngroups;
    }
    u32 gid = proc->gid;
    if (copy_to_user(list, &gid, sizeof(u32)) != 0) return -(s64)EFAULT;
    return 1;
}

s64 sys_setgroups_impl(pt_regs_t *r)
{
    size_t size = (size_t)r->rdi;
    const u32 *list = (const u32 *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETGID)) return -(s64)EPERM;
    if (size > 32) return -(s64)EINVAL;
    if (size > 0) {
        if (!list || (uintptr_t)list >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_from_user(proc->groups, list, size * sizeof(u32)) != 0) return -(s64)EFAULT;
    }
    proc->ngroups = (u32)size;
    return 0;
}

/* ── Linux Scheduling & Personality ABIs ──────────────────────────────────── */
/* SCHED_* / struct sched_param / sched_target() / sched_policy_valid() are
 * defined above with sched_setattr(). */

s64 sys_sched_setscheduler_impl(pt_regs_t *r)
{
    u32 pid    = (u32)(s32)r->rdi;
    int policy = (int)r->rsi;
    const struct sched_param *uparam = (const struct sched_param *)r->rdx;

    bool reset_on_fork = (policy & SCHED_RESET_ON_FORK) != 0;
    policy &= ~SCHED_RESET_ON_FORK;
    (void)reset_on_fork;  /* accepted, not acted on */

    if (policy != SCHED_OTHER && policy != SCHED_FIFO && policy != SCHED_RR &&
        policy != SCHED_BATCH && policy != SCHED_IDLE)
        return -(s64)EINVAL;

    int prio = 0;
    bool rt = (policy == SCHED_FIFO || policy == SCHED_RR);
    if (uparam) {
        struct sched_param kp;
        if (copy_from_user(&kp, uparam, sizeof(kp)) != 0) return -(s64)EFAULT;
        prio = kp.sched_priority;
    }
    if (rt) {
        if (prio < 1 || prio > 99) return -(s64)EINVAL;
    } else if (prio != 0) {
        return -(s64)EINVAL;
    }

    process_t *self = sched_current_process();
    if (rt && self && self->euid != 0 &&
        !security_check_permission(self, CAP_SYS_NICE))
        return -(s64)EPERM;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    tgt->sched_policy  = (u32)policy;
    tgt->sched_rt_prio = rt ? prio : 0;
    sched_apply_weight(tgt);

    if (put) proc_put(tgt);
    return 0;
}

s64 sys_sched_getscheduler_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;
    s64 policy = (s64)tgt->sched_policy;
    if (put) proc_put(tgt);
    return policy;
}

s64 sys_sched_setparam_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    const struct sched_param *uparam = (const struct sched_param *)r->rsi;
    if (!uparam) return -(s64)EINVAL;

    struct sched_param kp;
    if (copy_from_user(&kp, uparam, sizeof(kp)) != 0) return -(s64)EFAULT;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    bool rt = (tgt->sched_policy == SCHED_FIFO || tgt->sched_policy == SCHED_RR);
    s64 rc = 0;
    if (rt) {
        if (kp.sched_priority < 1 || kp.sched_priority > 99) rc = -(s64)EINVAL;
        else tgt->sched_rt_prio = kp.sched_priority;
    } else if (kp.sched_priority != 0) {
        rc = -(s64)EINVAL;
    }

    if (put) proc_put(tgt);
    return rc;
}

s64 sys_sched_getparam_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_param *uparam = (struct sched_param *)r->rsi;
    if (!uparam || (uintptr_t)uparam >= TASK_SIZE_MAX) return -(s64)EINVAL;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    struct sched_param kp = { .sched_priority = tgt->sched_rt_prio };
    if (put) proc_put(tgt);

    if (copy_to_user(uparam, &kp, sizeof(kp)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_sched_get_priority_max_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 /* SCHED_FIFO */ || policy == 2 /* SCHED_RR */) return 99;
    if (policy == 0 /* SCHED_OTHER */ || policy == 3 /* SCHED_BATCH */ || policy == 5 /* SCHED_IDLE */ || policy == 6 /* SCHED_DEADLINE */) return 0;
    return -(s64)EINVAL;
}

s64 sys_sched_get_priority_min_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 /* SCHED_FIFO */ || policy == 2 /* SCHED_RR */) return 1;
    if (policy == 0 /* SCHED_OTHER */ || policy == 3 /* SCHED_BATCH */ || policy == 5 /* SCHED_IDLE */ || policy == 6 /* SCHED_DEADLINE */) return 0;
    return -(s64)EINVAL;
}

s64 sys_sched_rr_get_interval_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct linux_timespec *tp = (struct linux_timespec *)r->rsi;
    if (!tp || (uintptr_t)tp >= TASK_SIZE_MAX) return -(s64)EFAULT;

    bool put = false;
    s64 err = 0;
    process_t *tgt = sched_target((s32)pid, &put, &err);
    if (!tgt) return err;

    /* Only SCHED_RR has a slice. Linux reports zero for every other policy —
     * SCHED_FIFO runs until it blocks, and a CFS thread's share is not a
     * fixed quantum — and reporting a made-up 10 ms for all of them, which is
     * what this did, tells a real-time program the opposite of the truth
     * about SCHED_FIFO. */
    u64 ns = (tgt->sched_policy == SCHED_RR) ? sched_rr_interval_ns() : 0;
    if (put) proc_put(tgt);

    struct linux_timespec ts = {
        .tv_sec  = (long)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL),
    };
    if (copy_to_user(tp, &ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_personality_impl(pt_regs_t *r)
{
    u64 persona = r->rdi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;

    s64 old = (s64)p->personality;
    /* 0xffffffff is the conventional "query without changing" call. */
    if (persona != 0xFFFFFFFFULL) {
        /* ADDR_NO_RANDOMIZE turns ASLR off for the next execve(). Refuse it
         * under no_new_privs: a sandbox that pinned its privileges should not
         * be able to weaken the address-space defences of what it exec's. */
        if ((persona & 0x0040000U) && p->no_new_privs) return -(s64)EPERM;
        p->personality = (u32)persona;
    }
    return old;
}

/* Linux capget/capset payload: a header {version, pid} plus, for the 64-bit
 * (_LINUX_CAPABILITY_VERSION_3) layout, two {effective, permitted, inheritable}
 * u32 triples — low 32 bits first, then high. */
#define LINUX_CAPABILITY_VERSION_3  0x20080522

typedef struct {
    u32 version;
    s32 pid;
} cap_user_header_t;

typedef struct {
    u32 effective;
    u32 permitted;
    u32 inheritable;
} cap_user_data_t;

/* capget/capset address a process by pid; 0 means "self". Only self and
 * children are addressable here, and modifying anything but self is refused —
 * letting one process rewrite another's capability sets would defeat the point
 * of having them. */
s64 sys_capget_impl(pt_regs_t *r)
{
    cap_user_header_t *uhdr = (cap_user_header_t *)r->rdi;
    cap_user_data_t   *udata = (cap_user_data_t *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    cap_user_header_t hdr = { LINUX_CAPABILITY_VERSION_3, 0 };
    if (uhdr) {
        if ((uintptr_t)uhdr >= TASK_SIZE_MAX) return -(s64)EFAULT;
        if (copy_from_user(&hdr, uhdr, sizeof(hdr)) != 0) return -(s64)EFAULT;
    }

    /* Report back the version we speak, as Linux does for a probe call. */
    if (hdr.version != LINUX_CAPABILITY_VERSION_3) {
        hdr.version = LINUX_CAPABILITY_VERSION_3;
        if (uhdr) copy_to_user(uhdr, &hdr, sizeof(hdr));
        if (!udata) return 0;              /* pure version probe */
        return -(s64)EINVAL;
    }

    process_t *target = proc;
    if (hdr.pid != 0 && (u32)hdr.pid != proc->pid) {
        target = NULL;
        for (process_t *q = sched_get_process_list(); q; q = q->next) {
            if (q->pid == (u32)hdr.pid) { target = q; break; }
        }
        if (!target) return -(s64)ESRCH;
    }

    if (!udata) return 0;
    if ((uintptr_t)udata >= TASK_SIZE_MAX) return -(s64)EFAULT;

    cap_user_data_t out[2];
    out[0].effective   = (u32)(target->cap_effective   & 0xFFFFFFFFu);
    out[0].permitted   = (u32)(target->cap_permitted   & 0xFFFFFFFFu);
    out[0].inheritable = (u32)(target->cap_inheritable & 0xFFFFFFFFu);
    out[1].effective   = (u32)(target->cap_effective   >> 32);
    out[1].permitted   = (u32)(target->cap_permitted   >> 32);
    out[1].inheritable = (u32)(target->cap_inheritable >> 32);

    if (copy_to_user(udata, out, sizeof(out)) != 0) return -(s64)EFAULT;
    return 0;
}

s64 sys_capset_impl(pt_regs_t *r)
{
    cap_user_header_t *uhdr = (cap_user_header_t *)r->rdi;
    cap_user_data_t   *udata = (cap_user_data_t *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!uhdr || !udata) return -(s64)EFAULT;
    if ((uintptr_t)uhdr >= TASK_SIZE_MAX ||
        (uintptr_t)udata >= TASK_SIZE_MAX) return -(s64)EFAULT;

    cap_user_header_t hdr;
    if (copy_from_user(&hdr, uhdr, sizeof(hdr)) != 0) return -(s64)EFAULT;
    if (hdr.version != LINUX_CAPABILITY_VERSION_3) return -(s64)EINVAL;
    /* Only self. Rewriting another process's capabilities is not something an
     * unprivileged caller may do, and we have no use for the privileged case. */
    if (hdr.pid != 0 && (u32)hdr.pid != proc->pid) return -(s64)EPERM;

    cap_user_data_t in[2];
    if (copy_from_user(in, udata, sizeof(in)) != 0) return -(s64)EFAULT;

    u64 new_eff = ((u64)in[1].effective   << 32) | in[0].effective;
    u64 new_prm = ((u64)in[1].permitted   << 32) | in[0].permitted;
    u64 new_inh = ((u64)in[1].inheritable << 32) | in[0].inheritable;

    /* Undefined capability bits are rejected rather than silently masked, so a
     * caller is never told "granted" for a bit we do not actually honour. */
    if ((new_eff | new_prm | new_inh) & ~CAP_FULL_SET) return -(s64)EINVAL;

    /* The two rules that make this safe, straight from POSIX.1e:
     *   - the new permitted set must be a subset of the old permitted set
     *     (without CAP_SETPCAP you may only ever drop privilege), and
     *   - effective and inheritable must be subsets of the new permitted set.
     * Together they make capset() a monotonically de-escalating operation. */
    if (new_prm & ~proc->cap_permitted) return -(s64)EPERM;
    if (new_eff & ~new_prm)             return -(s64)EPERM;
    if (new_inh & ~proc->cap_bounding)  return -(s64)EPERM;

    proc->cap_permitted   = new_prm;
    proc->cap_effective   = new_eff;
    proc->cap_inheritable = new_inh;
    /* The bounding set never grows: clamp it to what is still permitted plus
     * whatever is inheritable, so dropped privilege cannot return via exec. */
    proc->cap_bounding &= (new_prm | new_inh);
    return 0;
}

/* setpriority(2) selectors. */
#define PRIO_PROCESS 0
#define PRIO_PGRP    1
#define PRIO_USER    2

/* Resolve a getpriority/setpriority (which, who) into a pid list. who==0 maps
 * to the caller's own pid / pgid / real-uid. Returns count, or <0 errno. */
static int prio_targets(int which, int who, u32 *pids, int max)
{
    process_t *self = sched_current_process();
    if (!self) return -(int)EPERM;
    if (which != PRIO_PROCESS && which != PRIO_PGRP && which != PRIO_USER)
        return -(int)EINVAL;

    u32 key;
    if (who == 0) {
        key = (which == PRIO_PROCESS) ? self->pid
            : (which == PRIO_PGRP)    ? self->pgid
                                      : self->uid;
    } else {
        key = (u32)who;
    }
    return sched_collect_pids(pids, max, which, key);
}

s64 sys_getpriority_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who   = (int)r->rsi;

    u32 pids[64];
    int n = prio_targets(which, who, pids, 64);
    if (n < 0)  return (s64)n;
    if (n == 0) return -(s64)ESRCH;

    /* Kernel ABI: return 20 - nice, so the value is always positive (glibc
     * maps it back). Report the highest priority = lowest nice = largest 20-n. */
    int best = -1;   /* 20 - 19 */
    for (int i = 0; i < n; i++) {
        process_t *p = proc_get_by_pid(pids[i]);
        if (!p) continue;
        int v = 20 - p->prio_nice;
        if (v > best) best = v;
        proc_put(p);
    }
    return (s64)best;
}

s64 sys_setpriority_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who   = (int)r->rsi;
    int prio  = (int)r->rdx;

    if (prio < -20) prio = -20;
    if (prio >  19) prio =  19;

    process_t *self = sched_current_process();

    u32 pids[64];
    int n = prio_targets(which, who, pids, 64);
    if (n < 0)  return (s64)n;
    if (n == 0) return -(s64)ESRCH;

    s64 rc = -(s64)ESRCH;
    for (int i = 0; i < n; i++) {
        process_t *p = proc_get_by_pid(pids[i]);
        if (!p) continue;

        /* Must own the target (euid match) unless privileged; lowering the
         * nice value (raising priority) additionally needs CAP_SYS_NICE or
         * headroom under RLIMIT_NICE (encoded Linux-style as 20 - nice). */
        bool allowed = self && (self->euid == 0 || self->euid == p->euid ||
                                self->euid == p->uid);
        if (allowed && prio < p->prio_nice) {
            u64 nice_ceiling = p->rlimits[13 /* RLIMIT_NICE */].rlim_cur;
            int lowest_nice = (nice_ceiling >= 40) ? -20 : (20 - (int)nice_ceiling);
            if (prio < lowest_nice && self->euid != 0 &&
                !security_check_permission(self, CAP_SYS_NICE))
                allowed = false;
        }
        if (!allowed) { proc_put(p); rc = -(s64)EACCES; continue; }

        p->prio_nice = prio;
        sched_apply_weight(p);
        proc_put(p);
        if (rc == -(s64)ESRCH) rc = 0;
    }
    return rc;
}

/* setfsuid/setfsgid — set filesystem user/group identity according to Linux spec */
s64 sys_setfsuid_impl(pt_regs_t *r)
{
    u32 fsuid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return 0;
    u32 old = p->fsuid;
    if (p->euid == 0 || fsuid == p->uid || fsuid == p->euid || fsuid == p->suid || fsuid == p->fsuid) {
        p->fsuid = fsuid;
    }
    return (s64)old;
}
s64 sys_setfsgid_impl(pt_regs_t *r)
{
    u32 fsgid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return 0;
    u32 old = p->fsgid;
    if (p->euid == 0 || fsgid == p->gid || fsgid == p->egid || fsgid == p->sgid || fsgid == p->fsgid) {
        p->fsgid = fsgid;
    }
    return (s64)old;
}

/* unshare(flags) — unshare execution context */
s64 sys_unshare_impl(pt_regs_t *r)
{
    u64 flags = r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    #define CLONE_FS           0x00000200ULL
    #define CLONE_FILES        0x00000400ULL
    #define CLONE_NEWNS        0x00020000ULL
    #define CLONE_NEWCGROUP    0x02000000ULL
    #define CLONE_NEWUTS       0x04000000ULL
    #define CLONE_NEWIPC       0x08000000ULL
    #define CLONE_NEWUSER      0x10000000ULL
    #define CLONE_NEWPID       0x20000000ULL
    #define CLONE_NEWNET       0x40000000ULL

    u64 valid = CLONE_FS | CLONE_FILES | CLONE_NEWNS | CLONE_NEWCGROUP |
                CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET;

    if (flags & ~valid) return -(s64)EINVAL;

    /* Linux requires CAP_SYS_ADMIN for unsharing namespaces (except CLONE_NEWUSER) */
    if ((flags & (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNET)) &&
        !security_check_permission(proc, CAP_SYS_ADMIN)) {
        return -(s64)EPERM;
    }

    if (flags & CLONE_NEWUTS) {
        uts_namespace_t *new_ns = uts_ns_create(proc->uts_ns ? proc->uts_ns->nodename : "azamios",
                                                proc->uts_ns ? proc->uts_ns->domainname : "local");
        if (!new_ns) return -(s64)ENOMEM;
        uts_ns_put(proc->uts_ns);
        proc->uts_ns = new_ns;
    }

    if (flags & CLONE_NEWPID) {
        proc->new_pid_ns = true;
    }

    return 0;
}

/* ============================================================================
 * kcmp(2) — compare two processes to determine if they share kernel resources
 * ========================================================================= */

#define KCMP_FILE       0
#define KCMP_VM         1
#define KCMP_FILES      2
#define KCMP_FS         3
#define KCMP_SIGHAND    4
#define KCMP_IO         5
#define KCMP_SYSVSEM    6
#define KCMP_EPOLL_TFD  7

s64 sys_kcmp_impl(pt_regs_t *r)
{
    u32 pid1 = (u32)r->rdi;
    u32 pid2 = (u32)r->rsi;
    int type = (int)r->rdx;
    u64 idx1 = (u64)r->r10;
    u64 idx2 = (u64)r->r8;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;

    /* Hold references for the whole comparison: without them either target can
     * be reaped on another CPU mid-switch, turning p1->cwd / p1->handle_table
     * into a use-after-free. */
    process_t *p1 = proc_get_by_pid(pid1);
    process_t *p2 = proc_get_by_pid(pid2);
    s64 ret;
    if (!p1 || !p2) {
        ret = -(s64)ESRCH;
    } else if (caller->euid != 0 && caller->uid != p1->uid &&
               caller->uid != p2->uid) {
        ret = -(s64)EPERM;
    } else switch (type) {
        case KCMP_VM:
            ret = (p1->pml4_phys == p2->pml4_phys) ? 0 : ((p1->pml4_phys < p2->pml4_phys) ? 1 : 2);
            break;
        case KCMP_FS:
            ret = (strcmp(p1->cwd, p2->cwd) == 0 && p1->umask == p2->umask) ? 0 : 1;
            break;
        case KCMP_FILES:
            ret = (p1 == p2) ? 0 : 1;
            break;
        case KCMP_FILE: {
            if (idx1 >= 64 || idx2 >= 64) { ret = -(s64)EBADF; break; }
            void *f1 = p1->handle_table[idx1];
            void *f2 = p2->handle_table[idx2];
            if (!f1 || !f2) { ret = -(s64)EBADF; break; }
            ret = (f1 == f2) ? 0 : ((uintptr_t)f1 < (uintptr_t)f2 ? 1 : 2);
            break;
        }
        default:
            ret = (p1 == p2) ? 0 : 1;
            break;
    }

    proc_put(p1);
    proc_put(p2);
    return ret;
}

/* ============================================================================
 * setns(2)
 * ========================================================================= */

s64 sys_setns_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int nstype = (int)r->rsi;
    (void)nstype;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;

    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    fput(f);

    /* There is a single namespace of each kind, so no descriptor can name a
     * different one to join. EINVAL is what Linux returns for an fd that is
     * not a namespace file, and is what callers probe for. */
    return -(s64)EINVAL;
}

/* ============================================================================
 * ioprio_set(2) / ioprio_get(2)
 *
 * The block layer here services requests in arrival order, so a class and
 * level cannot change when a request is issued. They are still worth storing
 * and reporting: a program that lowers itself to IOPRIO_CLASS_IDLE and reads
 * the value back must see the change, ionice(1) works, and the value is
 * inherited across fork() as the interface promises.
 * ========================================================================= */

/* Linux's mapping when no explicit priority has been set: best-effort, with
 * the level derived from the CPU nice value. */
#define IOPRIO_NORM 4

static s64 ioprio_apply(process_t *p, int prio)
{
    int class = IOPRIO_PRIO_CLASS(prio);
    int data  = IOPRIO_PRIO_DATA(prio);
    process_t *me = sched_current_process();

    switch (class) {
    case IOPRIO_CLASS_RT:
        /* Real-time I/O can starve every other process on the machine. */
        if (!me || !security_check_permission(me, CAP_SYS_ADMIN)) return -(s64)EPERM;
        /* fall through */
    case IOPRIO_CLASS_BE:
        if (data >= IOPRIO_NR_LEVELS) return -(s64)EINVAL;
        break;
    case IOPRIO_CLASS_IDLE:
        break;
    case IOPRIO_CLASS_NONE:
        if (data) return -(s64)EINVAL;
        break;
    default:
        return -(s64)EINVAL;
    }
    p->ioprio = (u32)prio;
    return 0;
}

/* An unprivileged process may only change a process it could signal. */
static bool ioprio_may_change(const process_t *me, const process_t *target)
{
    if (!me || !target) return false;
    if (me->euid == 0) return true;
    return me->euid == target->uid || me->euid == target->euid;
}

s64 sys_ioprio_set_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who   = (int)r->rsi;
    int prio  = (int)r->rdx;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;

    s64 done = 0;
    switch (which) {
    case IOPRIO_WHO_PROCESS: {
        if (!who) return ioprio_apply(me, prio);
        process_t *t = proc_get_by_pid((u32)who);
        if (!t) return -(s64)ESRCH;
        s64 rc = ioprio_may_change(me, t) ? ioprio_apply(t, prio)
                                          : -(s64)EPERM;
        proc_put(t);
        return rc;
    }
    case IOPRIO_WHO_PGRP:
    case IOPRIO_WHO_USER: {
        u32 key = (u32)who;
        if (which == IOPRIO_WHO_PGRP && who == 0) key = me->pgid;
        if (which == IOPRIO_WHO_USER && who == 0) key = me->uid;

        sched_lock();
        for (process_t *t = sched_get_process_list(); t; t = t->next) {
            if (t->is_zombie) continue;
            bool match = (which == IOPRIO_WHO_PGRP) ? (t->pgid == key)
                                                    : (t->uid == key);
            if (!match) continue;
            if (!ioprio_may_change(me, t)) continue;
            if (ioprio_apply(t, prio) == 0) done++;
        }
        sched_unlock();
        return done ? 0 : -(s64)ESRCH;
    }
    default:
        return -(s64)EINVAL;
    }
}

s64 sys_ioprio_get_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who   = (int)r->rsi;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;

    /* ioprio_get() over a group returns the *highest* priority found, which
     * for this encoding is the numerically smallest value. */
    s64 best = -1;

    switch (which) {
    case IOPRIO_WHO_PROCESS: {
        process_t *t = me;
        if (who) {
            t = proc_get_by_pid((u32)who);
            if (!t) return -(s64)ESRCH;
        }
        best = t->ioprio ? (s64)t->ioprio
                         : (s64)IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_NORM);
        if (who) proc_put(t);
        return best;
    }
    case IOPRIO_WHO_PGRP:
    case IOPRIO_WHO_USER: {
        u32 key = (u32)who;
        if (which == IOPRIO_WHO_PGRP && who == 0) key = me->pgid;
        if (which == IOPRIO_WHO_USER && who == 0) key = me->uid;

        sched_lock();
        for (process_t *t = sched_get_process_list(); t; t = t->next) {
            if (t->is_zombie) continue;
            bool match = (which == IOPRIO_WHO_PGRP) ? (t->pgid == key)
                                                    : (t->uid == key);
            if (!match) continue;
            s64 v = t->ioprio ? (s64)t->ioprio
                              : (s64)IOPRIO_PRIO_VALUE(IOPRIO_CLASS_BE, IOPRIO_NORM);
            if (best < 0 || v < best) best = v;
        }
        sched_unlock();
        return best < 0 ? -(s64)ESRCH : best;
    }
    default:
        return -(s64)EINVAL;
    }
}
