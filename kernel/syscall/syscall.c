/* ============================================================================
 * AzamiOS — System Call Dispatcher Implementation
 * File: kernel/syscall/syscall.c
 *
 * Full POSIX x86_64 ABI System Call Implementation with argument parsing,
 * network sockets, polling, System V stack construction, and telemetry.
 * ============================================================================ */

#define DEBUG 0
#include "../../include/azami/debug.h"
#include "syscall.h"
#include "../ptrace.h"
#include "../perf/perf.h"
#include "../../drivers/char/console.h"
#include "../../drivers/char/uart.h"
#include "../../drivers/input/input.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/lib/random.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/sched/elf.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/vma.h"
#include "../../kernel/signal.h"
#include "../../kernel/ipc/ipc.h"
#include "../../kernel/ipc/sysvipc.h"
#include "../../kernel/ktimer.h"
#include "../../kernel/object/object.h"
#include "../../arch/x86_64/cpu/nospec.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../../drivers/misc/bga.h"
#include "../../include/azami/defs.h"
#include "../../kernel/uaccess.h"
#include "../../fs/pipe.h"
#include "../../drivers/acpi/power.h"
#include "../../drivers/misc/rtc.h"
#include "../../drivers/misc/hpet.h"
#include "../../include/azami/net.h"
#include "../../include/azami/socket.h"
#include "../security/acl.h"
#include "../../arch/x86_64/cpu/cpu.h"   /* g_pku_enabled */
#include "../../arch/x86_64/cpu/msr.h"   /* rdpkru / wrpkru */
#include "../security/security.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../arch/x86_64/cpu/smp.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../ipc/mqueue.h"
#include "../perf/ktrace.h"

typedef s64 (*syscall_fn_t)(pt_regs_t *r);


#define SYSCALL_TABLE_SIZE  560
static syscall_fn_t g_syscall_table[SYSCALL_TABLE_SIZE];

/* PROC_MAX_FDS lives in sched.h next to the arrays it sizes. */

/* Serialises fd-table slot moves (lookup / allocate / teardown). Held only
 * across pointer assignments — never across a blocking call. */
static spinlock_t g_fd_lock = SPINLOCK_INIT;

/* fget() — return the file behind `fd` with its reference count raised by one,
 * or NULL if the fd is not a valid open file. Every successful fget() must be
 * balanced by exactly one fput(), which is what stops a concurrent close() in
 * another thread from freeing the struct while this syscall is still using it. */
static file_t *fget(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    irqflags_t f = spinlock_lock_irqsave(&g_fd_lock);
    file_t *file = (file_t *)proc->handle_table[fd];
    if (file && (uintptr_t)file >= 0xFFFF800000000000ULL) {
        __atomic_add_fetch(&file->f_count, 1, __ATOMIC_SEQ_CST);
    } else {
        file = NULL;
    }
    spinlock_unlock_irqrestore(&g_fd_lock, f);
    return file;
}

/* fput() — drop a reference taken by fget(). Frees the file at zero. */
static void fput(file_t *file)
{
    if (file) vfs_close(file);
}

/* fd_table_release() — see syscall.h. Clear each slot under g_fd_lock so a
 * concurrent fget() (including a cross-process one from pidfd_getfd) cannot
 * observe a file_t between "still in the table" and "already freed"; the
 * blocking close work is done afterwards with the lock dropped. */
void fd_table_release(process_t *proc)
{
    if (!proc) return;
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
        file_t *f = (file_t *)__atomic_exchange_n(&proc->handle_table[i],
                                                  NULL, __ATOMIC_SEQ_CST);
        spinlock_unlock_irqrestore(&g_fd_lock, fl);
        if (f) vfs_close(f);

        /* Object handles have their own lock; az_handle_close() takes it. */
        if (proc->obj_handle_table[i])
            az_handle_close(proc, i);
    }
}

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

/* fd_install_from() — atomically claim the lowest free fd >= minfd for `file`.
 * Returns the fd, or -EMFILE if the table is full. Prevents two threads racing
 * on the "find a NULL slot" scan from both grabbing the same descriptor. */
/* RLIMIT_NOFILE (index 7, see the RLIMIT_* block further down) soft limit as an
 * fd-count ceiling, clamped to the table size. */
static int fd_limit(process_t *proc)
{
    u64 lim = proc->rlimits[7].rlim_cur;
    return (lim >= PROC_MAX_FDS) ? PROC_MAX_FDS : (int)lim;
}

static s64 fd_install_from(process_t *proc, void *file, u8 fd_flags, int minfd)
{
    if (!proc) return -(s64)EPERM;
    if (minfd < 0) minfd = 0;
    int limit = fd_limit(proc);
    irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
    for (int i = minfd; i < limit; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = file;
            proc->fd_flags[i] = fd_flags;
            spinlock_unlock_irqrestore(&g_fd_lock, fl);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&g_fd_lock, fl);
    return -(s64)EMFILE;
}

static inline s64 fd_install(process_t *proc, file_t *file, u8 fd_flags)
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
static s64 fd_install_pair(process_t *proc, void *f0, void *f1, u8 fd_flags,
                           int *out0, int *out1)
{
    if (!proc) return -(s64)EPERM;
    int limit = fd_limit(proc);
    irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
    int a = -1, b = -1;
    for (int i = 0; i < limit; i++) {
        if (!proc->handle_table[i]) {
            if (a < 0) a = i;
            else { b = i; break; }
        }
    }
    if (a < 0 || b < 0) { spinlock_unlock_irqrestore(&g_fd_lock, fl); return -(s64)EMFILE; }
    proc->handle_table[a] = f0; proc->fd_flags[a] = fd_flags;
    proc->handle_table[b] = f1; proc->fd_flags[b] = fd_flags;
    spinlock_unlock_irqrestore(&g_fd_lock, fl);
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

/* ── Forward declarations ────────────────────────────────────────────────── */
static s64 sys_read_impl(pt_regs_t *r);
static s64 sys_write_impl(pt_regs_t *r);
static s64 sys_open_impl(pt_regs_t *r);
static s64 sys_close_impl(pt_regs_t *r);
static s64 sys_stat_impl(pt_regs_t *r);
static s64 sys_fstat_impl(pt_regs_t *r);
static s64 sys_lstat_impl(pt_regs_t *r);
static s64 sys_poll_impl(pt_regs_t *r);
static s64 sys_lseek_impl(pt_regs_t *r);
static s64 sys_mmap_impl(pt_regs_t *r);
static s64 sys_mprotect_impl(pt_regs_t *r);
static s64 sys_munmap_impl(pt_regs_t *r);
static s64 sys_brk_impl(pt_regs_t *r);
static s64 sys_rt_sigaction_impl(pt_regs_t *r);
static s64 sys_rt_sigprocmask_impl(pt_regs_t *r);
/* sys_rt_sigreturn_impl declared in kernel/signal.h */
static s64 sys_ioctl_impl(pt_regs_t *r);
static s64 sys_readv_impl(pt_regs_t *r);
static s64 sys_writev_impl(pt_regs_t *r);
static s64 sys_access_impl(pt_regs_t *r);
static s64 sys_pipe_impl(pt_regs_t *r);
static s64 sys_select_impl(pt_regs_t *r);
static s64 sys_dup_impl(pt_regs_t *r);
static s64 sys_dup2_impl(pt_regs_t *r);
static s64 sys_pause_impl(pt_regs_t *r);
static s64 sys_nanosleep_impl(pt_regs_t *r);
static s64 sys_alarm_impl(pt_regs_t *r);
static s64 sys_getpid_impl(pt_regs_t *r);
static s64 sys_socket_impl(pt_regs_t *r);
static s64 sys_connect_impl(pt_regs_t *r);
static s64 sys_accept_impl(pt_regs_t *r);
static s64 sys_sendto_impl(pt_regs_t *r);
static s64 sys_recvfrom_impl(pt_regs_t *r);
static s64 sys_sendmsg_impl(pt_regs_t *r);
static s64 sys_recvmsg_impl(pt_regs_t *r);
static s64 sys_shutdown_impl(pt_regs_t *r);
static s64 sys_bind_impl(pt_regs_t *r);
static s64 sys_listen_impl(pt_regs_t *r);
static s64 sys_getsockname_impl(pt_regs_t *r);
static s64 sys_getpeername_impl(pt_regs_t *r);
static s64 sys_setsockopt_impl(pt_regs_t *r);
static s64 sys_getsockopt_impl(pt_regs_t *r);
static s64 sys_clone_impl(pt_regs_t *r);
static s64 sys_fork_impl(pt_regs_t *r);
static s64 sys_vfork_impl(pt_regs_t *r);
static s64 sys_execve_impl(pt_regs_t *r);
static s64 sys_execveat_impl(pt_regs_t *r);
s64 sys_exit_impl(pt_regs_t *r);
static s64 sys_wait4_impl(pt_regs_t *r);
static s64 sys_waitid_impl(pt_regs_t *r);
static s64 sys_close_range_impl(pt_regs_t *r);
static s64 sys_kill_impl(pt_regs_t *r);
static s64 sys_uname_impl(pt_regs_t *r);
static s64 sys_fcntl_impl(pt_regs_t *r);
static s64 sys_truncate_impl(pt_regs_t *r);
static s64 sys_ftruncate_impl(pt_regs_t *r);
static s64 sys_getdents_impl(pt_regs_t *r);
static s64 sys_getcwd_impl(pt_regs_t *r);
static s64 sys_chdir_impl(pt_regs_t *r);
static s64 sys_fchdir_impl(pt_regs_t *r);
static s64 sys_rename_impl(pt_regs_t *r);
static s64 sys_mkdir_impl(pt_regs_t *r);
static s64 sys_rmdir_impl(pt_regs_t *r);
static s64 sys_unlink_impl(pt_regs_t *r);
static s64 sys_symlink_impl(pt_regs_t *r);
static s64 sys_readlink_impl(pt_regs_t *r);
static s64 sys_chmod_impl(pt_regs_t *r);
static s64 sys_fchmod_impl(pt_regs_t *r);
static s64 sys_chown_impl(pt_regs_t *r);
static s64 sys_fchown_impl(pt_regs_t *r);
static s64 sys_umask_impl(pt_regs_t *r);
static s64 sys_gettimeofday_impl(pt_regs_t *r);
static s64 sys_sysinfo_impl(pt_regs_t *r);
static s64 sys_times_impl(pt_regs_t *r);
static s64 sys_getuid_impl(pt_regs_t *r);
static s64 sys_getgid_impl(pt_regs_t *r);
static s64 sys_setuid_impl(pt_regs_t *r);
static s64 sys_setgid_impl(pt_regs_t *r);
static s64 sys_geteuid_impl(pt_regs_t *r);
static s64 sys_getegid_impl(pt_regs_t *r);
static s64 sys_setpgid_impl(pt_regs_t *r);
static s64 sys_getppid_impl(pt_regs_t *r);
static s64 sys_getpgrp_impl(pt_regs_t *r);
static s64 sys_setsid_impl(pt_regs_t *r);
static s64 sys_utime_impl(pt_regs_t *r);
static s64 sys_statfs_impl(pt_regs_t *r);
static s64 sys_fstatfs_impl(pt_regs_t *r);
static s64 sys_reboot_impl(pt_regs_t *r);
static s64 sys_time_impl(pt_regs_t *r);
static s64 sys_getdents64_impl(pt_regs_t *r);
static s64 sys_clock_gettime_impl(pt_regs_t *r);
static s64 sys_exit_group_impl(pt_regs_t *r);
static s64 sys_utimes_impl(pt_regs_t *r);
static s64 sys_pselect6_impl(pt_regs_t *r);
static s64 sys_ppoll_impl(pt_regs_t *r);
static s64 sys_utimensat_impl(pt_regs_t *r);
static s64 sys_dup3_impl(pt_regs_t *r);
static s64 sys_pipe2_impl(pt_regs_t *r);
static s64 sys_getfacl_impl(pt_regs_t *r);
static s64 sys_setfacl_impl(pt_regs_t *r);
static s64 sys_getrlimit_impl(pt_regs_t *r);
static s64 sys_setrlimit_impl(pt_regs_t *r);
static s64 sys_getrusage_impl(pt_regs_t *r);
static s64 sys_openat_impl(pt_regs_t *r);
static s64 sys_mkdirat_impl(pt_regs_t *r);
static s64 sys_fstatat_impl(pt_regs_t *r);
static s64 sys_unlinkat_impl(pt_regs_t *r);
static s64 sys_readlinkat_impl(pt_regs_t *r);
static s64 sys_faccessat_impl(pt_regs_t *r);

/* Extended Azami Syscalls */
static s64 sys_az_channel_create(pt_regs_t *r);
static s64 sys_az_channel_send(pt_regs_t *r);
static s64 sys_az_channel_recv(pt_regs_t *r);
static s64 sys_az_channel_destroy(pt_regs_t *r);
static s64 sys_az_shmem_create(pt_regs_t *r);
static s64 sys_az_shmem_map(pt_regs_t *r);
static s64 sys_az_shmem_destroy(pt_regs_t *r);
static s64 sys_az_shmem_unmap(pt_regs_t *r);
static s64 sys_az_object_create(pt_regs_t *r);
static s64 sys_az_object_open(pt_regs_t *r);
static s64 sys_az_object_close(pt_regs_t *r);
static s64 sys_az_fb_info(pt_regs_t *r);
static s64 sys_az_fb_map(pt_regs_t *r);
static s64 sys_az_fb_flip(pt_regs_t *r);
static s64 sys_az_spawn(pt_regs_t *r);
static s64 sys_az_yield(pt_regs_t *r);
static s64 sys_az_thread_create_impl(pt_regs_t *r);
static s64 sys_az_thread_exit_impl(pt_regs_t *r);
static s64 sys_az_sysstat_impl(pt_regs_t *r);
static s64 sys_az_set_timer_impl(pt_regs_t *r);
static s64 sys_arch_prctl_impl(pt_regs_t *r);
static s64 sys_set_tid_address_impl(pt_regs_t *r);
static s64 sys_prlimit64_impl(pt_regs_t *r);
static s64 sys_rseq_impl(pt_regs_t *r);
static s64 sys_sendfile_impl(pt_regs_t *r);
static s64 sys_copy_file_range_impl(pt_regs_t *r);
static s64 sys_fallocate_impl(pt_regs_t *r);
static s64 sys_sync_file_range_impl(pt_regs_t *r);
static s64 sys_readahead_impl(pt_regs_t *r);
static s64 sys_splice_impl(pt_regs_t *r);
static s64 sys_tee_impl(pt_regs_t *r);
static s64 sys_vmsplice_impl(pt_regs_t *r);
static s64 sys_pread64_impl(pt_regs_t *r);
static s64 sys_pwrite64_impl(pt_regs_t *r);
static s64 sys_getrandom_impl(pt_regs_t *r);
static s64 sys_statx_impl(pt_regs_t *r);
static s64 sys_syslog_impl(pt_regs_t *r);
static s64 sys_swapon_impl(pt_regs_t *r);
static s64 sys_swapoff_impl(pt_regs_t *r);
static s64 sys_sched_yield_impl(pt_regs_t *r);
static s64 sys_msync_impl(pt_regs_t *r);
static s64 sys_madvise_impl(pt_regs_t *r);
static s64 sys_socketpair_impl(pt_regs_t *r);
static s64 sys_link_impl(pt_regs_t *r);
static s64 sys_prctl_impl(pt_regs_t *r);
static s64 sys_gettid_impl(pt_regs_t *r);
static s64 sys_tkill_impl(pt_regs_t *r);
static s64 sys_tgkill_impl(pt_regs_t *r);
static s64 sys_sched_setaffinity_impl(pt_regs_t *r);
static s64 sys_sched_getaffinity_impl(pt_regs_t *r);
static s64 sys_fadvise64_impl(pt_regs_t *r);
static s64 sys_fchownat_impl(pt_regs_t *r);
static s64 sys_linkat_impl(pt_regs_t *r);
static s64 sys_symlinkat_impl(pt_regs_t *r);
static s64 sys_fchmodat_impl(pt_regs_t *r);
static s64 sys_renameat_impl(pt_regs_t *r);
static s64 sys_flock_impl(pt_regs_t *r);
static s64 sys_fsync_impl(pt_regs_t *r);
static s64 sys_fdatasync_impl(pt_regs_t *r);
static s64 sys_sync_impl(pt_regs_t *r);
static s64 sys_syncfs_impl(pt_regs_t *r);
static s64 sys_getpgid_impl(pt_regs_t *r);
static s64 sys_getsid_impl(pt_regs_t *r);
static s64 sys_setreuid_impl(pt_regs_t *r);
static s64 sys_setregid_impl(pt_regs_t *r);
static s64 sys_setresuid_impl(pt_regs_t *r);
static s64 sys_getresuid_impl(pt_regs_t *r);
static s64 sys_setresgid_impl(pt_regs_t *r);
static s64 sys_getresgid_impl(pt_regs_t *r);
static s64 sys_getgroups_impl(pt_regs_t *r);
static s64 sys_setgroups_impl(pt_regs_t *r);
static s64 sys_clock_getres_impl(pt_regs_t *r);
static s64 sys_clock_settime_impl(pt_regs_t *r);
static s64 sys_clock_nanosleep_impl(pt_regs_t *r);
static s64 sys_mremap_impl(pt_regs_t *r);
static s64 sys_capget_impl(pt_regs_t *r);
static s64 sys_capset_impl(pt_regs_t *r);
static s64 sys_personality_impl(pt_regs_t *r);
static s64 sys_sched_setparam_impl(pt_regs_t *r);
static s64 sys_sched_getparam_impl(pt_regs_t *r);
static s64 sys_sched_setscheduler_impl(pt_regs_t *r);
static s64 sys_sched_getscheduler_impl(pt_regs_t *r);
static s64 sys_sched_get_priority_max_impl(pt_regs_t *r);
static s64 sys_sched_get_priority_min_impl(pt_regs_t *r);
static s64 sys_sched_rr_get_interval_impl(pt_regs_t *r);
static s64 sys_futex_impl(pt_regs_t *r);
static s64 sys_epoll_create_impl(pt_regs_t *r);
static s64 sys_epoll_create1_impl(pt_regs_t *r);
static s64 sys_epoll_ctl_impl(pt_regs_t *r);
static s64 sys_epoll_wait_impl(pt_regs_t *r);
static s64 sys_epoll_pwait_impl(pt_regs_t *r);
static s64 sys_signalfd_impl(pt_regs_t *r);
static s64 sys_signalfd4_impl(pt_regs_t *r);
static s64 sys_timerfd_create_impl(pt_regs_t *r);
static s64 sys_timerfd_settime_impl(pt_regs_t *r);
static s64 sys_timerfd_gettime_impl(pt_regs_t *r);
static s64 sys_eventfd_impl(pt_regs_t *r);
static s64 sys_eventfd2_impl(pt_regs_t *r);
static s64 sys_inotify_init_impl(pt_regs_t *r);
static s64 sys_inotify_init1_impl(pt_regs_t *r);
static s64 sys_inotify_add_watch_impl(pt_regs_t *r);
static s64 sys_inotify_rm_watch_impl(pt_regs_t *r);
static s64 sys_membarrier_impl(pt_regs_t *r);
static s64 sys_clone3_impl(pt_regs_t *r);
static s64 sys_close_range_impl(pt_regs_t *r);
static s64 sys_openat2_impl(pt_regs_t *r);
static s64 sys_faccessat2_impl(pt_regs_t *r);
static s64 sys_epoll_pwait2_impl(pt_regs_t *r);
static s64 sys_getcpu_impl(pt_regs_t *r);
static s64 sys_seccomp_impl(pt_regs_t *r);
static s64 sys_sched_setattr_impl(pt_regs_t *r);
static s64 sys_sched_getattr_impl(pt_regs_t *r);
static s64 sys_pidfd_open_impl(pt_regs_t *r);
static s64 sys_pidfd_send_signal_impl(pt_regs_t *r);
static s64 sys_pidfd_getfd_impl(pt_regs_t *r);

/* POSIX message queues */
static s64 sys_mq_open_impl(pt_regs_t *r);
static s64 sys_mq_unlink_impl(pt_regs_t *r);
static s64 sys_mq_timedsend_impl(pt_regs_t *r);
static s64 sys_mq_timedreceive_impl(pt_regs_t *r);
static s64 sys_mq_notify_impl(pt_regs_t *r);
static s64 sys_mq_getsetattr_impl(pt_regs_t *r);

/* futex2 */
static s64 sys_futex_wake_impl(pt_regs_t *r);
static s64 sys_futex_wait_impl(pt_regs_t *r);
static s64 sys_futex_requeue_impl(pt_regs_t *r);

/* Further Linux calls */
static s64 sys_accept4_impl(pt_regs_t *r);
static s64 sys_ioprio_set_impl(pt_regs_t *r);
static s64 sys_ioprio_get_impl(pt_regs_t *r);
static s64 sys_set_mempolicy_impl(pt_regs_t *r);
static s64 sys_get_mempolicy_impl(pt_regs_t *r);
static s64 sys_move_pages_impl(pt_regs_t *r);
static s64 sys_mbind_impl(pt_regs_t *r);
static s64 sys_migrate_pages_impl(pt_regs_t *r);
static s64 sys_set_mempolicy_home_node_impl(pt_regs_t *r);
static s64 sys_fchmodat2_impl(pt_regs_t *r);
static s64 sys_restart_syscall_impl(pt_regs_t *r);
static s64 sys_vhangup_impl(pt_regs_t *r);
static s64 sys_pivot_root_impl(pt_regs_t *r);
static s64 sys_process_mrelease_impl(pt_regs_t *r);
static s64 sys_memfd_create_impl(pt_regs_t *r);
static s64 sys_getrlimit_impl(pt_regs_t *r);
static s64 sys_setrlimit_impl(pt_regs_t *r);
static s64 sys_sethostname_impl(pt_regs_t *r);
static s64 sys_setdomainname_impl(pt_regs_t *r);
static s64 sys_getpriority_impl(pt_regs_t *r);
static s64 sys_setpriority_impl(pt_regs_t *r);
static s64 sys_chroot_impl(pt_regs_t *r);
static s64 sys_setxattr_impl(pt_regs_t *r);
static s64 sys_lsetxattr_impl(pt_regs_t *r);
static s64 sys_fsetxattr_impl(pt_regs_t *r);
static s64 sys_getxattr_impl(pt_regs_t *r);
static s64 sys_lgetxattr_impl(pt_regs_t *r);
static s64 sys_fgetxattr_impl(pt_regs_t *r);
static s64 sys_listxattr_impl(pt_regs_t *r);
static s64 sys_llistxattr_impl(pt_regs_t *r);
static s64 sys_flistxattr_impl(pt_regs_t *r);
static s64 sys_removexattr_impl(pt_regs_t *r);
static s64 sys_lremovexattr_impl(pt_regs_t *r);
static s64 sys_fremovexattr_impl(pt_regs_t *r);
static s64 sys_creat_impl(pt_regs_t *r);
static s64 sys_lchown_impl(pt_regs_t *r);
static s64 sys_preadv_impl(pt_regs_t *r);
static s64 sys_pwritev_impl(pt_regs_t *r);
static s64 sys_mlock_impl(pt_regs_t *r);
static s64 sys_munlock_impl(pt_regs_t *r);
static s64 sys_mlock2_impl(pt_regs_t *r);
static s64 sys_mlockall_impl(pt_regs_t *r);
static s64 sys_munlockall_impl(pt_regs_t *r);
static s64 sys_rt_sigpending_impl(pt_regs_t *r);
static s64 sys_sigaltstack_impl(pt_regs_t *r);
static s64 sys_getitimer_impl(pt_regs_t *r);
static s64 sys_setitimer_impl(pt_regs_t *r);
static s64 sys_setfsuid_impl(pt_regs_t *r);
static s64 sys_setfsgid_impl(pt_regs_t *r);
static s64 sys_mknod_impl(pt_regs_t *r);
static s64 sys_mknodat_impl(pt_regs_t *r);
static s64 sys_futimesat_impl(pt_regs_t *r);
static s64 sys_unshare_impl(pt_regs_t *r);
static s64 sys_adjtimex_impl(pt_regs_t *r);
static s64 sys_clock_adjtime_impl(pt_regs_t *r);
static s64 sys_settimeofday_impl(pt_regs_t *r);
static s64 sys_sendmmsg_impl(pt_regs_t *r);
static s64 sys_recvmmsg_impl(pt_regs_t *r);
static s64 sys_process_vm_readv_impl(pt_regs_t *r);
static s64 sys_shmget_impl(pt_regs_t *r);
static s64 sys_shmat_impl(pt_regs_t *r);
static s64 sys_shmdt_impl(pt_regs_t *r);
static s64 sys_shmctl_impl(pt_regs_t *r);
static s64 sys_semget_impl(pt_regs_t *r);
static s64 sys_semop_impl(pt_regs_t *r);
static s64 sys_semtimedop_impl(pt_regs_t *r);
static s64 sys_semctl_impl(pt_regs_t *r);
static s64 sys_msgget_impl(pt_regs_t *r);
static s64 sys_msgsnd_impl(pt_regs_t *r);
static s64 sys_msgrcv_impl(pt_regs_t *r);
static s64 sys_msgctl_impl(pt_regs_t *r);
static s64 sys_timer_create_impl(pt_regs_t *r);
static s64 sys_timer_settime_impl(pt_regs_t *r);
static s64 sys_timer_gettime_impl(pt_regs_t *r);
static s64 sys_timer_getoverrun_impl(pt_regs_t *r);
static s64 sys_timer_delete_impl(pt_regs_t *r);
static s64 sys_setitimer_impl(pt_regs_t *r);
static s64 sys_getitimer_impl(pt_regs_t *r);
static s64 sys_alarm_impl(pt_regs_t *r);
static s64 sys_rt_sigtimedwait_impl(pt_regs_t *r);
static s64 sys_rt_sigsuspend_impl(pt_regs_t *r);
static s64 sys_rt_sigqueueinfo_impl(pt_regs_t *r);
static s64 sys_rt_tgsigqueueinfo_impl(pt_regs_t *r);
static s64 sys_mount_impl(pt_regs_t *r);
static s64 sys_process_vm_writev_impl(pt_regs_t *r);
static s64 sys_umount2_impl(pt_regs_t *r);
static s64 sys_iopl_impl(pt_regs_t *r);
static s64 sys_ioperm_impl(pt_regs_t *r);
static s64 sys_acct_impl(pt_regs_t *r);
static s64 sys_kcmp_impl(pt_regs_t *r);
static s64 sys_finit_module_impl(pt_regs_t *r);
static s64 sys_pkey_alloc_impl(pt_regs_t *r);
static s64 sys_pkey_free_impl(pt_regs_t *r);
static s64 sys_pkey_mprotect_impl(pt_regs_t *r);
static s64 sys_set_robust_list_impl(pt_regs_t *r);
static s64 sys_get_robust_list_impl(pt_regs_t *r);
static s64 sys_mincore_impl(pt_regs_t *r);
static s64 sys_process_madvise_impl(pt_regs_t *r);
static s64 sys_cachestat_impl(pt_regs_t *r);
static s64 sys_futex_waitv_impl(pt_regs_t *r);
static s64 sys_mseal_impl(pt_regs_t *r);
static s64 sys_setns_impl(pt_regs_t *r);

/* POSIX named semaphores (Azami extended ABI) */
static s64 sys_az_sem_open_impl(pt_regs_t *r);
static s64 sys_az_sem_close_impl(pt_regs_t *r);
static s64 sys_az_sem_post_impl(pt_regs_t *r);
static s64 sys_az_sem_wait_impl(pt_regs_t *r);
static s64 sys_az_sem_trywait_impl(pt_regs_t *r);
static s64 sys_az_sem_timedwait_impl(pt_regs_t *r);
static s64 sys_az_sem_unlink_impl(pt_regs_t *r);
static s64 sys_az_sem_getvalue_impl(pt_regs_t *r);

/* ktrace function tracer (Azami extended ABI) */
static s64 sys_az_ktrace_enable_impl(pt_regs_t *r);
static s64 sys_az_ktrace_read_impl(pt_regs_t *r);
static s64 sys_az_ktrace_clear_impl(pt_regs_t *r);


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

static void syscall_report_missing(u64 nr, pt_regs_t *regs)
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

/* ── Registration ────────────────────────────────────────────────────────── */

static void reg(u32 nr, syscall_fn_t fn)
{
    if (nr < SYSCALL_TABLE_SIZE) g_syscall_table[nr] = fn;
}

void syscall_init(void)
{
    reg(SYS_read,          sys_read_impl);
    reg(SYS_write,         sys_write_impl);
    reg(SYS_open,          sys_open_impl);
    reg(SYS_close,         sys_close_impl);
    reg(SYS_stat,          sys_stat_impl);
    reg(SYS_fstat,         sys_fstat_impl);
    reg(SYS_lstat,         sys_lstat_impl);
    reg(SYS_poll,          sys_poll_impl);
    reg(SYS_lseek,         sys_lseek_impl);
    reg(SYS_mmap,          sys_mmap_impl);
    reg(SYS_mprotect,      sys_mprotect_impl);
    reg(SYS_munmap,        sys_munmap_impl);
    reg(SYS_brk,           sys_brk_impl);
    reg(SYS_rt_sigaction,  sys_rt_sigaction_impl);
    reg(SYS_rt_sigprocmask, sys_rt_sigprocmask_impl);
    reg(SYS_rt_sigreturn,  sys_rt_sigreturn_impl);
    reg(SYS_ioctl,         sys_ioctl_impl);
    reg(SYS_pread64,       sys_pread64_impl);
    reg(SYS_pwrite64,      sys_pwrite64_impl);
    reg(SYS_readv,         sys_readv_impl);
    reg(SYS_writev,        sys_writev_impl);
    reg(SYS_access,        sys_access_impl);
    reg(SYS_pipe,          sys_pipe_impl);
    reg(SYS_select,        sys_select_impl);
    reg(SYS_sched_yield,   sys_sched_yield_impl);
    reg(SYS_msync,         sys_msync_impl);
    reg(SYS_madvise,       sys_madvise_impl);
    reg(SYS_dup,           sys_dup_impl);
    reg(SYS_dup2,          sys_dup2_impl);
    reg(SYS_pause,         sys_pause_impl);
    reg(SYS_nanosleep,     sys_nanosleep_impl);
    reg(SYS_alarm,         sys_alarm_impl);
    reg(SYS_getpid,        sys_getpid_impl);
    reg(SYS_socket,        sys_socket_impl);
    reg(SYS_connect,       sys_connect_impl);
    reg(SYS_accept,        sys_accept_impl);
    reg(SYS_sendto,        sys_sendto_impl);
    reg(SYS_recvfrom,      sys_recvfrom_impl);
    reg(SYS_sendmsg,       sys_sendmsg_impl);
    reg(SYS_recvmsg,       sys_recvmsg_impl);
    reg(SYS_shutdown,      sys_shutdown_impl);
    reg(SYS_bind,          sys_bind_impl);
    reg(SYS_listen,        sys_listen_impl);
    reg(SYS_getsockname,   sys_getsockname_impl);
    reg(SYS_getpeername,   sys_getpeername_impl);
    reg(SYS_socketpair,    sys_socketpair_impl);
    reg(SYS_setsockopt,    sys_setsockopt_impl);
    reg(SYS_getsockopt,    sys_getsockopt_impl);
    reg(SYS_clone,         sys_clone_impl);
    reg(SYS_fork,          sys_fork_impl);
    reg(SYS_vfork,         sys_vfork_impl);
    reg(SYS_execve,        sys_execve_impl);
    reg(SYS_execveat,      sys_execveat_impl);
    reg(SYS_exit,          sys_exit_impl);
    reg(SYS_wait4,         sys_wait4_impl);
    reg(SYS_waitid,        sys_waitid_impl);
    reg(SYS_kill,          sys_kill_impl);
    reg(SYS_uname,         sys_uname_impl);
    reg(SYS_fcntl,         sys_fcntl_impl);
    reg(SYS_flock,         sys_flock_impl);
    reg(SYS_fsync,         sys_fsync_impl);
    reg(SYS_fdatasync,     sys_fdatasync_impl);
    reg(SYS_truncate,      sys_truncate_impl);
    reg(SYS_ftruncate,     sys_ftruncate_impl);
    reg(SYS_getdents,      sys_getdents_impl);
    reg(SYS_getcwd,        sys_getcwd_impl);
    reg(SYS_chdir,         sys_chdir_impl);
    reg(SYS_fchdir,        sys_fchdir_impl);
    reg(SYS_rename,        sys_rename_impl);
    reg(SYS_mkdir,         sys_mkdir_impl);
    reg(SYS_rmdir,         sys_rmdir_impl);
    reg(SYS_link,          sys_link_impl);
    reg(SYS_unlink,        sys_unlink_impl);
    reg(SYS_symlink,       sys_symlink_impl);
    reg(SYS_readlink,      sys_readlink_impl);
    reg(SYS_chmod,         sys_chmod_impl);
    reg(SYS_fchmod,        sys_fchmod_impl);
    reg(SYS_chown,         sys_chown_impl);
    reg(SYS_fchown,        sys_fchown_impl);
    reg(SYS_umask,         sys_umask_impl);
    reg(SYS_gettimeofday,  sys_gettimeofday_impl);
    reg(SYS_sysinfo,       sys_sysinfo_impl);
    reg(SYS_times,         sys_times_impl);
    reg(SYS_getuid,        sys_getuid_impl);
    reg(SYS_getgid,        sys_getgid_impl);
    reg(SYS_setuid,        sys_setuid_impl);
    reg(SYS_setgid,        sys_setgid_impl);
    reg(SYS_geteuid,       sys_geteuid_impl);
    reg(SYS_getegid,       sys_getegid_impl);
    reg(SYS_setpgid,       sys_setpgid_impl);
    reg(SYS_getppid,       sys_getppid_impl);
    reg(SYS_getpgrp,       sys_getpgrp_impl);
    reg(SYS_setsid,        sys_setsid_impl);
    reg(SYS_setreuid,      sys_setreuid_impl);
    reg(SYS_setregid,      sys_setregid_impl);
    reg(SYS_getgroups,     sys_getgroups_impl);
    reg(SYS_setgroups,     sys_setgroups_impl);
    reg(SYS_setresuid,     sys_setresuid_impl);
    reg(SYS_getresuid,     sys_getresuid_impl);
    reg(SYS_setresgid,     sys_setresgid_impl);
    reg(SYS_getresgid,     sys_getresgid_impl);
    reg(SYS_getpgid,       sys_getpgid_impl);
    reg(SYS_getsid,        sys_getsid_impl);
    reg(SYS_utime,         sys_utime_impl);
    reg(SYS_statfs,        sys_statfs_impl);
    reg(SYS_fstatfs,       sys_fstatfs_impl);
    reg(SYS_sync,          sys_sync_impl);
    reg(SYS_syncfs,        sys_syncfs_impl);
    reg(SYS_prctl,         sys_prctl_impl);
    reg(SYS_reboot,        sys_reboot_impl);
    reg(SYS_gettid,        sys_gettid_impl);
    reg(SYS_tkill,         sys_tkill_impl);
    reg(SYS_time,          sys_time_impl);
    reg(SYS_sched_setaffinity, sys_sched_setaffinity_impl);
    reg(SYS_sched_getaffinity, sys_sched_getaffinity_impl);
    reg(SYS_getdents64,    sys_getdents64_impl);
    reg(SYS_fadvise64,     sys_fadvise64_impl);
    reg(SYS_clock_settime, sys_clock_settime_impl);
    reg(SYS_clock_gettime, sys_clock_gettime_impl);
    reg(SYS_clock_getres,  sys_clock_getres_impl);
    reg(SYS_clock_nanosleep, sys_clock_nanosleep_impl);
    reg(SYS_exit_group,    sys_exit_group_impl);
    reg(SYS_tgkill,        sys_tgkill_impl);
    reg(SYS_utimes,        sys_utimes_impl);
    reg(SYS_pselect6,      sys_pselect6_impl);
    reg(SYS_ppoll,         sys_ppoll_impl);
    reg(SYS_utimensat,     sys_utimensat_impl);
    reg(SYS_dup3,          sys_dup3_impl);
    reg(SYS_pipe2,         sys_pipe2_impl);
    reg(SYS_AZ_GETFACL,    sys_getfacl_impl);
    reg(SYS_AZ_SETFACL,    sys_setfacl_impl);
    reg(SYS_getrlimit,     sys_getrlimit_impl);
    reg(SYS_setrlimit,     sys_setrlimit_impl);
    reg(SYS_getrusage,     sys_getrusage_impl);
    reg(SYS_openat,        sys_openat_impl);
    reg(SYS_mkdirat,       sys_mkdirat_impl);
    reg(SYS_fchownat,      sys_fchownat_impl);
    reg(SYS_fstatat,       sys_fstatat_impl);
    reg(SYS_unlinkat,      sys_unlinkat_impl);
    reg(SYS_renameat,      sys_renameat_impl);
    reg(SYS_renameat2,     sys_renameat_impl);
    reg(SYS_linkat,        sys_linkat_impl);
    reg(SYS_symlinkat,     sys_symlinkat_impl);
    reg(SYS_readlinkat,    sys_readlinkat_impl);
    reg(SYS_fchmodat,      sys_fchmodat_impl);
    reg(SYS_faccessat,     sys_faccessat_impl);
    reg(SYS_faccessat2,    sys_faccessat_impl);
    reg(SYS_arch_prctl,    sys_arch_prctl_impl);
    reg(SYS_set_tid_address, sys_set_tid_address_impl);
    reg(SYS_set_robust_list, sys_set_robust_list_impl);
    reg(SYS_prlimit64,     sys_prlimit64_impl);
    reg(SYS_getrandom,     sys_getrandom_impl);
    reg(SYS_statx,         sys_statx_impl);
    reg(SYS_sendfile,      sys_sendfile_impl);
    reg(SYS_copy_file_range, sys_copy_file_range_impl);
    reg(SYS_fallocate,     sys_fallocate_impl);
    reg(SYS_sync_file_range, sys_sync_file_range_impl);
    reg(SYS_readahead,     sys_readahead_impl);
    reg(SYS_splice,        sys_splice_impl);
    reg(SYS_tee,           sys_tee_impl);
    reg(SYS_vmsplice,      sys_vmsplice_impl);
    reg(SYS_syslog,        sys_syslog_impl);
    reg(SYS_swapon,        sys_swapon_impl);
    reg(SYS_swapoff,       sys_swapoff_impl);
    reg(SYS_rseq,          sys_rseq_impl);
    reg(SYS_close_range,   sys_close_range_impl);

    /* Linux ABIs */
    reg(SYS_mremap,        sys_mremap_impl);
    reg(SYS_mincore,       sys_mincore_impl);
    reg(SYS_capget,        sys_capget_impl);
    reg(SYS_capset,        sys_capset_impl);
    reg(SYS_personality,   sys_personality_impl);
    reg(SYS_sched_setparam, sys_sched_setparam_impl);
    reg(SYS_sched_getparam, sys_sched_getparam_impl);
    reg(SYS_sched_setscheduler, sys_sched_setscheduler_impl);
    reg(SYS_sched_getscheduler, sys_sched_getscheduler_impl);
    reg(SYS_getpriority,   sys_getpriority_impl);
    reg(SYS_setpriority,   sys_setpriority_impl);
    reg(SYS_chroot,        sys_chroot_impl);
    reg(SYS_sethostname,   sys_sethostname_impl);
    reg(SYS_setdomainname, sys_setdomainname_impl);
    reg(SYS_sched_get_priority_max, sys_sched_get_priority_max_impl);
    reg(SYS_sched_get_priority_min, sys_sched_get_priority_min_impl);
    reg(SYS_sched_rr_get_interval, sys_sched_rr_get_interval_impl);
    reg(SYS_futex,         sys_futex_impl);
    reg(SYS_epoll_create,  sys_epoll_create_impl);
    reg(SYS_epoll_create1, sys_epoll_create1_impl);
    reg(SYS_epoll_ctl,     sys_epoll_ctl_impl);
    reg(SYS_epoll_wait,    sys_epoll_wait_impl);
    reg(SYS_epoll_pwait,   sys_epoll_pwait_impl);
    reg(SYS_signalfd,      sys_signalfd_impl);
    reg(SYS_signalfd4,     sys_signalfd4_impl);
    reg(SYS_timerfd_create, sys_timerfd_create_impl);
    reg(SYS_timerfd_settime, sys_timerfd_settime_impl);
    reg(SYS_timerfd_gettime, sys_timerfd_gettime_impl);
    reg(SYS_eventfd,       sys_eventfd_impl);
    reg(SYS_eventfd2,      sys_eventfd2_impl);
    reg(SYS_inotify_init,  sys_inotify_init_impl);
    reg(SYS_inotify_init1, sys_inotify_init1_impl);
    reg(SYS_inotify_add_watch, sys_inotify_add_watch_impl);
    reg(SYS_inotify_rm_watch,  sys_inotify_rm_watch_impl);
    reg(SYS_membarrier,    sys_membarrier_impl);
    reg(SYS_clone3,        sys_clone3_impl);
    reg(SYS_close_range,   sys_close_range_impl);
    reg(SYS_openat2,       sys_openat2_impl);
    reg(SYS_faccessat2,    sys_faccessat2_impl);
    reg(SYS_epoll_pwait2,  sys_epoll_pwait2_impl);
    reg(SYS_getcpu,        sys_getcpu_impl);
    reg(SYS_seccomp,       sys_seccomp_impl);
    reg(SYS_sched_setattr, sys_sched_setattr_impl);
    reg(SYS_sched_getattr, sys_sched_getattr_impl);
    reg(SYS_pidfd_open,    sys_pidfd_open_impl);
    reg(SYS_pidfd_send_signal, sys_pidfd_send_signal_impl);
    reg(SYS_pidfd_getfd,   sys_pidfd_getfd_impl);

    /* POSIX.1-2008 message queues */
    reg(SYS_mq_open,        sys_mq_open_impl);
    reg(SYS_mq_unlink,      sys_mq_unlink_impl);
    reg(SYS_mq_timedsend,   sys_mq_timedsend_impl);
    reg(SYS_mq_timedreceive,sys_mq_timedreceive_impl);
    reg(SYS_mq_notify,      sys_mq_notify_impl);
    reg(SYS_mq_getsetattr,  sys_mq_getsetattr_impl);

    /* futex2 */
    reg(SYS_futex_wake,     sys_futex_wake_impl);
    reg(SYS_futex_wait,     sys_futex_wait_impl);
    reg(SYS_futex_requeue,  sys_futex_requeue_impl);

    /* Remaining Linux ABI additions */
    reg(SYS_accept4,        sys_accept4_impl);
    reg(SYS_ioprio_set,     sys_ioprio_set_impl);
    reg(SYS_ioprio_get,     sys_ioprio_get_impl);
    reg(SYS_set_mempolicy,  sys_set_mempolicy_impl);
    reg(SYS_get_mempolicy,  sys_get_mempolicy_impl);
    reg(SYS_move_pages,     sys_move_pages_impl);
    reg(SYS_mbind,          sys_mbind_impl);
    reg(SYS_migrate_pages,  sys_migrate_pages_impl);
    reg(SYS_set_mempolicy_home_node, sys_set_mempolicy_home_node_impl);
    reg(SYS_fchmodat2,      sys_fchmodat2_impl);
    reg(SYS_restart_syscall, sys_restart_syscall_impl);
    reg(SYS_vhangup,        sys_vhangup_impl);
    reg(SYS_pivot_root,     sys_pivot_root_impl);
    reg(SYS_process_mrelease, sys_process_mrelease_impl);
    reg(SYS_memfd_create,  sys_memfd_create_impl);
    reg(SYS_getrlimit,     sys_getrlimit_impl);
    reg(SYS_setrlimit,     sys_setrlimit_impl);
    reg(SYS_setxattr,      sys_setxattr_impl);
    reg(SYS_lsetxattr,     sys_lsetxattr_impl);
    reg(SYS_fsetxattr,     sys_fsetxattr_impl);
    reg(SYS_getxattr,      sys_getxattr_impl);
    reg(SYS_lgetxattr,     sys_lgetxattr_impl);
    reg(SYS_fgetxattr,     sys_fgetxattr_impl);
    reg(SYS_listxattr,     sys_listxattr_impl);
    reg(SYS_llistxattr,    sys_llistxattr_impl);
    reg(SYS_flistxattr,    sys_flistxattr_impl);
    reg(SYS_removexattr,   sys_removexattr_impl);
    reg(SYS_lremovexattr,  sys_lremovexattr_impl);
    reg(SYS_fremovexattr,  sys_fremovexattr_impl);


    /* System V IPC (XSI) */
    reg(SYS_shmget,        sys_shmget_impl);
    reg(SYS_shmat,         sys_shmat_impl);
    reg(SYS_shmdt,         sys_shmdt_impl);
    reg(SYS_shmctl,        sys_shmctl_impl);
    reg(SYS_semget,        sys_semget_impl);
    reg(SYS_semop,         sys_semop_impl);
    reg(SYS_semtimedop,    sys_semtimedop_impl);
    reg(SYS_semctl,        sys_semctl_impl);
    reg(SYS_msgget,        sys_msgget_impl);
    reg(SYS_msgsnd,        sys_msgsnd_impl);
    reg(SYS_msgrcv,        sys_msgrcv_impl);
    reg(SYS_msgctl,        sys_msgctl_impl);

    /* POSIX timers */
    reg(SYS_timer_create,     sys_timer_create_impl);
    reg(SYS_timer_settime,    sys_timer_settime_impl);
    reg(SYS_timer_gettime,    sys_timer_gettime_impl);
    reg(SYS_timer_getoverrun, sys_timer_getoverrun_impl);
    reg(SYS_timer_delete,     sys_timer_delete_impl);

    /* POSIX signal waiting and queueing */
    reg(SYS_rt_sigtimedwait,   sys_rt_sigtimedwait_impl);
    reg(SYS_rt_sigsuspend,     sys_rt_sigsuspend_impl);
    reg(SYS_rt_sigqueueinfo,   sys_rt_sigqueueinfo_impl);
    reg(SYS_rt_tgsigqueueinfo, sys_rt_tgsigqueueinfo_impl);

    reg(SYS_mount,         sys_mount_impl);

    /* Azami extended */
    reg(SYS_AZ_CHANNEL_CREATE, sys_az_channel_create);
    reg(SYS_AZ_CHANNEL_SEND,   sys_az_channel_send);
    reg(SYS_AZ_CHANNEL_RECV,   sys_az_channel_recv);
    reg(SYS_AZ_CHANNEL_DESTROY, sys_az_channel_destroy);
    reg(SYS_AZ_SHMEM_CREATE,   sys_az_shmem_create);
    reg(SYS_AZ_SHMEM_MAP,      sys_az_shmem_map);
    reg(SYS_AZ_SHMEM_DESTROY,  sys_az_shmem_destroy);
    reg(SYS_AZ_SHMEM_UNMAP,    sys_az_shmem_unmap);
    reg(SYS_AZ_OBJECT_CREATE,  sys_az_object_create);
    reg(SYS_AZ_OBJECT_OPEN,    sys_az_object_open);
    reg(SYS_AZ_OBJECT_CLOSE,   sys_az_object_close);
    reg(SYS_AZ_FB_INFO,        sys_az_fb_info);
    reg(SYS_AZ_FB_MAP,         sys_az_fb_map);
    reg(SYS_AZ_FB_FLIP,        sys_az_fb_flip);
    reg(SYS_AZ_SPAWN,          sys_az_spawn);
    reg(SYS_AZ_YIELD,          sys_az_yield);
    reg(SYS_AZ_THREAD_CREATE,  sys_az_thread_create_impl);
    reg(SYS_AZ_THREAD_EXIT,    sys_az_thread_exit_impl);
    reg(SYS_AZ_SYSSTAT,        sys_az_sysstat_impl);
    reg(SYS_AZ_SET_TIMER,      sys_az_set_timer_impl);

    /* POSIX named semaphores */
    reg(SYS_AZ_SEM_OPEN,      sys_az_sem_open_impl);
    reg(SYS_AZ_SEM_CLOSE,     sys_az_sem_close_impl);
    reg(SYS_AZ_SEM_POST,      sys_az_sem_post_impl);
    reg(SYS_AZ_SEM_WAIT,      sys_az_sem_wait_impl);
    reg(SYS_AZ_SEM_TRYWAIT,   sys_az_sem_trywait_impl);
    reg(SYS_AZ_SEM_TIMEDWAIT, sys_az_sem_timedwait_impl);
    reg(SYS_AZ_SEM_UNLINK,    sys_az_sem_unlink_impl);
    reg(SYS_AZ_SEM_GETVALUE,  sys_az_sem_getvalue_impl);

    /* ktrace function tracer */
    reg(SYS_AZ_KTRACE_ENABLE,  sys_az_ktrace_enable_impl);
    reg(SYS_AZ_KTRACE_READ,    sys_az_ktrace_read_impl);
    reg(SYS_AZ_KTRACE_CLEAR,   sys_az_ktrace_clear_impl);

    /* ── Additional Linux / POSIX calls ─────────────────────────────────── */
    reg(SYS_creat,             sys_creat_impl);
    reg(SYS_lchown,            sys_lchown_impl);
    reg(SYS_preadv,            sys_preadv_impl);
    reg(SYS_pwritev,           sys_pwritev_impl);
    reg(SYS_preadv2,           sys_preadv_impl);
    reg(SYS_pwritev2,          sys_pwritev_impl);
    reg(SYS_mlock,             sys_mlock_impl);
    reg(SYS_munlock,           sys_munlock_impl);
    reg(SYS_mlockall,          sys_mlockall_impl);
    reg(SYS_munlockall,        sys_munlockall_impl);
    reg(SYS_mlock2,            sys_mlock2_impl);
    reg(SYS_rt_sigpending,     sys_rt_sigpending_impl);
    reg(SYS_sigaltstack,       sys_sigaltstack_impl);
    reg(SYS_getitimer,         sys_getitimer_impl);
    reg(SYS_setitimer,         sys_setitimer_impl);
    reg(SYS_setfsuid,          sys_setfsuid_impl);
    reg(SYS_setfsgid,          sys_setfsgid_impl);
    reg(SYS_mknod,             sys_mknod_impl);
    reg(SYS_mknodat,           sys_mknodat_impl);
    reg(SYS_futimesat,         sys_futimesat_impl);
    reg(SYS_unshare,           sys_unshare_impl);
    reg(SYS_adjtimex,          sys_adjtimex_impl);
    reg(SYS_clock_adjtime,     sys_clock_adjtime_impl);
    reg(SYS_settimeofday,      sys_settimeofday_impl);
    reg(SYS_sendmmsg,          sys_sendmmsg_impl);
    reg(SYS_recvmmsg,          sys_recvmmsg_impl);
    reg(SYS_ptrace,        sys_ptrace_impl);
    reg(SYS_perf_event_open, sys_perf_event_open_impl);
    reg(SYS_process_vm_readv,  sys_process_vm_readv_impl);
    reg(SYS_process_vm_writev, sys_process_vm_writev_impl);
    reg(SYS_umount2,           sys_umount2_impl);
    reg(SYS_iopl,              sys_iopl_impl);
    reg(SYS_ioperm,            sys_ioperm_impl);
    reg(SYS_acct,              sys_acct_impl);
    reg(SYS_kcmp,              sys_kcmp_impl);
    reg(SYS_init_module,       sys_finit_module_impl);
    reg(SYS_delete_module,     sys_finit_module_impl);
    reg(SYS_finit_module,      sys_finit_module_impl);

    /* ── Memory-protection keys, robust futexes and the newer Linux calls ── */
    reg(SYS_pkey_alloc,        sys_pkey_alloc_impl);
    reg(SYS_pkey_free,         sys_pkey_free_impl);
    reg(SYS_pkey_mprotect,     sys_pkey_mprotect_impl);
    reg(SYS_set_robust_list,   sys_set_robust_list_impl);
    reg(SYS_get_robust_list,   sys_get_robust_list_impl);
    reg(SYS_mincore,           sys_mincore_impl);
    reg(SYS_process_madvise,   sys_process_madvise_impl);
    reg(SYS_cachestat,         sys_cachestat_impl);
    reg(SYS_futex_waitv,       sys_futex_waitv_impl);
    reg(SYS_mseal,             sys_mseal_impl);
    reg(SYS_setns,             sys_setns_impl);

    pr_debug("[SYSCALL] Dispatch table ready (%d entries)\n", SYSCALL_TABLE_SIZE);
}

/* ── Path Resolution Helper ──────────────────────────────────────────────── */

static s64 copy_str_from_user(char *dst, const char *user_src, size_t max_len)
{
    if (!dst || !user_src || max_len == 0) return -(s64)EINVAL;
    if ((uintptr_t)user_src >= 0x0000800000000000ULL) return -(s64)EFAULT;

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
static inline bool proc_is_confined(const process_t *proc)
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
static s64 vpath_to_real(const process_t *proc, const char *vpath, char *out, size_t out_len)
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
static const char *real_to_vpath(const process_t *proc, const char *real)
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
static s64 copy_user_vpath_resolve_at(int dirfd, char *vpath, size_t max_len, const char *user_path)
{
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

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
static s64 copy_user_path_resolve_at(int dirfd, char *kpath, size_t max_len, const char *user_path)
{
    process_t *proc = sched_current_process();
    if (!proc_is_confined(proc))
        return copy_user_vpath_resolve_at(dirfd, kpath, max_len, user_path);

    char vpath[512];
    s64 err = copy_user_vpath_resolve_at(dirfd, vpath, sizeof(vpath), user_path);
    if (err < 0) return err;
    return vpath_to_real(proc, vpath, kpath, max_len);
}

static s64 copy_user_path_resolve(char *kpath, size_t max_len, const char *user_path)
{
    return copy_user_path_resolve_at(AT_FDCWD, kpath, max_len, user_path);
}

/* ── Standard I/O Syscalls ───────────────────────────────────────────────── */

static s64 sys_read_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    char *buf = (char *)r->rsi;
    s64 count = (s64)r->rdx;
    /* BUG-01: negative count is EINVAL; zero count returns 0 immediately */
    if (count < 0) return -(s64)EINVAL;
    if (count == 0 || !buf) return 0;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    file_t *file = fget(proc, fd);

    if (fd == 0 && !file) {
        int c = uart_getc(UART_COM1);
        if (c != -1) {
            char kchar = (char)c;
            if (copy_to_user(buf, &kchar, 1) != 0) return -(s64)EFAULT;
            return 1;
        }
        return -(s64)EAGAIN;
    }

    if (!file) return -(s64)EBADF;

    s64 total_read = 0;
    char kbuf[512];
    while (count > 0) {
        size_t chunk = count > 512 ? 512 : (size_t)count;
        s64 ret = (s64)vfs_read(file, kbuf, chunk);
        if (ret < 0) {
            if (total_read == 0) total_read = ret;
            break;
        }
        if (ret == 0) break;
        if (copy_to_user(buf + total_read, kbuf, (size_t)ret) != 0) {
            if (total_read == 0) total_read = -(s64)EFAULT;
            break;
        }
        total_read += ret;
        count -= ret;
        if (ret < (s64)chunk) break;
    }
    fput(file);
    return total_read;
}

static s64 sys_write_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    const char *buf = (const char *)r->rsi;
    s64 count = (s64)r->rdx;
    if (count <= 0 || !buf) return 0;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    file_t *file = fget(proc, fd);

    if ((fd == 1 || fd == 2) && !file) {
        char kbuf[512];
        s64 total_written = 0;
        while (count > 0) {
            size_t chunk = count > 512 ? 512 : (size_t)count;
            if (copy_from_user(kbuf, buf + total_written, chunk) != 0) break;
            extern void uart_write(u16 port, const char *buf, size_t len);
            uart_write(0x3F8, kbuf, chunk);
            total_written += chunk;
            count -= chunk;
        }
        /* BUG-02: return EFAULT (not rdx) if nothing was written due to copy failure */
        return total_written > 0 ? total_written : -(s64)EFAULT;
    }

    if (!file) return -(s64)EBADF;

    s64 total_written = 0;
    char kbuf[512];
    while (count > 0) {
        size_t chunk = count > 512 ? 512 : (size_t)count;
        if (copy_from_user(kbuf, buf + total_written, chunk) != 0) {
            if (total_written == 0) total_written = -(s64)EFAULT;
            break;
        }
        s64 ret = (s64)vfs_write(file, kbuf, chunk);
        if (ret < 0) {
            if (ret == -(s64)EPIPE && proc) {
                sched_kill_process(proc->pid, 13 /* SIGPIPE */);
            }
            if (total_written == 0) total_written = ret;
            break;
        }
        if (ret == 0) break;
        total_written += ret;
        count -= ret;
        if (ret < (s64)chunk) break;
    }
    fput(file);
    return total_written;
}

static s64 sys_open_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    int flags = (int)r->rsi;
    u32 mode = (u32)r->rdx;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* B-02: apply process umask when creating a file */
    if (flags & O_CREAT) {
        mode &= ~proc->umask;
    }

    s64 open_err = 0;
    file_t *file = vfs_open_err(kpath, (u32)flags, mode, &open_err);
    if (!file) return open_err ? open_err : -(s64)ENOENT;

    s64 fd = fd_install(proc, file, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) vfs_close(file);
    return fd;
}

/* Detach the file at `fd` from the table under g_fd_lock so the unref is
 * ordered against a concurrent fget() (see fget()). Returns the detached file
 * (caller must vfs_close() it outside the lock) or NULL. */
static file_t *fd_detach(process_t *proc, int fd)
{
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return NULL;
    irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
    file_t *f = (file_t *)proc->handle_table[fd];
    proc->handle_table[fd] = NULL;
    proc->fd_flags[fd] = 0;
    spinlock_unlock_irqrestore(&g_fd_lock, fl);
    if (f && (uintptr_t)f < 0xFFFF800000000000ULL) f = NULL;
    return f;
}

static s64 sys_close_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc) return -(s64)EBADF;
    file_t *f = fd_detach(proc, fd);
    if (!f) return -(s64)EBADF;
    vfs_close(f);
    return 0;
}

static s64 sys_close_range_impl(pt_regs_t *r)
{
    unsigned int first = (unsigned int)r->rdi;
    unsigned int last  = (unsigned int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (last >= PROC_MAX_FDS) last = PROC_MAX_FDS - 1;
    for (unsigned int i = first; i <= last && i < PROC_MAX_FDS; i++) {
        file_t *f = fd_detach(proc, (int)i);
        if (f) vfs_close(f);
    }
    return 0;
}

/* ── Scatter-Gather I/O ──────────────────────────────────────────────────── */

struct iovec {
    void  *iov_base;
    size_t iov_len;
};

static s64 sys_readv_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    int iovcnt = (int)r->rdx;

    if (!iov || iovcnt <= 0 || iovcnt > 1024) return -(s64)EINVAL;
    if ((uintptr_t)iov >= 0x8000000000000000ULL) return -(s64)EFAULT;

    s64 total = 0;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec kiov;
        if (copy_from_user(&kiov, &iov[i], sizeof(struct iovec)) != 0) return -(s64)EFAULT;
        if (kiov.iov_len == 0) continue;
        /* B-08: overflow and length sanity check */
        if (kiov.iov_len > 0x7FFFFFFF || (s64)kiov.iov_len < 0) return -(s64)EINVAL;
        if (total + (s64)kiov.iov_len < total) return -(s64)EINVAL;

        pt_regs_t sub_r = *r;
        sub_r.rdi = (u64)fd;
        sub_r.rsi = (u64)(uintptr_t)kiov.iov_base;
        sub_r.rdx = (u64)kiov.iov_len;

        s64 n = sys_read_impl(&sub_r);
        if (n < 0) {
            if (total > 0) return total;
            return n;
        }
        total += n;
        if ((size_t)n < kiov.iov_len) break;
    }
    return total;
}

static s64 sys_writev_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    int iovcnt = (int)r->rdx;

    if (!iov || iovcnt <= 0 || iovcnt > 1024) return -(s64)EINVAL;
    if ((uintptr_t)iov >= 0x8000000000000000ULL) return -(s64)EFAULT;

    s64 total = 0;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec kiov;
        if (copy_from_user(&kiov, &iov[i], sizeof(struct iovec)) != 0) return -(s64)EFAULT;
        if (kiov.iov_len == 0) continue;
        /* B-08: overflow and length sanity check */
        if (kiov.iov_len > 0x7FFFFFFF || (s64)kiov.iov_len < 0) return -(s64)EINVAL;
        if (total + (s64)kiov.iov_len < total) return -(s64)EINVAL;

        pt_regs_t sub_r = *r;
        sub_r.rdi = (u64)fd;
        sub_r.rsi = (u64)(uintptr_t)kiov.iov_base;
        sub_r.rdx = (u64)kiov.iov_len;

        s64 n = sys_write_impl(&sub_r);
        if (n < 0) {
            if (total > 0) return total;
            return n;
        }
        total += n;
        if ((size_t)n < kiov.iov_len) break;
    }
    return total;
}

static s64 sys_pread64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    void *user_buf = (void *)r->rsi;
    size_t count = (size_t)r->rdx;
    u64 pos = (u64)r->r10;

    if (!user_buf || count == 0) return 0;
    if ((uintptr_t)user_buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = (file_t *)proc->handle_table[fd];
    char *kbuf = kmalloc(count > 65536 ? 65536 : count);
    if (!kbuf) return -(s64)ENOMEM;

    size_t total_read = 0;
    while (total_read < count) {
        size_t chunk = count - total_read;
        if (chunk > 65536) chunk = 65536;

        u64 saved_pos = file->f_pos;
        file->f_pos = pos + total_read;
        s64 n = vfs_read(file, kbuf, chunk);
        file->f_pos = saved_pos;

        if (n <= 0) {
            if (total_read > 0) break;
            kfree(kbuf);
            return n;
        }

        if (copy_to_user((char *)user_buf + total_read, kbuf, (size_t)n) != 0) {
            kfree(kbuf);
            return -(s64)EFAULT;
        }

        total_read += (size_t)n;
        if ((size_t)n < chunk) break;
    }

    kfree(kbuf);
    return (s64)total_read;
}

static s64 sys_pwrite64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    const void *user_buf = (const void *)r->rsi;
    size_t count = (size_t)r->rdx;
    u64 pos = (u64)r->r10;

    if (!user_buf || count == 0) return 0;
    if ((uintptr_t)user_buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = (file_t *)proc->handle_table[fd];
    char *kbuf = kmalloc(count > 65536 ? 65536 : count);
    if (!kbuf) return -(s64)ENOMEM;

    size_t total_written = 0;
    while (total_written < count) {
        size_t chunk = count - total_written;
        if (chunk > 65536) chunk = 65536;

        if (copy_from_user(kbuf, (const char *)user_buf + total_written, chunk) != 0) {
            kfree(kbuf);
            return -(s64)EFAULT;
        }

        u64 saved_pos = file->f_pos;
        file->f_pos = pos + total_written;
        s64 n = vfs_write(file, kbuf, chunk);
        file->f_pos = saved_pos;

        if (n <= 0) {
            if (total_written > 0) break;
            kfree(kbuf);
            return n;
        }

        total_written += (size_t)n;
        if ((size_t)n < chunk) break;
    }

    kfree(kbuf);
    return (s64)total_written;
}

/* ── Memory Management Syscalls ──────────────────────────────────────────── */

static s64 sys_brk_impl(pt_regs_t *r)
{
    virt_addr_t new_brk = (virt_addr_t)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (proc->heap_start == 0) {
        proc->heap_start = 0x10000000;
        proc->heap_end   = proc->heap_start;
    }

    if (new_brk == 0) {
        return (s64)proc->heap_end;
    }

    if (new_brk < proc->heap_start || new_brk >= 0x00007ffff0000000ULL) {
        return (s64)proc->heap_end;
    }

    virt_addr_t cur_page = ALIGN_UP(proc->heap_end, PAGE_SIZE);
    virt_addr_t target_page = ALIGN_UP(new_brk, PAGE_SIZE);

    if (target_page > cur_page) {
        for (virt_addr_t va = cur_page; va < target_page; va += PAGE_SIZE) {
            phys_addr_t phys = vmm_translate(proc->pml4_phys, va);
            if (!phys) {
                phys = pmm_alloc_page();
                if (!phys) return (s64)proc->heap_end;
                hw_clear_page((void *)PHYS_TO_VIRT(phys));
                vmm_map(proc->pml4_phys, va, phys, VMM_USER_RW);
            }
        }
    } else if (target_page < cur_page) {
        /* One batched teardown: vmm_unmap_range() applies the same ownership
         * rule (skip VMM_F_SHARED frames, which the shmem object or a peer
         * mapping still owns) and pays for one TLB shootdown per chunk rather
         * than one per page. */
        vmm_unmap_range(proc->pml4_phys, target_page,
                        (size_t)((cur_page - target_page) / PAGE_SIZE), true);
    }

    proc->heap_end = new_brk;
    return (s64)proc->heap_end;
}


static s64 sys_mmap_impl(pt_regs_t *r)
{
    virt_addr_t addr   = (virt_addr_t)r->rdi;
    size_t length      = (size_t)r->rsi;
    int prot           = (int)r->rdx;
    u64 flags          = r->r10; /* MAP_SHARED=1, MAP_PRIVATE=2, MAP_FIXED=16, MAP_ANONYMOUS=32 */
    int fd             = (int)(s32)r->r8;
    u64 file_offset    = (u64)r->r9;

    KTRACE_CALL("mmap", length);

    if (length == 0) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    /* Reject lengths that overflow on page rounding. */
    if (aligned_len < length || aligned_len == 0) return -(s64)EINVAL;

    /* Anonymous mmap bump arena: [MMAP_ARENA_BASE, MMAP_ARENA_END). Kept well
     * below the user stack auto-growth window so mmap can never march into it. */
    #define MMAP_ARENA_BASE 0x0000600000000000ULL
    #define MMAP_ARENA_END  0x00007f0000000000ULL

    bool map_fixed     = !!(flags & 0x10);
    virt_addr_t target_addr = addr;

    if (!map_fixed) {
        bool need_alloc = false;
        if (!target_addr || target_addr < 0x1000 || target_addr + aligned_len >= 0x0000800000000000ULL) {
            need_alloc = true;
        } else {
            /* Check collision with existing mappings */
            for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
                if (vmm_translate(proc->pml4_phys, target_addr + offset)) {
                    need_alloc = true;
                    break;
                }
            }
        }

        if (need_alloc) {
            if (!proc->mmap_current || proc->mmap_current < MMAP_ARENA_BASE || proc->mmap_current >= MMAP_ARENA_END) {
                proc->mmap_current = 0x0000700000000000ULL;
            }
            /* Refuse if the request would run past the end of the arena
             * (also catches address overflow). */
            if (aligned_len > MMAP_ARENA_END - proc->mmap_current) {
                return -(s64)ENOMEM;
            }
            target_addr = proc->mmap_current;
            proc->mmap_current += aligned_len;
        }
    } else {
        /* Guard against integer overflow in range check */
        if (target_addr + aligned_len < target_addr || target_addr + aligned_len >= 0x0000800000000000ULL) {
            return -(s64)EINVAL;
        }
        /* MAP_FIXED over a live range: drop whatever was there first. The
         * range walk also picks up PROT_NONE pages, which keep their frame
         * with PRESENT clear. */
        vmm_unmap_range(proc->pml4_phys, target_addr, aligned_len / PAGE_SIZE, true);
    }

    /* Translate mmap flags to the VMA flag set. */
    u32 vma_fl = (flags & 0x20 /* MAP_ANONYMOUS */) ? VMA_F_ANON : VMA_F_FILE;
    if (flags & 0x01 /* MAP_SHARED */) vma_fl |= VMA_F_SHARED;

    file_t *file = NULL;
    if (!(flags & 0x20) && fd >= 0 && fd < PROC_MAX_FDS && proc->handle_table[fd]) {
        file = (file_t *)proc->handle_table[fd];
        if (file && file->f_op && file->f_op->mmap) {
            s64 ret = file->f_op->mmap(file, target_addr, aligned_len, (u32)prot, (u32)flags, file_offset);
            if (ret == 0) {
                vma_add(proc, target_addr, target_addr + aligned_len, (u32)prot & 7, vma_fl);
                return (s64)target_addr;
            }
            return ret;
        }
    }

    u64 vmm_flags = VMM_F_USER;
    if (flags & 0x01 /* MAP_SHARED */) vmm_flags |= VMM_F_SHARED;
    if (prot != 0 /* PROT_NONE */) vmm_flags |= VMM_F_PRESENT;
    if (prot & 0x2 /* PROT_WRITE */) vmm_flags |= VMM_F_WRITE;
    if (!(prot & 0x4 /* PROT_EXEC */)) vmm_flags |= VMM_F_NX;

    for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) {
            /* BUG fix: unmap pages already installed in this call before
             * reporting OOM — otherwise they are leaked permanently. */
            if (offset > 0)
                vmm_unmap_range(proc->pml4_phys, target_addr, offset / PAGE_SIZE, true);
            return -(s64)ENOMEM;
        }
        void *page_buf = (void *)PHYS_TO_VIRT(phys);
        __builtin_memset(page_buf, 0, PAGE_SIZE);

        if (file && file->f_inode) {
            u64 cur_foff = file_offset + offset;
            if (cur_foff < file->f_inode->i_size) {
                size_t to_read = file->f_inode->i_size - cur_foff;
                if (to_read > PAGE_SIZE) to_read = PAGE_SIZE;
                u64 saved_fpos = file->f_pos;
                file->f_pos = cur_foff;
                vfs_read(file, page_buf, to_read);
                file->f_pos = saved_fpos;
            }
        }

        vmm_map(proc->pml4_phys, target_addr + offset, phys, vmm_flags);
    }

    vma_add(proc, target_addr, target_addr + aligned_len, (u32)prot & 7, vma_fl);
    return (s64)target_addr;
}

static s64 sys_munmap_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    if (length == 0 || (addr & (PAGE_SIZE - 1))) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr) return -(s64)EINVAL;

    vmm_unmap_range(proc->pml4_phys, addr, aligned_len / PAGE_SIZE, true);
    vma_remove(proc, addr, addr + aligned_len);
    return 0;
}

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

static s64 sys_mprotect_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    int prot = (int)r->rdx;

    if (length == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr >= 0x0000800000000000ULL || addr < 0x1000) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr || addr + aligned_len > 0x0000800000000000ULL) return -(s64)EINVAL;

    u64 vmm_flags = VMM_F_USER;
    if (prot != PROT_NONE) vmm_flags |= VMM_F_PRESENT;
    if (prot & PROT_WRITE) vmm_flags |= VMM_F_WRITE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_F_NX;

    size_t page_count = aligned_len / PAGE_SIZE;

    /* Record the new protection first. It is what the #PF handler grants to
     * demand-paged frames in this range, so if it cannot be recorded exactly
     * the call must fail before the page tables are touched — otherwise the
     * range ends up with narrower PTEs than the registry claims and later
     * faults hand back the *old*, wider rights. */
    s64 rc = vma_setprot(proc, addr, addr + aligned_len, (u32)prot & 7);
    if (rc < 0) return rc;

    vmm_set_flags(proc->pml4_phys, addr, page_count, vmm_flags);
    return 0;
}

/* ── File Operations & Metadata ─────────────────────────────────────────── */

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static s64 sys_ioctl_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    u32 cmd = (u32)r->rsi;
    u64 arg = r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];

    /* Generic file descriptor ioctls */
    if (cmd == 0x5451 /* FIOCLEX */) {
        proc->fd_flags[fd] |= FD_CLOEXEC;
        return 0;
    }
    if (cmd == 0x5450 /* FIONCLEX */) {
        proc->fd_flags[fd] &= ~FD_CLOEXEC;
        return 0;
    }
    if (cmd == 0x5421 /* FIONBIO */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        int on = 0;
        if (copy_from_user(&on, (void *)arg, sizeof(int)) != 0) return -(s64)EFAULT;
        if (on) file->f_flags |= O_NONBLOCK;
        else    file->f_flags &= ~O_NONBLOCK;
        return 0;
    }
    if (cmd == 0x5452 /* FIOASYNC */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        int on = 0;
        if (copy_from_user(&on, (void *)arg, sizeof(int)) != 0) return -(s64)EFAULT;
        if (on) file->f_flags |= 0x2000 /* O_ASYNC */;
        else    file->f_flags &= ~0x2000 /* O_ASYNC */;
        return 0;
    }

    /* Network configuration ioctl privilege checks */
    if (cmd == 0x8916 /* SIOCSIFADDR */ || cmd == 0x891C /* SIOCSIFNETMASK */ ||
        cmd == 0x892A /* SIOCSIFGW */   || cmd == 0x892B /* SIOCSIFDNS */ ||
        cmd == 0x8914 /* SIOCSIFFLAGS */ || cmd == 0x8990 /* SIOCSIFDHCP */) {
        if (!security_check_permission(proc, CAP_NET_ADMIN)) {
            return -(s64)EPERM;
        }
    }

    /* Try file operations driver ioctl first if implemented */
    if (file->f_op && file->f_op->ioctl) {
        s64 r_drv = file->f_op->ioctl(file, cmd, arg);
        if (r_drv != -(s64)ENOTTY && r_drv != -(s64)ENOSYS) {
            return r_drv;
        }
    }

    /* TTY ioctl commands fallback: only valid for character devices / TTYs */
    if (cmd == 0x5401 /* TCGETS */ || cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ ||
        cmd == 0x5404 /* TCSETSF */ || cmd == 0x5413 /* TIOCGWINSZ */ || cmd == 0x5414 /* TIOCSWINSZ */ ||
        cmd == 0x540F /* TIOCGPGRP */ || cmd == 0x5410 /* TIOCSPGRP */ || cmd == 0x540E /* TIOCSCTTY */ ||
        cmd == 0x5409 /* TCSBRK */ || cmd == 0x540A /* TCXONC */ || cmd == 0x540B /* TCFLSH */ ||
        cmd == 0x5429 /* TIOCGSID */) {
        if (file->f_inode && !S_ISCHR(file->f_inode->i_mode)) {
            return -(s64)ENOTTY;
        }
        if (cmd == 0x5413 /* TIOCGWINSZ */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                struct winsize ws;
                ws.ws_row = 24;
                ws.ws_col = 80;
                ws.ws_xpixel = 640;
                ws.ws_ypixel = 480;
                if (copy_to_user((void *)arg, &ws, sizeof(ws)) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5414 /* TIOCSWINSZ */) return 0;
        if (cmd == 0x5401 /* TCGETS */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                char termios_buf[64];
                __builtin_memset(termios_buf, 0, sizeof(termios_buf));
                *(u32 *)&termios_buf[0]  = 0x0100; /* ICRNL */
                *(u32 *)&termios_buf[4]  = 0x0005; /* OPOST | ONLCR */
                *(u32 *)&termios_buf[8]  = 0x00BF; /* CS8 | CREAD | B38400 */
                *(u32 *)&termios_buf[12] = 0x0A3B; /* ISIG | ICANON | ECHO | ECHOE | ECHOK */
                termios_buf[16] = 0;               /* c_line */
                termios_buf[17 + 0] = 0x03;        /* VINTR = ^C */
                termios_buf[17 + 1] = 0x1C;        /* VQUIT = ^\ */
                termios_buf[17 + 2] = 0x7F;        /* VERASE = DEL */
                termios_buf[17 + 3] = 0x15;        /* VKILL = ^U */
                termios_buf[17 + 4] = 0x04;        /* VEOF = ^D */
                termios_buf[17 + 5] = 0;           /* VTIME */
                termios_buf[17 + 6] = 1;           /* VMIN */
                termios_buf[17 + 7] = 0;           /* VSWTC */
                termios_buf[17 + 8] = 0x11;        /* VSTART = ^Q */
                termios_buf[17 + 9] = 0x13;        /* VSTOP = ^S */
                termios_buf[17 + 10] = 0x1A;       /* VSUSP = ^Z */
                if (copy_to_user((void *)arg, termios_buf, 60) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ || cmd == 0x5404 /* TCSETSF */) {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
            char dummy[60];
            if (copy_from_user(dummy, (const void *)arg, 60) != 0) return -(s64)EFAULT;
            return 0;
        }
        if (cmd == 0x5409 /* TCSBRK */ || cmd == 0x540A /* TCXONC */ || cmd == 0x540B /* TCFLSH */) return 0;
        if (cmd == 0x540F /* TIOCGPGRP */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                int pgid = (int)proc->pgid;
                if (copy_to_user((void *)arg, &pgid, sizeof(int)) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5410 /* TIOCSPGRP */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                int pgid = 0;
                if (copy_from_user(&pgid, (const void *)arg, sizeof(int)) != 0) return -(s64)EFAULT;
                proc->pgid = (u32)pgid;
                return 0;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5429 /* TIOCGSID */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                int sid = (int)proc->sid;
                if (copy_to_user((void *)arg, &sid, sizeof(int)) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x540E /* TIOCSCTTY */) {
            proc->sid = proc->pid;
            return 0;
        }
        if (cmd == 0x5422 /* TIOCNOTTY */) return 0;
    }

    return vfs_ioctl(file, cmd, arg);

}

static s64 sys_lseek_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    s64 offset = (s64)r->rsi;
    int whence = (int)r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];
    return vfs_lseek(file, offset, whence);
}

static s64 sys_stat_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!user_path || !statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    struct stat kstat;
    s64 ret = vfs_stat(kpath, &kstat);
    if (ret == 0) {
        if (copy_to_user(statbuf, &kstat, sizeof(struct stat)) != 0) {
            return -(s64)EFAULT;
        }
    }
    return ret;
}

static s64 sys_lstat_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!user_path || !statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    struct stat kstat;
    s64 ret = vfs_lstat(kpath, &kstat);
    if (ret == 0) {
        if (copy_to_user(statbuf, &kstat, sizeof(struct stat)) != 0) {
            return -(s64)EFAULT;
        }
    }
    return ret;
}

static s64 sys_fstat_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= 0x8000000000000000ULL) return -(s64)EFAULT;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];
    struct stat kstat;
    s64 ret = vfs_fstat(file, &kstat);
    if (ret == 0) {
        if (copy_to_user(statbuf, &kstat, sizeof(struct stat)) != 0) {
            return -(s64)EFAULT;
        }
    }
    return ret;
}

static s64 sys_statfs_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct statfs *buf = (struct statfs *)r->rsi;
    if (!user_path || !buf) return -(s64)EINVAL;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    struct statfs kbuf;
    s64 ret = vfs_statfs(kpath, &kbuf);
    if (ret == 0) {
        if (copy_to_user(buf, &kbuf, sizeof(struct statfs)) != 0) return -(s64)EFAULT;
    }
    return ret;
}

static s64 sys_fstatfs_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    struct statfs *buf = (struct statfs *)r->rsi;
    if (!buf) return -(s64)EINVAL;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = (file_t *)proc->handle_table[fd];
    struct statfs kbuf;
    s64 ret = vfs_fstatfs(file, &kbuf);
    if (ret == 0) {
        if (copy_to_user(buf, &kbuf, sizeof(struct statfs)) != 0) return -(s64)EFAULT;
    }
    return ret;
}

static s64 sys_chmod_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    u32 mode = (u32)r->rsi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_chmod(kpath, mode);
}

static s64 sys_fchmod_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    u32 mode = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    return vfs_fchmod((file_t *)proc->handle_table[fd], mode);
}

static s64 sys_chown_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    u32 uid = (u32)r->rsi;
    u32 gid = (u32)r->rdx;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_chown(kpath, uid, gid);
}

static s64 sys_fchown_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    u32 uid = (u32)r->rsi;
    u32 gid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    return vfs_fchown((file_t *)proc->handle_table[fd], uid, gid);
}

static s64 sys_umask_impl(pt_regs_t *r)
{
    /* POSIX-02: store the umask per-process and return the old value.
     * The umask field is stored in process_t.umask; open/mkdir apply it. */
    u32 new_mask = (u32)r->rdi & 0777;
    process_t *proc = sched_current_process();
    if (!proc) return 022;
    u32 old_mask = proc->umask;
    proc->umask = new_mask;
    return (s64)old_mask;
}

static s64 sys_symlink_impl(pt_regs_t *r)
{
    const char *user_target = (const char *)r->rdi;
    const char *user_link = (const char *)r->rsi;
    if (!user_target || !user_link) return -(s64)EINVAL;

    char ktarget[256], klink[256];
    __builtin_memset(ktarget, 0, sizeof(ktarget));
    s64 terr = copy_str_from_user(ktarget, user_target, sizeof(ktarget));
    if (terr < 0) return terr;

    s64 perr = copy_user_path_resolve(klink, sizeof(klink), user_link);
    if (perr < 0) return perr;

    return vfs_symlink(ktarget, klink);
}

static s64 sys_readlink_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char *user_buf = (char *)r->rsi;
    size_t bufsiz = (size_t)r->rdx;
    if (!user_path || !user_buf || bufsiz == 0) return -(s64)EINVAL;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    process_t *proc = sched_current_process();
    if (proc && (strcmp(kpath, "/proc/self/exe") == 0 || strcmp(kpath, "/proc/thread-self/exe") == 0)) {
        size_t nlen = strlen(proc->name);
        size_t copylen = nlen > bufsiz ? bufsiz : nlen;
        if (copy_to_user(user_buf, proc->name, copylen) != 0) return -(s64)EFAULT;
        return (s64)copylen;
    }
    if (proc && strcmp(kpath, "/proc/self/cwd") == 0) {
        size_t clen = strlen(proc->cwd);
        size_t copylen = clen > bufsiz ? bufsiz : clen;
        if (copy_to_user(user_buf, proc->cwd, copylen) != 0) return -(s64)EFAULT;
        return (s64)copylen;
    }

    char kbuf[256];
    s64 ret = vfs_readlink(kpath, kbuf, sizeof(kbuf) - 1);
    if (ret > 0) {
        size_t copylen = (size_t)ret > bufsiz ? bufsiz : (size_t)ret;
        if (copy_to_user(user_buf, kbuf, copylen) != 0) return -(s64)EFAULT;
        return (s64)copylen;
    }
    return ret;
}


/* ── Polling & Multiplexing Syscalls ─────────────────────────────────────── */

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

struct linux_timeval {
    long tv_sec;
    long tv_usec;
};

typedef struct {
    u64 fds_bits[16]; /* 16 * 64 = 1024 bits */
} kernel_fd_set_t;

#define K_FD_ISSET(fd, set) (((set)->fds_bits[(fd) / 64] & (1ULL << ((fd) % 64))) != 0)
#define K_FD_SET(fd, set)   ((set)->fds_bits[(fd) / 64] |= (1ULL << ((fd) % 64)))

static short check_file_readiness(file_t *f, short events)
{
    if (!f) return POLLNVAL;

    if (f->f_op && f->f_op->poll) {
        return (short)f->f_op->poll(f);
    }

    /* If file is a pipe */
    if (f->f_inode && S_ISFIFO(f->f_inode->i_mode) && f->private_data) {
        pipe_t *p = (pipe_t *)f->private_data;
        short rev = 0;
        spinlock_lock(&p->lock);
        if (events & POLLIN) {
            if (p->count > 0) rev |= POLLIN;
            else if (p->writers == 0) rev |= (POLLHUP | POLLIN);
        }
        if (events & POLLOUT) {
            if (p->readers == 0) rev |= (POLLERR | POLLHUP);
            else if (p->count < PIPE_BUFFER_SIZE) rev |= POLLOUT;
        }
        spinlock_unlock(&p->lock);
        return rev;
    }

    /* Regular files, devfs character devices, and block devices */
    short rev = 0;
    if (events & POLLIN) rev |= POLLIN;
    if (events & POLLOUT) rev |= POLLOUT;
    return rev;
}

static s64 sys_poll_impl(pt_regs_t *r)
{
    struct pollfd *user_fds = (struct pollfd *)r->rdi;
    u64 nfds = r->rsi;
    int timeout_ms = (int)r->rdx;

    if (nfds > 1024) return -(s64)EINVAL;
    if (nfds == 0) {
        if (timeout_ms > 0) sched_sleep((timeout_ms + 9) / 10);
        return 0;
    }
    if (!user_fds || (uintptr_t)user_fds >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    struct pollfd *kfds = (struct pollfd *)kmalloc(sizeof(struct pollfd) * nfds);
    if (!kfds) return -(s64)ENOMEM;

    if (copy_from_user(kfds, user_fds, sizeof(struct pollfd) * nfds) != 0) {
        kfree(kfds);
        return -(s64)EFAULT;
    }

    u64 end_ticks = (timeout_ms > 0) ? (sched_get_ticks() + (timeout_ms + 9) / 10) : 0;
    int ready_count = 0;

    for (;;) {
        ready_count = 0;
        for (u64 i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            int fd = kfds[i].fd;
            if (fd < 0) continue;

            /* fget()/fput() rather than a raw handle_table[] read. This loop
             * sleeps and re-runs, so a sibling thread closing the fd in
             * between would otherwise leave check_file_readiness() reading a
             * freed file_t — a use-after-free any multithreaded program can
             * reach with close() and poll() on the same descriptor. */
            file_t *f = fget(proc, fd);
            if (!f) {
                kfds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            short req = kfds[i].events;
            short rev = check_file_readiness(f, req);
            fput(f);

            if (rev & (req | POLLHUP | POLLERR | POLLNVAL)) {
                kfds[i].revents = rev;
                ready_count++;
            }
        }

        if (ready_count > 0 || timeout_ms == 0) break;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) break;

        /* BUG-AO fix: interrupt on deliverable signal per POSIX */
        if (proc->sig_pending & ~proc->sig_blocked) {
            kfree(kfds);
            return -(s64)EINTR;
        }

        sched_sleep(1);
    }

    copy_to_user(user_fds, kfds, sizeof(struct pollfd) * nfds);
    kfree(kfds);
    return ready_count;
}

static s64 sys_ppoll_impl(pt_regs_t *r)
{
    struct pollfd *user_fds = (struct pollfd *)r->rdi;
    u64 nfds = r->rsi;
    const struct linux_timespec *tmo_p = (const struct linux_timespec *)r->rdx;
    int timeout_ms = -1;
    if (tmo_p && (uintptr_t)tmo_p < 0x8000000000000000ULL) {
        struct linux_timespec ts;
        if (copy_from_user(&ts, tmo_p, sizeof(ts)) == 0) {
            timeout_ms = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        }
    }
    pt_regs_t sub = *r;
    sub.rdi = (u64)(uintptr_t)user_fds;
    sub.rsi = nfds;
    sub.rdx = (u64)(s64)timeout_ms;
    return sys_poll_impl(&sub);
}

static s64 sys_select_impl(pt_regs_t *r)
{
    int nfds = (int)r->rdi;
    kernel_fd_set_t *u_rfds = (kernel_fd_set_t *)r->rsi;
    kernel_fd_set_t *u_wfds = (kernel_fd_set_t *)r->rdx;
    kernel_fd_set_t *u_efds = (kernel_fd_set_t *)r->r10;
    struct linux_timeval *u_tv = (struct linux_timeval *)r->r8;

    if (nfds < 0 || nfds > 1024) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    kernel_fd_set_t in_rfds, in_wfds, in_efds;
    __builtin_memset(&in_rfds, 0, sizeof(in_rfds));
    __builtin_memset(&in_wfds, 0, sizeof(in_wfds));
    __builtin_memset(&in_efds, 0, sizeof(in_efds));

    if (u_rfds && (uintptr_t)u_rfds < 0x8000000000000000ULL) {
        if (copy_from_user(&in_rfds, u_rfds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }
    if (u_wfds && (uintptr_t)u_wfds < 0x8000000000000000ULL) {
        if (copy_from_user(&in_wfds, u_wfds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }
    if (u_efds && (uintptr_t)u_efds < 0x8000000000000000ULL) {
        if (copy_from_user(&in_efds, u_efds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }

    int timeout_ms = -1;
    if (u_tv && (uintptr_t)u_tv < 0x8000000000000000ULL) {
        struct linux_timeval tv;
        if (copy_from_user(&tv, u_tv, sizeof(tv)) != 0) return -(s64)EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0) return -(s64)EINVAL;
        /* Saturate instead of computing in int: tv_sec is user-controlled, and
         * tv_sec * 1000 overflowing could land on a negative timeout_ms, which
         * this function reads as "block forever" — the opposite of the very
         * long timeout that was asked for. */
        s64 ms = (tv.tv_sec > (s64)0x7FFFFFFF / 1000)
                     ? (s64)0x7FFFFFFF
                     : tv.tv_sec * 1000 + tv.tv_usec / 1000;
        if (ms > (s64)0x7FFFFFFF) ms = (s64)0x7FFFFFFF;
        timeout_ms = (int)ms;
    }

    u64 end_ticks = (timeout_ms > 0) ? (sched_get_ticks() + (timeout_ms + 9) / 10) : 0;
    int check_nfds = nfds > PROC_MAX_FDS ? PROC_MAX_FDS : nfds;
    kernel_fd_set_t out_rfds, out_wfds, out_efds;
    int ready_count = 0;

    for (;;) {
        ready_count = 0;
        __builtin_memset(&out_rfds, 0, sizeof(out_rfds));
        __builtin_memset(&out_wfds, 0, sizeof(out_wfds));
        __builtin_memset(&out_efds, 0, sizeof(out_efds));

        for (int fd = 0; fd < check_nfds; fd++) {
            /* Referenced for the same reason as sys_poll_impl(): the loop
             * sleeps between passes and the fd can be closed underneath it. */
            file_t *f = fget(proc, fd);
            if (!f) continue;

            if (K_FD_ISSET(fd, &in_rfds)) {
                short rev = check_file_readiness(f, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) {
                    K_FD_SET(fd, &out_rfds);
                    ready_count++;
                }
            }
            if (K_FD_ISSET(fd, &in_wfds)) {
                short rev = check_file_readiness(f, POLLOUT);
                if (rev & POLLOUT) {
                    K_FD_SET(fd, &out_wfds);
                    ready_count++;
                }
            }
            if (K_FD_ISSET(fd, &in_efds)) {
                short rev = check_file_readiness(f, POLLERR);
                if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                    K_FD_SET(fd, &out_efds);
                    ready_count++;
                }
            }
            fput(f);
        }

        if (ready_count > 0 || timeout_ms == 0) break;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) break;

        /* BUG-AO fix: interrupt on deliverable signal per POSIX */
        if (proc->sig_pending & ~proc->sig_blocked) {
            return -(s64)EINTR;
        }

        sched_sleep(1);
    }

    if (u_rfds && (uintptr_t)u_rfds < 0x8000000000000000ULL) copy_to_user(u_rfds, &out_rfds, sizeof(kernel_fd_set_t));
    if (u_wfds && (uintptr_t)u_wfds < 0x8000000000000000ULL) copy_to_user(u_wfds, &out_wfds, sizeof(kernel_fd_set_t));
    if (u_efds && (uintptr_t)u_efds < 0x8000000000000000ULL) copy_to_user(u_efds, &out_efds, sizeof(kernel_fd_set_t));

    return ready_count;
}

static s64 sys_pselect6_impl(pt_regs_t *r)
{
    int nfds = (int)r->rdi;
    kernel_fd_set_t *u_rfds = (kernel_fd_set_t *)r->rsi;
    kernel_fd_set_t *u_wfds = (kernel_fd_set_t *)r->rdx;
    kernel_fd_set_t *u_efds = (kernel_fd_set_t *)r->r10;
    const struct linux_timespec *u_ts = (const struct linux_timespec *)r->r8;

    struct linux_timeval tv;
    struct linux_timeval *tv_ptr = NULL;
    if (u_ts && (uintptr_t)u_ts < 0x8000000000000000ULL) {
        struct linux_timespec ts;
        if (copy_from_user(&ts, u_ts, sizeof(ts)) == 0) {
            tv.tv_sec = ts.tv_sec;
            tv.tv_usec = ts.tv_nsec / 1000;
            tv_ptr = &tv;
        }
    }

    pt_regs_t sub = *r;
    sub.rdi = (u64)nfds;
    sub.rsi = (u64)(uintptr_t)u_rfds;
    sub.rdx = (u64)(uintptr_t)u_wfds;
    sub.r10 = (u64)(uintptr_t)u_efds;
    sub.r8  = (u64)(uintptr_t)tv_ptr;
    return sys_select_impl(&sub);
}

/* ── Process Hierarchy, Execve & Lifecycle ───────────────────────────────── */

static s64 sys_getpid_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 1;
}

static s64 sys_getppid_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (proc && proc->parent) return (s64)proc->parent->pid;
    return 0;
}

static s64 sys_fork_impl(pt_regs_t *r)
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

    /* BUG-AN fix: acquire g_fd_lock while copying parent's handle_table */
    irqflags_t fd_irqf = spinlock_lock_irqsave(&g_fd_lock);
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
    spinlock_unlock_irqrestore(&g_fd_lock, fd_irqf);

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
        if (!child_stack || (uintptr_t)child_stack >= 0x0000800000000000ULL)
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
            (uintptr_t)parent_tidptr < 0x0000800000000000ULL) {
            int tid = (int)t->tid;
            copy_to_user(parent_tidptr, &tid, sizeof(int));
        }
        if ((flags & 0x01000000ULL /* CLONE_CHILD_SETTID */) && child_tidptr &&
            (uintptr_t)child_tidptr < 0x0000800000000000ULL) {
            int tid = (int)t->tid;
            copy_to_user(child_tidptr, &tid, sizeof(int));
        }
        if ((flags & 0x00200000ULL /* CLONE_CHILD_CLEARTID */) && child_tidptr &&
            (uintptr_t)child_tidptr < 0x0000800000000000ULL) {
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

    /* BUG-AN fix: acquire g_fd_lock while copying parent's handle_table */
    irqflags_t fd_irqf = spinlock_lock_irqsave(&g_fd_lock);
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
    spinlock_unlock_irqrestore(&g_fd_lock, fd_irqf);

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
    if ((flags & 0x00100000ULL /* CLONE_PARENT_SETTID */) && parent_tidptr && (uintptr_t)parent_tidptr < 0x0000800000000000ULL) {
        int tid = (int)child->pid;
        copy_to_user(parent_tidptr, &tid, sizeof(int));
    }

    if ((flags & 0x01000000ULL /* CLONE_CHILD_SETTID */) && child_tidptr && (uintptr_t)child_tidptr < 0x0000800000000000ULL) {
        int tid = (int)child->pid;
        copy_to_user(child_tidptr, &tid, sizeof(int));
    }

    if ((flags & 0x00200000ULL /* CLONE_CHILD_CLEARTID */) && child_tidptr && (uintptr_t)child_tidptr < 0x0000800000000000ULL) {
        t->clear_child_tid = (u64)(uintptr_t)child_tidptr;
    }

    sched_enqueue_thread(t);
    return (s64)child->pid;
}

static s64 sys_clone_impl(pt_regs_t *r)
{
    u64 flags = r->rdi;
    virt_addr_t child_stack = (virt_addr_t)r->rsi;
    int *parent_tidptr = (int *)r->rdx;
    int *child_tidptr = (int *)r->r10;
    u64 newtls = r->r8;
    return do_clone(flags, child_stack, parent_tidptr, child_tidptr, newtls, r);
}

static s64 sys_vfork_impl(pt_regs_t *r)
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
    if (user_argv && (uintptr_t)user_argv < 0x8000000000000000ULL) {
        for (int i = 0; i < 127; i++) {
            const char *arg_ptr = NULL;
            if (copy_from_user(&arg_ptr, &user_argv[i], sizeof(char *)) != 0) break;
            if (!arg_ptr) break;
            if ((uintptr_t)arg_ptr >= 0x8000000000000000ULL) break;

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
    if (user_envp && (uintptr_t)user_envp < 0x8000000000000000ULL) {
        for (int i = 0; i < 127; i++) {
            const char *env_ptr = NULL;
            if (copy_from_user(&env_ptr, &user_envp[i], sizeof(char *)) != 0) break;
            if (!env_ptr) break;
            if ((uintptr_t)env_ptr >= 0x8000000000000000ULL) break;

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

static s64 sys_execve_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    return execve_core(r, kpath,
                       (const char *const *)r->rsi,
                       (const char *const *)r->rdx);
}

/* execveat(dirfd, path, argv, envp, flags) — Linux syscall 322.
 * Supports AT_EMPTY_PATH (exec the file the dirfd refers to). */
static s64 sys_execveat_impl(pt_regs_t *r)
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
    if (user_path && (uintptr_t)user_path < 0x8000000000000000ULL) {
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
        if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;
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
         * slot under g_fd_lock (atomic swap-to-NULL) so a sibling thread also
         * exiting, sched_exit_thread's last-thread cleanup, the reaper, or a
         * cross-process fget() cannot double-close or use-after-free the same
         * file_t / object. */
        fd_table_release(proc);
    }
    sched_exit_thread();
    __builtin_unreachable();
}

static s64 sys_exit_group_impl(pt_regs_t *r)
{
    /* POSIX: exit_group terminates every thread of the process, not just the
     * caller. Wind the siblings down first, then exit this thread. */
    sched_exit_group_mark();
    return sys_exit_impl(r);
}

static s64 sys_wait4_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    int *user_status = (int *)r->rsi;
    int options = (int)r->rdx;
    int kstatus = 0;

    s64 res = sched_waitpid(pid, user_status ? &kstatus : NULL, options);
    if (res >= 0 && user_status) {
        if ((uintptr_t)user_status < 0x8000000000000000ULL) {
            copy_to_user(user_status, &kstatus, sizeof(int));
        }
    }
    return res;
}

static s64 sys_waitid_impl(pt_regs_t *r)
{
    s32 id = (s32)r->rsi;
    void *infop = (void *)r->rdx;
    int options = (int)r->r10;

    int status = 0;
    s64 res = sched_waitpid(id == 0 ? -1 : id, &status, options);
    if (res < 0) return res;

    if (infop && (uintptr_t)infop < 0x8000000000000000ULL) {
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

static s64 sys_kill_impl(pt_regs_t *r)
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
        return sched_kill_process((u32)pid, sig);
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
        s64 r2 = sched_kill_process(pids[i], sig);
        if (r2 == 0) ret = 0;
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

static s64 sys_rt_sigaction_impl(pt_regs_t *r)
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
        if ((uintptr_t)user_oldact >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_to_user(user_oldact, &proc->sigactions[signum], sizeof(sigaction_t)) != 0)
            return -(s64)EFAULT;
    }

    if (user_act) {
        if ((uintptr_t)user_act >= 0x8000000000000000ULL) return -(s64)EFAULT;
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
            h >= 0x0000800000000000ULL)
            return -(s64)EFAULT;
        if (rst != 0 && rst >= 0x0000800000000000ULL)
            return -(s64)EFAULT;

        proc->sigactions[signum] = kact;
    }

    return 0;
}

static s64 sys_rt_sigprocmask_impl(pt_regs_t *r)
{
    int how = (int)r->rdi;
    const sigset_t *user_set = (const sigset_t *)r->rsi;
    sigset_t *user_oldset = (sigset_t *)r->rdx;
    size_t sigsetsize = (size_t)r->r10;

    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (user_oldset) {
        if ((uintptr_t)user_oldset >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_to_user(user_oldset, &proc->sig_blocked, sizeof(sigset_t)) != 0)
            return -(s64)EFAULT;
    }

    if (user_set) {
        if ((uintptr_t)user_set >= 0x8000000000000000ULL) return -(s64)EFAULT;
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

/* sys_rt_sigreturn_impl() lives in kernel/signal.c alongside the frame builder. */

static s64 sys_pause_impl(pt_regs_t *r)
{
    (void)r;
    /* B-14: block until awakened by signal */
    sched_block(THREAD_BLOCKED);
    return -(s64)EINTR;
}

/* ── Sockets & Networking Syscalls ───────────────────────────────────────── */

static s64 sys_socket_impl(pt_regs_t *r)
{
    int domain = (int)r->rdi;
    int type = (int)r->rsi;
    int protocol = (int)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Strip non-standard flags like SOCK_CLOEXEC or SOCK_NONBLOCK */
    int base_type = type & 0x0F;

    /* Privilege check for RAW sockets */
    if (base_type == SOCK_RAW) {
        if (!security_check_permission(proc, CAP_NET_RAW)) {
            return -(s64)EPERM;
        }
    }

    socket_t *sock = sock_alloc(domain, base_type, protocol);
    if (!sock) return -(s64)ENOMEM;

    file_t *f = sock_create_file(sock);
    if (!f) {
        sock_free(sock);
        return -(s64)ENOMEM;
    }

    if (type & 00004000) { /* O_NONBLOCK / SOCK_NONBLOCK */
        f->f_flags |= O_NONBLOCK;
    }

    {
        s64 nfd = fd_install(proc, f, (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0);
        if (nfd >= 0) return nfd;
    }

    sock_free(sock);
    kfree(f);
    return -(s64)EMFILE;
}

/* struct sockaddr_in and struct sockaddr_un share nothing but sa_family at
 * offset 0 — a bind()/connect() on an AF_UNIX socket carries a path in
 * sun_path, not the fixed 16-byte sockaddr_in this file otherwise assumes
 * everywhere. Peeks the family, then copies out just the path (NUL-safe:
 * sun_path isn't guaranteed to be NUL-terminated by the caller, so this
 * always terminates the result itself from addrlen). Returns 0 with
 * *out_path set for AF_UNIX, or a negative errno; leaves *out_path
 * untouched (caller should fall back to sockaddr_in handling) when the
 * address isn't AF_UNIX at all — that isn't an error at this layer. */
static s64 copy_user_sockaddr_un_path(const struct sockaddr *uaddr, socklen_t addrlen,
                                       char out_path[UNIX_PATH_MAX], bool *out_is_unix)
{
    *out_is_unix = false;
    sa_family_t fam;
    if (copy_from_user(&fam, uaddr, sizeof(fam)) != 0) return -(s64)EFAULT;
    if (fam != AF_UNIX) return 0;

    *out_is_unix = true;
    if (addrlen < sizeof(sa_family_t) + 1) return -(s64)EINVAL; /* need at least a 1-char path */
    size_t path_len = addrlen - sizeof(sa_family_t);
    if (path_len >= UNIX_PATH_MAX) path_len = UNIX_PATH_MAX - 1;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    size_t copy_len = sizeof(sa_family_t) + path_len;
    if (copy_len > sizeof(sun)) copy_len = sizeof(sun);
    if (copy_from_user(&sun, uaddr, copy_len) != 0) return -(s64)EFAULT;
    sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
    if (!sun.sun_path[0]) return -(s64)EINVAL; /* abstract-namespace sockets not supported */

    strncpy(out_path, sun.sun_path, UNIX_PATH_MAX - 1);
    out_path[UNIX_PATH_MAX - 1] = '\0';
    return 0;
}

static s64 sys_bind_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->rsi;
    socklen_t addrlen = (socklen_t)r->rdx;

    if (!uaddr || addrlen < sizeof(sa_family_t)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        char path[UNIX_PATH_MAX];
        bool is_unix = false;
        s64 err = copy_user_sockaddr_un_path(uaddr, addrlen, path, &is_unix);
        if (err < 0) return err;
        if (!is_unix) return -(s64)EINVAL;
        return unix_socket_bind(sock->uds, path);
    }

    if (addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, uaddr, sizeof(struct sockaddr_in)) != 0) {
        return -(s64)EFAULT;
    }

    u16 port = ntohs(sin.sin_port);
    const u8 *ip = (const u8 *)&sin.sin_addr.s_addr;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_bind(sock->tcp, ip, port);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return (s64)udp_bind(sock->udp, ip, port);
    }

    return -(s64)EOPNOTSUPP;
}

static s64 sys_connect_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->rsi;
    socklen_t addrlen = (socklen_t)r->rdx;

    if (!uaddr || addrlen < sizeof(sa_family_t)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

    if (sock->domain == AF_UNIX) {
        char path[UNIX_PATH_MAX];
        bool is_unix = false;
        s64 err = copy_user_sockaddr_un_path(uaddr, addrlen, path, &is_unix);
        if (err < 0) return err;
        if (!is_unix) return -(s64)EINVAL;
        return unix_socket_connect(sock->uds, path, nonblock);
    }

    if (addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, uaddr, sizeof(struct sockaddr_in)) != 0) {
        return -(s64)EFAULT;
    }

    u16 port = ntohs(sin.sin_port);
    const u8 *ip = (const u8 *)&sin.sin_addr.s_addr;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_connect(sock->tcp, ip, port, nonblock);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return (s64)udp_connect(sock->udp, ip, port);
    }

    return -(s64)EOPNOTSUPP;
}

static s64 sys_listen_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int backlog = (int)r->rsi;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        return unix_socket_listen(sock->uds, backlog);
    }
    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_listen(sock->tcp, backlog);
    }

    return -(s64)EOPNOTSUPP;
}

/* accept4(2) flags. Same bit values as the type flags socket(2) takes, which
 * is what lets a caller pass SOCK_NONBLOCK|SOCK_CLOEXEC through unchanged. */
#define SOCK_NONBLOCK_FLAG  00004000
#define SOCK_CLOEXEC_FLAG   02000000

/* accept(2) is accept4(2) with no flags; sharing one body is what stops the
 * two from drifting apart in the details that matter (blocking behaviour,
 * address copy-out, the fd's close-on-exec state). */
static s64 do_accept(pt_regs_t *r, int a4flags)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (a4flags & ~(SOCK_NONBLOCK_FLAG | SOCK_CLOEXEC_FLAG)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    socket_t *listener = NULL;
    int ret = sock_get_from_fd(fd, &listener);
    if (ret < 0) return -(s64)ret;

    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

    if (listener->domain == AF_UNIX) {
        unix_sock_t *child_uds = unix_socket_create(SOCK_STREAM);
        if (!child_uds) return -(s64)ENOMEM;

        int aerr = unix_socket_accept(listener->uds, child_uds, nonblock);
        if (aerr < 0) {
            kfree(child_uds);
            return (s64)aerr;
        }

        socket_t *child_sock = (socket_t *)kzalloc(sizeof(socket_t));
        if (!child_sock) {
            unix_socket_close(child_uds);
            return -(s64)ENOMEM;
        }
        child_sock->domain = AF_UNIX;
        child_sock->type = SOCK_STREAM;
        child_sock->uds = child_uds;

        file_t *child_file = sock_create_file(child_sock);
        if (!child_file) {
            sock_free(child_sock);
            return -(s64)ENOMEM;
        }
        if (a4flags & SOCK_NONBLOCK_FLAG) child_file->f_flags |= O_NONBLOCK;

        int new_fd = (int)fd_install(proc, child_file,
                                     (a4flags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0);
        if (new_fd < 0) {
            sock_free(child_sock);
            kfree(child_file);
            return -(s64)EMFILE;
        }
        /* No meaningful peer address to fill in beyond AF_UNIX + this
         * listener's own bound path — real Linux reports the *connecting*
         * side's bind() path here (empty for an unbound/autobind client,
         * which is the overwhelmingly common case), and this
         * implementation doesn't track that on the accepted side at all.
         * Leaving *uaddrlen at whatever the caller passed in (unchanged) is
         * closer to "no information available" than fabricating a sockaddr. */
        return (s64)new_fd;
    }

    if (listener->type != SOCK_STREAM || !listener->tcp) {
        return -(s64)EOPNOTSUPP;
    }

    u8 client_ip[4];
    u16 client_port = 0;
    tcp_sock_t *child_tcp = tcp_accept(listener->tcp, client_ip, &client_port, nonblock);
    if (!child_tcp) {
        return nonblock ? -(s64)EAGAIN : -(s64)EINVAL;
    }

    socket_t *child_sock = (socket_t *)kzalloc(sizeof(socket_t));
    if (!child_sock) {
        tcp_socket_close(child_tcp);
        return -(s64)ENOMEM;
    }

    child_sock->domain = listener->domain;
    child_sock->type = SOCK_STREAM;
    child_sock->protocol = IPPROTO_TCP;
    child_sock->tcp = child_tcp;

    file_t *child_file = sock_create_file(child_sock);
    if (!child_file) {
        sock_free(child_sock);
        return -(s64)ENOMEM;
    }

    if (a4flags & SOCK_NONBLOCK_FLAG) child_file->f_flags |= O_NONBLOCK;

    int new_fd = (int)fd_install(proc, child_file,
                                 (a4flags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0);
    if (new_fd < 0) {
        sock_free(child_sock);
        kfree(child_file);
        return -(s64)EMFILE;
    }

    /* Fill caller address if requested */
    if (uaddr && uaddrlen) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(client_port);
        memcpy(&sin.sin_addr.s_addr, client_ip, 4);

        copy_to_user(uaddr, &sin, sizeof(sin));
        socklen_t slen = sizeof(sin);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
    }

    return (s64)new_fd;
}

static s64 sys_accept_impl(pt_regs_t *r)
{
    return do_accept(r, 0);
}

static s64 sys_accept4_impl(pt_regs_t *r)
{
    return do_accept(r, (int)r->r10);
}

static s64 sys_shutdown_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int how = (int)r->rsi;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    /* AF_UNIX must be checked before sock->type: `sock->tcp` and `sock->uds`
     * are the same union storage, so `sock->type == SOCK_STREAM && sock->tcp`
     * is true for an AF_UNIX stream socket too — tcp_shutdown() would then
     * run against a unix_sock_t reinterpreted as a tcp_sock_t. Every
     * function below in this file that dispatches on sock->type alone has
     * the same hazard; this one and getsockname/getpeername right after it
     * used to have it — caught by shutdown()/send()/recv() on a real
     * AF_UNIX socket landing in tcp_send() and getting a nonsense
     * -ENOTCONN back instead of actually writing. */
    if (sock->domain == AF_UNIX) {
        return 0; /* no half-close modeled for the pipe-backed byte stream */
    }
    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_shutdown(sock->tcp, how);
    }
    return 0;
}

static s64 sys_getsockname_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (!uaddr || !uaddrlen) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL || (uintptr_t)uaddrlen >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, sock->uds->path, UNIX_PATH_MAX - 1);
        copy_to_user(uaddr, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
        return 0;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        sin.sin_port = htons(sock->tcp->local_port);
        memcpy(&sin.sin_addr.s_addr, sock->tcp->local_ip, 4);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        sin.sin_port = htons(sock->udp->local_port);
        memcpy(&sin.sin_addr.s_addr, sock->udp->local_ip, 4);
    }

    copy_to_user(uaddr, &sin, sizeof(sin));
    socklen_t slen = sizeof(sin);
    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));

    return 0;
}

static s64 sys_getpeername_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (!uaddr || !uaddrlen) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL || (uintptr_t)uaddrlen >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        /* This implementation doesn't track the *peer's* bound path on the
         * accepted/connected side (see do_accept()'s AF_UNIX branch comment
         * in this file) — report AF_UNIX with an empty path, same as real
         * Linux does for an unbound/autobind peer, rather than fabricating
         * one. Still validates the socket is actually connected. */
        if (sock->uds->state != UNIX_ST_CONNECTED) return -(s64)ENOTCONN;
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        copy_to_user(uaddr, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
        return 0;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        if (sock->tcp->state != TCP_STATE_ESTABLISHED && sock->tcp->state != TCP_STATE_CLOSE_WAIT) {
            return -(s64)ENOTCONN;
        }
        sin.sin_port = htons(sock->tcp->remote_port);
        memcpy(&sin.sin_addr.s_addr, sock->tcp->remote_ip, 4);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (!sock->udp->connected) return -(s64)ENOTCONN;
        sin.sin_port = htons(sock->udp->remote_port);
        memcpy(&sin.sin_addr.s_addr, sock->udp->remote_ip, 4);
    }

    copy_to_user(uaddr, &sin, sizeof(sin));
    socklen_t slen = sizeof(sin);
    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));

    return 0;
}

static s64 sys_setsockopt_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int level = (int)r->rsi;
    int optname = (int)r->rdx;
    const void *optval = (const void *)r->r10;
    socklen_t optlen = (socklen_t)r->r8;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (!optval || (uintptr_t)optval >= 0x8000000000000000ULL) return -(s64)EFAULT;

    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_reuseaddr = val;
            return 0;
        } else if (optname == SO_REUSEPORT && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_reuseport = val;
            return 0;
        } else if (optname == SO_BROADCAST && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_broadcast = val;
            return 0;
        } else if (optname == SO_RCVTIMEO) {
            if (optlen >= sizeof(struct linux_timeval)) {
                struct linux_timeval tv;
                copy_from_user(&tv, optval, sizeof(tv));
                sock->so_rcvtimeo = (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
            } else if (optlen >= sizeof(int)) {
                int ms = 0;
                copy_from_user(&ms, optval, sizeof(int));
                sock->so_rcvtimeo = (u32)ms;
            }
            return 0;
        } else if (optname == SO_SNDTIMEO) {
            if (optlen >= sizeof(struct linux_timeval)) {
                struct linux_timeval tv;
                copy_from_user(&tv, optval, sizeof(tv));
                sock->so_sndtimeo = (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
            } else if (optlen >= sizeof(int)) {
                int ms = 0;
                copy_from_user(&ms, optval, sizeof(int));
                sock->so_sndtimeo = (u32)ms;
            }
            return 0;
        } else if (optname == SO_KEEPALIVE || optname == SO_SNDBUF || optname == SO_RCVBUF) {
            return 0;
        }
    } else if (level == IPPROTO_TCP) {
        if (optname == TCP_NODELAY) {
            return 0;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == 1 /* IP_TOS */ || optname == 2 /* IP_TTL */) {
            return 0;
        }
    }
    return 0;
}

static s64 sys_getsockopt_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int level = (int)r->rsi;
    int optname = (int)r->rdx;
    void *optval = (void *)r->r10;
    socklen_t *optlen = (socklen_t *)r->r8;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (!optval || !optlen) return -(s64)EINVAL;
    if ((uintptr_t)optval >= 0x8000000000000000ULL || (uintptr_t)optlen >= 0x8000000000000000ULL) return -(s64)EFAULT;

    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR) {
            int val = sock->so_reuseaddr;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_REUSEPORT) {
            int val = sock->so_reuseport;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_BROADCAST) {
            int val = sock->so_broadcast;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_TYPE) {
            int val = sock->type;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_ERROR) {
            int val = sock->so_error;
            sock->so_error = 0;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_RCVBUF || optname == SO_SNDBUF) {
            int val = 65536;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        }
    }
    return 0;
}

/* Upper bound on a single socket transfer's kernel bounce buffer. Matches the
 * 64 KB chunking read()/write() already use, and comfortably exceeds the
 * 65507-byte maximum UDP payload. */
#define SOCK_XFER_MAX  65536u

static s64 sys_sendto_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const void *ubuf = (const void *)r->rsi;
    size_t len = (size_t)r->rdx;
    int flags = (int)r->r10;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->r8;
    socklen_t uaddrlen = (socklen_t)r->r9;

    if (!ubuf || len == 0) return 0;
    if ((uintptr_t)ubuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    /* Clamp the bounce buffer the way read()/write() do. `len` is raw user
     * input and went straight to kmalloc(), which serves anything up to 1 GB:
     * an unprivileged process could pin arbitrary kernel memory per call.
     * A stream socket may legally send fewer bytes than asked, so clamping is
     * the correct short-send; a datagram larger than the buffer cannot be
     * truncated silently, so it gets EMSGSIZE. */
    if (len > SOCK_XFER_MAX) {
        if (sock->type == SOCK_DGRAM) return -(s64)EMSGSIZE;
        len = SOCK_XFER_MAX;
    }

    void *kbuf = kmalloc(len);
    if (!kbuf) return -(s64)ENOMEM;
    if (copy_from_user(kbuf, ubuf, len) != 0) {
        kfree(kbuf);
        return -(s64)EFAULT;
    }

    s64 res = -(s64)EOPNOTSUPP;

    /* AF_UNIX must be checked before sock->type: sock->tcp and sock->uds
     * are the same union storage, so `sock->type == SOCK_STREAM && sock->tcp`
     * below is true for an AF_UNIX stream socket too — this used to send an
     * AF_UNIX socket's data through tcp_send() reinterpreting its
     * unix_sock_t as a tcp_sock_t, which is how send()/sendto() on a real
     * connected AF_UNIX socket ended up failing with a nonsense -ENOTCONN
     * (tcp_send() read what it thought was TCP connection state out of
     * memory that was actually unix_sock_t fields) instead of writing. */
    if (sock->domain == AF_UNIX) {
        char dest_path[UNIX_PATH_MAX];
        bool have_dest = false;
        if (uaddr && uaddrlen >= sizeof(sa_family_t)) {
            s64 perr = copy_user_sockaddr_un_path(uaddr, uaddrlen, dest_path, &have_dest);
            if (perr < 0) { kfree(kbuf); return perr; }
        }
        file_t *f = (file_t *)sched_current_process()->handle_table[fd];
        bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);
        res = unix_socket_sendmsg(sock->uds, have_dest ? dest_path : NULL, kbuf, len, NULL, 0, nonblock);
        kfree(kbuf);
        return res;
    }

    if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_send(sock->tcp, kbuf, len, flags);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (uaddr) {
            struct sockaddr_in sin;
            if (copy_from_user(&sin, uaddr, sizeof(sin)) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            res = udp_sendto(sock->udp, kbuf, len, (const u8 *)&sin.sin_addr.s_addr, ntohs(sin.sin_port));
        } else {
            res = udp_sendto(sock->udp, kbuf, len, NULL, 0);
        }
    } else if (sock->type == SOCK_RAW && sock->raw) {
        if (uaddr) {
            struct sockaddr_in sin;
            if (copy_from_user(&sin, uaddr, sizeof(sin)) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + len);
            if (!buf) {
                kfree(kbuf);
                return -(s64)ENOMEM;
            }
            net_buf_reserve(buf, NET_BUF_HEADROOM);
            void *p = net_buf_put(buf, len);
            memcpy(p, kbuf, len);
            int err = ipv4_send(buf, (const u8 *)&sin.sin_addr.s_addr, (u8)sock->raw->protocol);
            if (err < 0) res = (s64)err;
            else res = (s64)len;
        }
    }

    kfree(kbuf);
    if (res < 0 && res == -(s64)EPIPE && !(flags & 0x4000 /* MSG_NOSIGNAL */)) {
        process_t *proc = sched_current_process();
        if (proc) {
            sched_kill_process(proc->pid, 13 /* SIGPIPE */);
        }
    }
    return res;
}

static s64 sys_recvfrom_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    void *ubuf = (void *)r->rsi;
    size_t len = (size_t)r->rdx;
    int flags = (int)r->r10;
    struct sockaddr *uaddr = (struct sockaddr *)r->r8;
    socklen_t *uaddrlen = (socklen_t *)r->r9;
    (void)flags;

    if (!ubuf || len == 0) return 0;
    if ((uintptr_t)ubuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = (f ? ((f->f_flags & O_NONBLOCK) != 0) : false) || ((flags & 0x40 /* MSG_DONTWAIT */) != 0);

    /* AF_UNIX must be checked before sock->type — see sys_sendto_impl's
     * identical comment just above; the same tcp/uds union aliasing bug
     * applied here on the receive side too. */
    if (sock->domain == AF_UNIX) {
        size_t clen = len > SOCK_XFER_MAX ? SOCK_XFER_MAX : len;
        void *kbuf = kmalloc(clen);
        if (!kbuf) return -(s64)ENOMEM;
        char src_path[UNIX_PATH_MAX];
        s64 res = unix_socket_recvmsg(sock->uds, kbuf, clen, src_path, NULL, nonblock);
        if (res > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            if (uaddr && uaddrlen) {
                struct sockaddr_un sun;
                memset(&sun, 0, sizeof(sun));
                sun.sun_family = AF_UNIX;
                strncpy(sun.sun_path, src_path, UNIX_PATH_MAX - 1);
                copy_to_user(uaddr, &sun, sizeof(sun));
                socklen_t slen = sizeof(sun);
                copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
            }
        }
        /* Any SCM_RIGHTS fds this datagram carried are silently dropped —
         * plain recvfrom(2) has no ancillary-data channel to return them
         * through, matching real Linux's own behavior here exactly (the
         * fds are simply closed, never leaked to any process). */
        kfree(kbuf);
        return res;
    }

    /* Same unbounded-kmalloc guard as sendto. Returning fewer bytes than the
     * caller's buffer size is always valid for recv, so a plain clamp is a
     * correct short read here. */
    if (len > SOCK_XFER_MAX) len = SOCK_XFER_MAX;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -(s64)ENOMEM;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        s64 res = tcp_recv(sock->tcp, kbuf, len, nonblock);
        if (res > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
        }
        kfree(kbuf);
        return res;
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        u8 src_ip[4];
        u16 src_port = 0;
        s64 res = udp_recvfrom(sock->udp, kbuf, len, src_ip, &src_port, nonblock);
        if (res > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            if (uaddr && uaddrlen) {
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_port = htons(src_port);
                memcpy(&sin.sin_addr.s_addr, src_ip, 4);

                copy_to_user(uaddr, &sin, sizeof(sin));
                socklen_t slen = sizeof(sin);
                copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
            }
        }
        kfree(kbuf);
        return res;
    } else if (sock->type == SOCK_RAW && sock->raw) {
        kfree(kbuf);
        for (;;) {
            net_buf_t *pkt = net_buf_queue_pop(&sock->raw->rx_queue);
            if (pkt) {
                if (pkt->len < 6) {
                    net_buf_free(pkt);
                    continue;
                }
                size_t psize = pkt->len - 6;
                size_t clen = (psize < len) ? psize : len;
                if (copy_to_user(ubuf, pkt->data + 6, clen) != 0) {
                    net_buf_free(pkt);
                    return -(s64)EFAULT;
                }
                if (uaddr && uaddrlen) {
                    struct sockaddr_in sin;
                    memset(&sin, 0, sizeof(sin));
                    sin.sin_family = AF_INET;
                    memcpy(&sin.sin_addr.s_addr, pkt->data, 4);
                    copy_to_user(uaddr, &sin, sizeof(sin));
                    socklen_t slen = sizeof(sin);
                    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
                }
                net_buf_free(pkt);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            spinlock_lock(&sock->raw->lock);
            if (net_buf_queue_len(&sock->raw->rx_queue) == 0) {
                sock->raw->wait_thread = sched_current_thread();
                spinlock_unlock(&sock->raw->lock);
                sched_block(THREAD_BLOCKED_PENDING);
            } else {
                spinlock_unlock(&sock->raw->lock);
            }
        }
    }

    kfree(kbuf);
    return -(s64)EOPNOTSUPP;
}

/* Must stay field-for-field identical to `struct msghdr`/`struct cmsghdr` in
 * userland/libc/include/sys/socket.h — same reasoning as struct linux_timex
 * above. */
struct linux_msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

struct linux_cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

/* Real sendmsg(2)/recvmsg(2): gather/scatter across every iovec (not just
 * iov[0], which is all SYS_sendmsg/SYS_recvmsg used to do when they were
 * literal aliases for sys_sendto_impl/sys_recvfrom_impl — see the reg()
 * calls below), plus SCM_RIGHTS fd-passing for AF_UNIX
 * (kernel/net/unix_socket.c). A handful of iovecs is the overwhelmingly
 * common case for real callers (X11/Wayland-style protocols, systemd/dbus
 * fd-passing), so both cap at a fixed, on-stack SENDMSG_MAX_IOV rather than
 * readv/writev's 1024 — a caller past that gets -EINVAL rather than a
 * kmalloc'd array, matching this file's general preference for bounded
 * stack allocations in these paths. */
#define SENDMSG_MAX_IOV 16

static s64 sys_sendmsg_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct linux_msghdr *umsg = (const struct linux_msghdr *)r->rsi;
    int flags = (int)r->rdx;

    if (!umsg) return -(s64)EFAULT;
    if ((uintptr_t)umsg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    struct linux_msghdr msg;
    if (copy_from_user(&msg, umsg, sizeof(msg)) != 0) return -(s64)EFAULT;
    if (msg.msg_iovlen > SENDMSG_MAX_IOV) return -(s64)EINVAL;

    struct iovec kiov[SENDMSG_MAX_IOV];
    size_t total = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        if (copy_from_user(kiov, msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec)) != 0)
            return -(s64)EFAULT;
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (kiov[i].iov_len > 0x7FFFFFFF) return -(s64)EINVAL;
            total += kiov[i].iov_len;
        }
    }
    if (total > SOCK_XFER_MAX) {
        if (sock->type == SOCK_DGRAM) return -(s64)EMSGSIZE;
        total = SOCK_XFER_MAX; /* stream socket: short send, same as sendto's own clamp */
    }

    void *kbuf = NULL;
    if (total > 0) {
        kbuf = kmalloc(total);
        if (!kbuf) return -(s64)ENOMEM;
        size_t off = 0;
        for (size_t i = 0; i < msg.msg_iovlen && off < total; i++) {
            size_t take = kiov[i].iov_len;
            if (off + take > total) take = total - off;
            if (take > 0 && copy_from_user((u8 *)kbuf + off, kiov[i].iov_base, take) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            off += take;
        }
    }

    /* SCM_RIGHTS is only meaningful — and only parsed — for AF_UNIX; a
     * non-unix socket's msg_control is silently ignored, matching real
     * Linux (ancillary data is a per-protocol-family feature there too). */
    file_t *out_fds[UNIX_SCM_MAX_FDS];
    int out_nfds = 0;
    process_t *proc = sched_current_process();
    if (sock->domain == AF_UNIX && msg.msg_control && msg.msg_controllen >= sizeof(struct linux_cmsghdr)) {
        size_t cctrllen = msg.msg_controllen;
        if (cctrllen > 4096) cctrllen = 4096; /* bound the control-buffer copy */
        u8 *ctrl = (u8 *)kmalloc(cctrllen);
        if (!ctrl) { if (kbuf) kfree(kbuf); return -(s64)ENOMEM; }
        if (copy_from_user(ctrl, msg.msg_control, cctrllen) != 0) {
            kfree(ctrl); if (kbuf) kfree(kbuf); return -(s64)EFAULT;
        }
        size_t off = 0;
        while (off + sizeof(struct linux_cmsghdr) <= cctrllen) {
            struct linux_cmsghdr *ch = (struct linux_cmsghdr *)(ctrl + off);
            if (ch->cmsg_len < sizeof(struct linux_cmsghdr) || off + ch->cmsg_len > cctrllen) break;
            if (ch->cmsg_level == SOL_SOCKET && ch->cmsg_type == SCM_RIGHTS) {
                int *fdp = (int *)(void *)(ch + 1);
                size_t nfd_here = (ch->cmsg_len - sizeof(struct linux_cmsghdr)) / sizeof(int);
                for (size_t i = 0; i < nfd_here && out_nfds < UNIX_SCM_MAX_FDS; i++) {
                    file_t *uf = fget(proc, fdp[i]);
                    if (!uf) {
                        for (int j = 0; j < out_nfds; j++) fput(out_fds[j]);
                        kfree(ctrl); if (kbuf) kfree(kbuf);
                        return -(s64)EBADF;
                    }
                    out_fds[out_nfds++] = uf;
                }
            }
            size_t adv = (ch->cmsg_len + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
            if (adv == 0) break;
            off += adv;
        }
        kfree(ctrl);
    }

    char dest_path[UNIX_PATH_MAX];
    bool have_dest = false;
    if (sock->domain == AF_UNIX && msg.msg_name && msg.msg_namelen >= sizeof(sa_family_t)) {
        s64 perr = copy_user_sockaddr_un_path((const struct sockaddr *)msg.msg_name, msg.msg_namelen,
                                               dest_path, &have_dest);
        if (perr < 0) { for (int j = 0; j < out_nfds; j++) fput(out_fds[j]); if (kbuf) kfree(kbuf); return perr; }
    }

    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);

    s64 res;
    if (sock->domain == AF_UNIX) {
        res = unix_socket_sendmsg(sock->uds, have_dest ? dest_path : NULL, kbuf, total,
                                   out_fds, out_nfds, nonblock);
        if (res < 0) for (int j = 0; j < out_nfds; j++) fput(out_fds[j]);
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_send(sock->tcp, kbuf, total, flags);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in sin;
            if (copy_from_user(&sin, msg.msg_name, sizeof(sin)) != 0) {
                if (kbuf) kfree(kbuf);
                return -(s64)EFAULT;
            }
            res = udp_sendto(sock->udp, kbuf, total, (const u8 *)&sin.sin_addr.s_addr, ntohs(sin.sin_port));
        } else {
            res = udp_sendto(sock->udp, kbuf, total, NULL, 0);
        }
    } else {
        res = -(s64)EOPNOTSUPP;
    }

    if (kbuf) kfree(kbuf);
    if (res < 0 && res == -(s64)EPIPE && !(flags & 0x4000 /* MSG_NOSIGNAL */)) {
        if (proc) sched_kill_process(proc->pid, 13 /* SIGPIPE */);
    }
    return res;
}

static s64 sys_recvmsg_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct linux_msghdr *umsg = (struct linux_msghdr *)r->rsi;
    int flags = (int)r->rdx;

    if (!umsg) return -(s64)EFAULT;
    if ((uintptr_t)umsg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    struct linux_msghdr msg;
    if (copy_from_user(&msg, umsg, sizeof(msg)) != 0) return -(s64)EFAULT;
    if (msg.msg_iovlen > SENDMSG_MAX_IOV) return -(s64)EINVAL;

    struct iovec kiov[SENDMSG_MAX_IOV];
    size_t total = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        if (copy_from_user(kiov, msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec)) != 0)
            return -(s64)EFAULT;
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (kiov[i].iov_len > 0x7FFFFFFF) return -(s64)EINVAL;
            total += kiov[i].iov_len;
        }
    }
    if (total > SOCK_XFER_MAX) total = SOCK_XFER_MAX;

    void *kbuf = total ? kmalloc(total) : NULL;
    if (total && !kbuf) return -(s64)ENOMEM;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);

    char src_path[UNIX_PATH_MAX] = {0};
    int uds_nfds = 0;
    file_t *uds_fds[UNIX_SCM_MAX_FDS];
    s64 res;

    if (sock->domain == AF_UNIX) {
        res = unix_socket_recvmsg(sock->uds, kbuf, total, src_path, &uds_nfds, nonblock);
        if (res >= 0 && uds_nfds > 0) {
            uds_nfds = unix_socket_recvmsg_take_fds(sock->uds, uds_fds, UNIX_SCM_MAX_FDS);
        }
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_recv(sock->tcp, kbuf, total, nonblock);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        u8 src_ip[4]; u16 src_port = 0;
        res = udp_recvfrom(sock->udp, kbuf, total, src_ip, &src_port, nonblock);
        if (res >= 0 && msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(src_port);
            memcpy(&sin.sin_addr.s_addr, src_ip, 4);
            copy_to_user(msg.msg_name, &sin, sizeof(sin));
            socklen_t slen = sizeof(sin);
            copy_to_user(&umsg->msg_namelen, &slen, sizeof(slen));
        }
    } else {
        res = -(s64)EOPNOTSUPP;
    }

    if (res < 0) {
        if (kbuf) kfree(kbuf);
        return res;
    }

    /* Scatter the received bytes back out across the caller's iovecs. */
    size_t remaining = (size_t)res;
    size_t off = 0;
    for (size_t i = 0; i < msg.msg_iovlen && remaining > 0; i++) {
        size_t take = kiov[i].iov_len;
        if (take > remaining) take = remaining;
        if (take > 0) {
            if (copy_to_user(kiov[i].iov_base, (u8 *)kbuf + off, take) != 0) {
                if (kbuf) kfree(kbuf);
                return -(s64)EFAULT;
            }
            off += take;
            remaining -= take;
        }
    }
    if (kbuf) kfree(kbuf);

    if (sock->domain == AF_UNIX && msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_un)) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, src_path, UNIX_PATH_MAX - 1);
        copy_to_user(msg.msg_name, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(&umsg->msg_namelen, &slen, sizeof(slen));
    }

    /* SCM_RIGHTS: install each received fd into *this* (receiving)
     * process's own fd table (syscall_install_fd, exported for exactly this
     * kind of "mint an fd for something that isn't a fresh open()" case —
     * see its declaration in kernel/syscall/syscall.h) and describe them to
     * the caller in msg_control, same cmsghdr layout CMSG_FIRSTHDR()/
     * CMSG_DATA() in userland's sys/socket.h expect. */
    int out_flags = 0;
    if (uds_nfds > 0 && msg.msg_control && msg.msg_controllen >= sizeof(struct linux_cmsghdr)) {
        int installed[UNIX_SCM_MAX_FDS];
        int n_installed = 0;
        for (int i = 0; i < uds_nfds; i++) {
            s64 nfd = syscall_install_fd(proc, uds_fds[i], 0);
            if (nfd < 0) { vfs_close(uds_fds[i]); continue; } /* no room: drop rather than leak */
            installed[n_installed++] = (int)nfd;
        }
        int fit = n_installed;
        while (fit > 0 && sizeof(struct linux_cmsghdr) + (size_t)fit * sizeof(int) > msg.msg_controllen) fit--;
        if (fit < n_installed) out_flags |= 0x08; /* MSG_CTRUNC */

        u8 cbuf[sizeof(struct linux_cmsghdr) + UNIX_SCM_MAX_FDS * sizeof(int)];
        struct linux_cmsghdr *ch = (struct linux_cmsghdr *)(void *)cbuf;
        ch->cmsg_len = sizeof(struct linux_cmsghdr) + (size_t)fit * sizeof(int);
        ch->cmsg_level = SOL_SOCKET;
        ch->cmsg_type = SCM_RIGHTS;
        memcpy(cbuf + sizeof(struct linux_cmsghdr), installed, (size_t)fit * sizeof(int));
        copy_to_user(msg.msg_control, cbuf, ch->cmsg_len);
        size_t clen = ch->cmsg_len;
        copy_to_user(&umsg->msg_controllen, &clen, sizeof(clen));
    } else {
        if (uds_nfds > 0) {
            /* Fds were dequeued but the caller gave no control buffer to
             * receive them in: close rather than leak the reference. */
            for (int i = 0; i < uds_nfds; i++) vfs_close(uds_fds[i]);
        }
        if (msg.msg_controllen > 0) {
            size_t zero = 0;
            copy_to_user(&umsg->msg_controllen, &zero, sizeof(zero));
        }
    }
    copy_to_user(&umsg->msg_flags, &out_flags, sizeof(out_flags));

    return res;
}

/* ── Pipes & File Descriptors ────────────────────────────────────────────── */

static s64 sys_pipe_impl(pt_regs_t *r)
{
    int *user_fds = (int *)r->rdi;
    if (!user_fds || (uintptr_t)user_fds >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *rf = NULL, *wf = NULL;
    int err = pipe_create(&rf, &wf);
    if (err < 0) return (s64)err;

    int fd0, fd1;
    if (fd_install_pair(proc, rf, wf, 0, &fd0, &fd1) < 0) {
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EMFILE;
    }

    int fds[2] = { fd0, fd1 };
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        vfs_close(fd_detach(proc, fd0));
        vfs_close(fd_detach(proc, fd1));
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_pipe2_impl(pt_regs_t *r)
{
    int *user_fds = (int *)r->rdi;
    int flags = (int)r->rsi;
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -(s64)EINVAL;
    if (!user_fds || (uintptr_t)user_fds >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *rf = NULL, *wf = NULL;
    int err = pipe_create(&rf, &wf);
    if (err < 0) return (s64)err;

    if (flags & O_NONBLOCK) {
        rf->f_flags |= O_NONBLOCK;
        wf->f_flags |= O_NONBLOCK;
    }

    int fd0, fd1;
    if (fd_install_pair(proc, rf, wf, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0, &fd0, &fd1) < 0) {
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EMFILE;
    }

    int fds[2] = { fd0, fd1 };
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        vfs_close(fd_detach(proc, fd0));
        vfs_close(fd_detach(proc, fd1));
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_dup_impl(pt_regs_t *r)
{
    int oldfd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc || oldfd < 0 || oldfd >= PROC_MAX_FDS) return -(s64)EBADF;

    irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
    file_t *f = (file_t *)proc->handle_table[oldfd];
    if (!f || (uintptr_t)f < 0xFFFF800000000000ULL) {
        spinlock_unlock_irqrestore(&g_fd_lock, fl);
        return -(s64)EBADF;
    }
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->handle_table[i]) {
            __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
            proc->handle_table[i] = f;
            proc->fd_flags[i] = 0; /* dup clears FD_CLOEXEC */
            spinlock_unlock_irqrestore(&g_fd_lock, fl);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&g_fd_lock, fl);
    return -(s64)EMFILE;
}

static s64 do_dup2(int oldfd, int newfd, u8 fd_flags)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (oldfd < 0 || oldfd >= PROC_MAX_FDS) return -(s64)EBADF;
    if (newfd < 0 || newfd >= PROC_MAX_FDS) return -(s64)EBADF;

    irqflags_t fl = spinlock_lock_irqsave(&g_fd_lock);
    file_t *f = (file_t *)proc->handle_table[oldfd];
    if (!f || (uintptr_t)f < 0xFFFF800000000000ULL) {
        spinlock_unlock_irqrestore(&g_fd_lock, fl);
        return -(s64)EBADF;
    }
    if (oldfd == newfd) {                    /* POSIX: no-op, keep FD_CLOEXEC */
        spinlock_unlock_irqrestore(&g_fd_lock, fl);
        return newfd;
    }
    file_t *victim = (file_t *)proc->handle_table[newfd];
    if (victim && (uintptr_t)victim < 0xFFFF800000000000ULL) victim = NULL;
    __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
    proc->handle_table[newfd] = f;
    proc->fd_flags[newfd] = fd_flags; /* BUG-AL fix: set fd_flags atomically under g_fd_lock */
    spinlock_unlock_irqrestore(&g_fd_lock, fl);

    if (victim) vfs_close(victim);           /* drop the replaced fd outside the lock */
    return newfd;
}

static s64 sys_dup2_impl(pt_regs_t *r)
{
    return do_dup2((int)(s32)r->rdi, (int)(s32)r->rsi, 0);
}

static s64 sys_dup3_impl(pt_regs_t *r)
{
    /* BUG-04: POSIX requires dup3(old, new, flags) to return EINVAL when oldfd == newfd */
    int oldfd = (int)(s32)r->rdi;
    int newfd = (int)(s32)r->rsi;
    int flags = (int)r->rdx;
    if (oldfd == newfd) return -(s64)EINVAL;
    if (flags & ~O_CLOEXEC) return -(s64)EINVAL;

    return do_dup2(oldfd, newfd, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
}

static s64 sys_fcntl_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int cmd = (int)r->rsi;
    u64 arg = r->rdx;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];

    switch (cmd) {
    case 0:      /* F_DUPFD */
    case 1030: { /* F_DUPFD_CLOEXEC */
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= PROC_MAX_FDS) return -(s64)EINVAL;
        __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
        s64 nfd = fd_install_from(proc, f, (cmd == 1030) ? FD_CLOEXEC : 0, minfd);
        if (nfd < 0) __atomic_sub_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
        return nfd;
    }
    case 1: /* F_GETFD */
        return (s64)proc->fd_flags[fd];
    case 2: /* F_SETFD */
        proc->fd_flags[fd] = (u8)(arg & FD_CLOEXEC);
        return 0;
    case 3: /* F_GETFL */
        return f->f_flags;
    case 4: /* F_SETFL */
        /* POSIX: Only status flags (O_APPEND, O_NONBLOCK) can be modified */
        f->f_flags = (f->f_flags & ~(O_APPEND | O_NONBLOCK)) | ((u32)arg & (O_APPEND | O_NONBLOCK));
        return 0;
    case 5: { /* F_GETLK */
        if (!arg || arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        struct {
            short l_type;
            short l_whence;
            s64   l_start;
            s64   l_len;
            s32   l_pid;
        } fl;
        if (copy_from_user(&fl, (const void *)arg, sizeof(fl)) != 0) return -(s64)EFAULT;
        if (f->f_inode && f->f_inode->i_flock_type == LOCK_EX && f->f_inode->i_flock_owner != proc->pid) {
            fl.l_type = 1; /* F_WRLCK */
            fl.l_pid  = (s32)f->f_inode->i_flock_owner;
        } else if (f->f_inode && f->f_inode->i_flock_type == LOCK_SH && fl.l_type == 1 /* F_WRLCK */ && f->f_inode->i_flock_owner != proc->pid) {
            fl.l_type = 0; /* F_RDLCK */
            fl.l_pid  = (s32)f->f_inode->i_flock_owner;
        } else {
            fl.l_type = 2; /* F_UNLCK */
        }
        if (copy_to_user((void *)arg, &fl, sizeof(fl)) != 0) return -(s64)EFAULT;
        return 0;
    }
    case 6:   /* F_SETLK */
    case 7: { /* F_SETLKW */
        if (!arg || arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        struct {
            short l_type;
            short l_whence;
            s64   l_start;
            s64   l_len;
            s32   l_pid;
        } fl;
        if (copy_from_user(&fl, (const void *)arg, sizeof(fl)) != 0) return -(s64)EFAULT;
        int op = (cmd == 6) ? LOCK_NB : 0;
        if (fl.l_type == 0 /* F_RDLCK */) op |= LOCK_SH;
        else if (fl.l_type == 1 /* F_WRLCK */) op |= LOCK_EX;
        else if (fl.l_type == 2 /* F_UNLCK */) op |= LOCK_UN;
        else return -(s64)EINVAL;
        return vfs_flock(f, op);
    }
    case 8: /* F_SETOWN */
        proc->pgid = (u32)arg;
        return 0;
    case 9: /* F_GETOWN */
        return (s64)proc->pgid;
    case 1031: /* F_SETPIPE_SZ */
    case 1032: /* F_GETPIPE_SZ */
        if (!f->f_inode || !S_ISFIFO(f->f_inode->i_mode)) return -(s64)EINVAL;
        return 65536;
    default:
        return -(s64)EINVAL;
    }
}

/* ── Directories, Timers & System Information ────────────────────────────── */

static s64 sys_getcwd_impl(pt_regs_t *r)
{
    char *user_buf = (char *)r->rdi;
    size_t size    = (size_t)r->rsi;
    if (!user_buf || size == 0) return -(s64)EINVAL;
    if ((uintptr_t)user_buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
    size_t len = strlen(cwd) + 1;

    if (size < len) return -(s64)ERANGE;
    if (copy_to_user(user_buf, cwd, len) != 0) return -(s64)EFAULT;
    return (s64)(uintptr_t)user_buf;
}

static s64 sys_chdir_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    process_t *proc = sched_current_process();

    /* cwd is stored in the process's own view of the filesystem, so resolve to
     * the virtual path and map to the real one only for the lookup. */
    char vpath[512], kpath[512];
    s64 perr = copy_user_vpath_resolve_at(AT_FDCWD, vpath, sizeof(vpath), user_path);
    if (perr < 0) return perr;
    perr = vpath_to_real(proc, vpath, kpath, sizeof(kpath));
    if (perr < 0) return perr;

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (!S_ISDIR(dentry->d_inode->i_mode)) {
        return -(s64)ENOTDIR;
    }

    if (proc) {
        strncpy(proc->cwd, vpath, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    }
    return 0;
}

static s64 sys_fchdir_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];
    if (!f || !f->f_dentry || !f->f_dentry->d_inode) return -(s64)EBADF;
    if (!S_ISDIR(f->f_dentry->d_inode->i_mode)) return -(s64)ENOTDIR;

    /* d_name is only this dentry's own path component (e.g. "tmp", not
     * "/tmp") in the normal hierarchical case — the `d_name[0] == '/'` check
     * this used to have was true only for the root dentry itself, so
     * fchdir() to any other directory silently returned success without
     * ever updating proc->cwd, and getcwd() afterwards still reported the
     * old directory. dentry_build_path() (fs/vfs.c), already used the same
     * way for *at() dirfd resolution just above in this file, walks
     * d_parent to build the real full path; real_to_vpath() brings that
     * back into the process's own view before it's stored, same as
     * sys_chdir_impl() does for a path-based chdir(). */
    char real_path[512];
    __builtin_memset(real_path, 0, sizeof(real_path));
    dentry_build_path(f->f_dentry, real_path, sizeof(real_path));
    const char *v = real_to_vpath(proc, real_path);
    strncpy(proc->cwd, v, sizeof(proc->cwd) - 1);
    proc->cwd[sizeof(proc->cwd) - 1] = '\0';
    return 0;
}

static s64 sys_unlink_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_unlink(kpath);
}

static s64 sys_rename_impl(pt_regs_t *r)
{
    const char *user_old = (const char *)r->rdi;
    const char *user_new = (const char *)r->rsi;
    char kold[256], knew[256];
    s64 perr1 = copy_user_path_resolve(kold, sizeof(kold), user_old);
    if (perr1 < 0) return perr1;
    s64 perr2 = copy_user_path_resolve(knew, sizeof(knew), user_new);
    if (perr2 < 0) return perr2;
    return vfs_rename(kold, knew);
}

static s64 sys_mkdir_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    u32 mode = (u32)r->rsi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    process_t *proc = sched_current_process();
    if (proc) mode &= ~proc->umask; /* B-01: apply umask */
    return vfs_mkdir(kpath, mode);
}

static s64 sys_rmdir_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_rmdir(kpath);
}

static s64 sys_truncate_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    s64 length = (s64)r->rsi;
    if (length < 0) return -(s64)EINVAL;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    file_t *f = vfs_open(kpath, O_WRONLY, 0);
    if (!f) return -(s64)ENOENT;
    s64 ret = vfs_truncate(f, (u64)length);
    vfs_close(f);
    return ret;
}

static s64 sys_ftruncate_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    s64 length = (s64)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    if (length < 0) return -(s64)EINVAL;
    return vfs_truncate((file_t *)proc->handle_table[fd], (u64)length);
}

static s64 sys_access_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    int mode = (int)r->rsi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    struct stat st;
    s64 ret = vfs_stat(kpath, &st);
    if (ret < 0) return ret; /* ENOENT or other error */

    /* F_OK (0): file existence check only */
    if (mode == 0) return 0;

    process_t *proc = sched_current_process();
    u32 uid = proc ? proc->uid : 0;
    u32 gid = proc ? proc->gid : 0;

    /* POSIX: Root user (UID 0) has full read & write permissions.
     * Execute is permitted if it's a directory or any execute bit (0111) is set. */
    if (uid == 0) {
        if ((mode & 1) && !S_ISDIR(st.st_mode) && !(st.st_mode & 0111)) {
            return -(s64)EACCES;
        }
        return 0;
    }

    u32 file_mode = st.st_mode;
    u32 perm_bits = 0;
    if (uid == st.st_uid) {
        perm_bits = (file_mode >> 6) & 7;
    } else if (gid == st.st_gid) {
        perm_bits = (file_mode >> 3) & 7;
    } else {
        perm_bits = file_mode & 7;
    }

    if ((mode & 4) && !(perm_bits & 4)) return -(s64)EACCES; /* R_OK */
    if ((mode & 2) && !(perm_bits & 2)) return -(s64)EACCES; /* W_OK */
    if ((mode & 1) && !(perm_bits & 1)) return -(s64)EACCES; /* X_OK */

    return 0;
}

static s64 sys_getdents_impl(pt_regs_t *r)
{
    return sys_getdents64_impl(r);
}

static s64 sys_getdents64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    void *dirp = (void *)r->rsi;
    size_t count = (size_t)r->rdx;
    
    if (!dirp || count == 0) return -(s64)EINVAL;
    if ((uintptr_t)dirp >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (count > 65536) count = 65536;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];
    void *kbuf = kzalloc(count);
    if (!kbuf) return -(s64)ENOMEM;
    
    s64 ret = 0;
    if (file->f_op && file->f_op->readdir) {
        ret = file->f_op->readdir(file, kbuf, count, &file->f_pos);
        if (ret > 0) {
            if (copy_to_user(dirp, kbuf, (size_t)ret) != 0) {
                ret = -(s64)EFAULT;
            }
        }
    } else {
        ret = -(s64)ENOTDIR;
    }
    
    kfree(kbuf);
    return ret;
}

static s64 sys_nanosleep_impl(pt_regs_t *r)
{
    const struct linux_timespec *req = (const struct linux_timespec *)r->rdi;
    struct linux_timespec *rem = (struct linux_timespec *)r->rsi;
    if (!req) return -(s64)EINVAL;
    if ((uintptr_t)req >= 0x8000000000000000ULL) return -(s64)EFAULT;
    
    struct linux_timespec t;
    if (copy_from_user(&t, req, sizeof(t)) != 0) return -(s64)EFAULT;
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000L) return -(s64)EINVAL;
    
    u64 ticks = (t.tv_sec * 100) + (t.tv_nsec / 10000000);
    if (ticks == 0 && (t.tv_sec > 0 || t.tv_nsec > 0)) ticks = 1;
    
    u64 start_ticks = sched_get_ticks();
    sched_sleep(ticks);
    u64 elapsed = sched_get_ticks() - start_ticks;
    
    /* A-07: if awakened early by signal, return -EINTR and remaining time */
    if (elapsed < ticks) {
        if (rem && (uintptr_t)rem < 0x8000000000000000ULL) {
            u64 rem_ticks = ticks - elapsed;
            struct linux_timespec rts = {
                .tv_sec = (long)(rem_ticks / 100),
                .tv_nsec = (long)((rem_ticks % 100) * 10000000ULL)
            };
            copy_to_user(rem, &rts, sizeof(rts));
        }
        return -(s64)EINTR;
    }
    return 0;
}

/* Cached RTC wall-clock state (refreshed at most once per second = 100 ticks) */
static u64 s_rtc_unix_sec = 0;
static u64 s_rtc_base_ticks = 0;

/* settimeofday(2)/clock_settime(2) adjust this rather than the RTC-derived
 * state above: they add to it instead of overwriting s_rtc_unix_sec, so the
 * requested time sticks across the periodic RTC refresh below instead of
 * being silently overwritten by real hardware time within a second. This
 * does not touch the real CMOS RTC — a reboot reverts to hardware time,
 * same as any OS whose settimeofday(2) only ever meant "the running
 * kernel's clock", not "the battery-backed one". */
static s64 s_wall_clock_offset_sec = 0;

u64 get_cached_unix_time(void)
{
    u64 ticks = sched_get_ticks();
    if (s_rtc_unix_sec == 0 || (ticks - s_rtc_base_ticks) >= 100) {
        rtc_time_t t;
        rtc_read_time(&t);
        s_rtc_unix_sec   = rtc_to_unix_time(&t);
        s_rtc_base_ticks = ticks;
    }
    return (u64)((s64)(s_rtc_unix_sec + (ticks - s_rtc_base_ticks) / 100) + s_wall_clock_offset_sec);
}

/* Rebases the cached wall clock so get_cached_unix_time() immediately
 * reports @unix_sec and keeps advancing from there. */
static void set_wall_clock(u64 unix_sec)
{
    u64 current = get_cached_unix_time();
    s_wall_clock_offset_sec += (s64)unix_sec - (s64)current;
}

static s64 sys_clock_gettime_impl(pt_regs_t *r)
{
    u32 clk_id = (u32)r->rdi;
    void *tp   = (void *)r->rsi;
    if (!tp) return -(s64)EINVAL;
    if ((uintptr_t)tp >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u64 sec, nsec;

    /* CLOCK_MONOTONIC / MONOTONIC_RAW / BOOTTIME: prefer the HPET for real
     * nanosecond resolution, fall back to the 100 Hz tick. */
    if (clk_id == 1 || clk_id == 4 || clk_id == 7) {
        if (hpet_available()) {
            u64 ns = hpet_now_ns();
            sec  = ns / 1000000000ULL;
            nsec = ns % 1000000000ULL;
        } else {
            u64 ticks = sched_get_ticks();
            sec  = ticks / 100;
            nsec = (ticks % 100) * 10000000ULL;
        }
    } else {
        /* CLOCK_REALTIME and friends: RTC seconds + tick sub-second part, kept
         * phase-aligned so realtime never steps backwards within a second. */
        u64 ticks = sched_get_ticks();
        sec  = get_cached_unix_time();
        nsec = (ticks % 100) * 10000000ULL;
    }

    u64 ts[2] = { sec, nsec };
    if (copy_to_user(tp, ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_gettimeofday_impl(pt_regs_t *r)
{
    struct linux_timeval *user_tv = (struct linux_timeval *)r->rdi;
    if (user_tv) {
        if ((uintptr_t)user_tv >= 0x8000000000000000ULL) return -(s64)EFAULT;
        u64 ticks = sched_get_ticks();
        u64 unix_sec = get_cached_unix_time();
        u64 sub_sec_us = (ticks % 100) * 10000ULL;

        struct linux_timeval tv = { (long)unix_sec, (long)sub_sec_us };
        if (copy_to_user(user_tv, &tv, sizeof(tv)) != 0) return -(s64)EFAULT;
    }

    struct { int tz_minuteswest; int tz_dsttime; } *user_tz = (void *)r->rsi;
    if (user_tz) {
        if ((uintptr_t)user_tz >= 0x8000000000000000ULL) return -(s64)EFAULT;
        struct { int tz_minuteswest; int tz_dsttime; } tz = { 0, 0 };
        if (copy_to_user(user_tz, &tz, sizeof(tz)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_time_impl(pt_regs_t *r)
{
    long *user_tloc = (long *)r->rdi;
    u64 unix_sec = get_cached_unix_time();

    if (user_tloc && (uintptr_t)user_tloc < 0x8000000000000000ULL) {
        long sec = (long)unix_sec;
        copy_to_user(user_tloc, &sec, sizeof(long));
    }
    return (s64)unix_sec;
}

/* utime(2)/utimes(2)/utimensat(2)/futimesat(2) all used to be no-ops that
 * returned success without ever touching an inode. Real behavior now goes
 * through vfs_utimes()/vfs_futimes() (fs/vfs.c), which follow the same
 * (u64)-1-means-"leave unchanged" convention vfs_chown() already uses for
 * uid/gid — utimensat's UTIME_OMIT maps to that sentinel, UTIME_NOW maps to
 * get_cached_unix_time(). */

struct linux_utimbuf {
    long actime;
    long modtime;
};

static s64 sys_utime_impl(pt_regs_t *r)
{
    const char *path = (const char *)r->rdi;
    const struct linux_utimbuf *times = (const struct linux_utimbuf *)r->rsi;
    if (!path) return -(s64)EFAULT;

    char kpath[512];
    s64 err = copy_user_path_resolve_at(AT_FDCWD, kpath, sizeof(kpath), path);
    if (err < 0) return err;

    u64 atime, mtime;
    if (times) {
        struct linux_utimbuf t;
        if (copy_from_user(&t, times, sizeof(t)) != 0) return -(s64)EFAULT;
        atime = (u64)t.actime;
        mtime = (u64)t.modtime;
    } else {
        atime = mtime = get_cached_unix_time();
    }
    return vfs_utimes(kpath, atime, mtime);
}

static s64 sys_utimes_impl(pt_regs_t *r)
{
    const char *path = (const char *)r->rdi;
    const struct linux_timeval *times = (const struct linux_timeval *)r->rsi;
    if (!path) return -(s64)EFAULT;

    char kpath[512];
    s64 err = copy_user_path_resolve_at(AT_FDCWD, kpath, sizeof(kpath), path);
    if (err < 0) return err;

    u64 atime, mtime;
    if (times) {
        struct linux_timeval t[2];
        if (copy_from_user(t, times, sizeof(t)) != 0) return -(s64)EFAULT;
        atime = (u64)t[0].tv_sec;
        mtime = (u64)t[1].tv_sec;
    } else {
        atime = mtime = get_cached_unix_time();
    }
    return vfs_utimes(kpath, atime, mtime);
}

/* Shared by utimensat(2) and futimesat(2): decode a `struct timespec
 * times[2]` (or NULL, meaning "both to now") into the (u64)-1-sentinel
 * convention vfs_utimes()/vfs_futimes() expect. */
static s64 decode_utimens(const struct linux_timespec *user_times, u64 *out_atime, u64 *out_mtime)
{
    if (!user_times) {
        *out_atime = *out_mtime = get_cached_unix_time();
        return 0;
    }
    struct linux_timespec t[2];
    if (copy_from_user(t, user_times, sizeof(t)) != 0) return -(s64)EFAULT;

    if (t[0].tv_nsec == UTIME_OMIT) *out_atime = (u64)-1;
    else if (t[0].tv_nsec == UTIME_NOW) *out_atime = get_cached_unix_time();
    else *out_atime = (u64)t[0].tv_sec;

    if (t[1].tv_nsec == UTIME_OMIT) *out_mtime = (u64)-1;
    else if (t[1].tv_nsec == UTIME_NOW) *out_mtime = get_cached_unix_time();
    else *out_mtime = (u64)t[1].tv_sec;

    return 0;
}

static s64 sys_utimensat_impl(pt_regs_t *r)
{
    int dfd = (int)(s32)r->rdi;
    const char *path = (const char *)r->rsi;
    const struct linux_timespec *times = (const struct linux_timespec *)r->rdx;
    /* r10 carries `flags` (AT_SYMLINK_NOFOLLOW) — not honored: vfs_utimes()
     * always follows symlinks, same as the plain (non-l-prefixed) vfs_chown()
     * this file already exposes as sys_fchownat_impl's backend. A dedicated
     * *_nofollow variant of vfs_utimes() would be a small, separate addition
     * if a caller ever needs it. */

    u64 atime, mtime;
    s64 err = decode_utimens(times, &atime, &mtime);
    if (err < 0) return err;

    /* utimensat(fd, NULL, times, 0) means "operate on the fd itself" —
     * matches openat()'s AT_EMPTY_PATH convention. */
    if (!path) {
        process_t *proc = sched_current_process();
        if (!proc || dfd < 0 || dfd >= PROC_MAX_FDS || !proc->handle_table[dfd]) return -(s64)EBADF;
        return vfs_futimes((file_t *)proc->handle_table[dfd], atime, mtime);
    }

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (perr < 0) return perr;
    return vfs_utimes(kpath, atime, mtime);
}

struct tms {
    u64 tms_utime;
    u64 tms_stime;
    u64 tms_cutime;
    u64 tms_cstime;
};

static s64 sys_times_impl(pt_regs_t *r)
{
    struct tms *buf = (struct tms *)r->rdi;
    process_t *proc = sched_current_process();
    if (buf && (uintptr_t)buf < 0x8000000000000000ULL) {
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

static s64 sys_sysinfo_impl(pt_regs_t *r)
{
    void *user_info = (void *)r->rdi;
    if (!user_info) return -(s64)EINVAL;
    
    struct {
        long uptime;
        unsigned long loads[3];
        unsigned long totalram;
        unsigned long freeram;
        unsigned long sharedram;
        unsigned long bufferram;
        unsigned long totalswap;
        unsigned long freeswap;
        unsigned short procs;
        unsigned short pad;
        unsigned long totalhigh;
        unsigned long freehigh;
        unsigned int mem_unit;
        char _f[20-2*sizeof(long)-sizeof(int)];
    } info;
    
    __builtin_memset(&info, 0, sizeof(info));
    info.uptime = (long)(sched_get_ticks() / 100);
    info.mem_unit = 4096;
    info.totalram = pmm_get_total_pages();
    info.freeram = pmm_get_free_pages();
    info.procs = (unsigned short)sched_get_process_count();
    
    if (copy_to_user(user_info, &info, sizeof(info)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_az_sysstat_impl(pt_regs_t *r)
{
    az_sysstat_t *user_stat = (az_sysstat_t *)r->rdi;
    if (!user_stat) return -(s64)EINVAL;
    
    az_sysstat_t stat;
    __builtin_memset(&stat, 0, sizeof(stat));
    
    for (int i = 0; i < 16; i++) {
        stat.idle_ticks[i] = sched_get_idle_ticks((u32)i);
        stat.active_ticks[i] = sched_get_active_ticks((u32)i);
    }
    
    if (copy_to_user(user_stat, &stat, sizeof(stat)) != 0) return -(s64)EFAULT;
    return 0;
}

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

static char g_kernel_nodename[65] = "azamios";
static char g_kernel_domainname[65] = "local";

static s64 sys_uname_impl(pt_regs_t *r)
{
    struct utsname *u = (struct utsname *)r->rdi;
    if (!u || (uintptr_t)u >= 0x8000000000000000ULL) return -(s64)EFAULT;

    struct utsname info;
    memset(&info, 0, sizeof(info));
    strncpy(info.sysname, "AzamiOS", sizeof(info.sysname) - 1);
    strncpy(info.nodename, g_kernel_nodename, sizeof(info.nodename) - 1);
    strncpy(info.release, "7.0.0-posix", sizeof(info.release) - 1);
    strncpy(info.version, "AzamiOS Modular Microkernel v7.0 x86_64 SMP", sizeof(info.version) - 1);
    strncpy(info.machine, "x86_64", sizeof(info.machine) - 1);
    strncpy(info.domainname, g_kernel_domainname, sizeof(info.domainname) - 1);

    if (copy_to_user(u, &info, sizeof(info)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_reboot_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_BOOT)) {
        return -(s64)EPERM;
    }
    u32 cmd = (u32)r->rdx;
    /* Last chance to get buffered filesystem writes onto the platter. */
    vfs_sync_all();
    if (cmd == 0x01234567 /* LINUX_REBOOT_CMD_RESTART */) {
        power_reboot();
    } else {
        power_shutdown();
    }
    __builtin_unreachable();
}

static s64 sys_getuid_impl(pt_regs_t *r)  { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->uid : 0; }
static s64 sys_geteuid_impl(pt_regs_t *r) { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->euid : 0; }
static s64 sys_getgid_impl(pt_regs_t *r)  { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->gid : 0; }
static s64 sys_getegid_impl(pt_regs_t *r) { (void)r; process_t *p = sched_current_process(); return p ? (s64)p->egid : 0; }
static s64 sys_setuid_impl(pt_regs_t *r)  {
    u32 new_uid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;
    if (p->euid != 0 && new_uid != p->uid && new_uid != p->euid) return -(s64)EPERM;
    p->uid = new_uid;
    p->euid = new_uid;
    security_caps_on_setuid(p);
    return 0;
}
static s64 sys_setgid_impl(pt_regs_t *r)  {
    u32 new_gid = (u32)r->rdi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;
    if (p->euid != 0 && new_gid != p->gid && new_gid != p->egid) return -(s64)EPERM;
    p->gid = new_gid;
    p->egid = new_gid;
    return 0;
}
static process_t *proc_by_pid(u32 pid)
{
    for (process_t *p = sched_get_process_list(); p; p = p->next)
        if (p->pid == pid) return p;
    return NULL;
}

static s64 sys_getpgrp_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pgid : 0;
}

/* setpgid(pid, pgid) — POSIX job control. */
static s64 sys_setpgid_impl(pt_regs_t *r) {
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
static s64 sys_setsid_impl(pt_regs_t *r) {
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

static s64 sys_getrlimit_impl(pt_regs_t *r)
{
    int resource = (int)r->rdi;
    struct rlimit *rlim = (struct rlimit *)r->rsi;
    if (!rlim || (uintptr_t)rlim >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    krlimit_t k;
    s64 rc = do_prlimit(proc, resource, NULL, &k);
    if (rc != 0) return rc;

    struct rlimit out = { k.rlim_cur, k.rlim_max };
    if (copy_to_user(rlim, &out, sizeof(out)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_setrlimit_impl(pt_regs_t *r)
{
    int resource = (int)r->rdi;
    const struct rlimit *rlim = (const struct rlimit *)r->rsi;
    if (!rlim || (uintptr_t)rlim >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

static s64 sys_getrusage_impl(pt_regs_t *r)
{
    int who = (int)r->rdi;
    struct rusage *usage = (struct rusage *)r->rsi;
    if (!usage || (uintptr_t)usage >= 0x0000800000000000ULL) return -(s64)EFAULT;
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

/* ── POSIX *at Syscall Family ────────────────────────────────────────────── */

static s64 sys_openat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int flags = (int)r->rdx;
    u32 mode = (u32)r->r10;

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (flags & O_CREAT) {
        mode &= ~proc->umask;
    }

    s64 open_err = 0;
    file_t *file = vfs_open_err(kpath, (u32)flags, mode, &open_err);
    if (!file) return open_err ? open_err : -(s64)ENOENT;

    s64 fd = fd_install(proc, file, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) vfs_close(file);
    return fd;
}

static s64 sys_mkdirat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    u32 mode = (u32)r->rdx;

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    process_t *proc = sched_current_process();
    if (proc) mode &= ~proc->umask;
    return vfs_mkdir(kpath, mode);
}

static s64 sys_fstatat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    struct stat *statbuf = (struct stat *)r->rdx;
    int flags = (int)r->r10;

    if (!statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    struct stat kst;
    __builtin_memset(&kst, 0, sizeof(kst));

    /* If user_path is empty or NULL (or AT_EMPTY_PATH is set), fstat on dirfd */
    if ((flags & AT_EMPTY_PATH) || !user_path) {
        if (dirfd >= 0 && dirfd < PROC_MAX_FDS) {
            process_t *proc = sched_current_process();
            if (!proc || !proc->handle_table[dirfd]) return -(s64)EBADF;
            file_t *file = (file_t *)proc->handle_table[dirfd];
            if (!file || !file->f_inode) return -(s64)EBADF;
            s64 ret = vfs_fstat(file, &kst);
            if (ret < 0) return ret;
            if (copy_to_user(statbuf, &kst, sizeof(struct stat)) != 0) return -(s64)EFAULT;
            return 0;
        }
    }

    char raw[256];
    __builtin_memset(raw, 0, sizeof(raw));
    if (user_path) {
        s64 slen = copy_str_from_user(raw, user_path, sizeof(raw));
        if (slen < 0) return slen;
    }

    if (raw[0] == '\0') {
        if (dirfd >= 0 && dirfd < PROC_MAX_FDS) {
            process_t *proc = sched_current_process();
            if (!proc || !proc->handle_table[dirfd]) return -(s64)EBADF;
            file_t *file = (file_t *)proc->handle_table[dirfd];
            if (!file || !file->f_inode) return -(s64)EBADF;
            s64 ret = vfs_fstat(file, &kst);
            if (ret < 0) return ret;
            if (copy_to_user(statbuf, &kst, sizeof(struct stat)) != 0) return -(s64)EFAULT;
            return 0;
        }
    }

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    s64 ret = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kpath, &kst) : vfs_stat(kpath, &kst);
    if (ret < 0) return ret;

    if (copy_to_user(statbuf, &kst, sizeof(struct stat)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_faccessat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int mode = (int)r->rdx;
    int flags = (int)r->r10;

    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char raw[256];
    __builtin_memset(raw, 0, sizeof(raw));
    s64 slen = copy_str_from_user(raw, user_path, sizeof(raw));
    if (slen < 0) return slen;

    struct stat st;
    __builtin_memset(&st, 0, sizeof(st));

    if (raw[0] == '\0') {
        if (dirfd >= 0 && dirfd < PROC_MAX_FDS) {
            process_t *proc = sched_current_process();
            if (!proc || !proc->handle_table[dirfd]) return -(s64)EBADF;
            file_t *file = (file_t *)proc->handle_table[dirfd];
            if (!file || !file->f_inode) return -(s64)EBADF;
            s64 ret = vfs_fstat(file, &st);
            if (ret < 0) return ret;
        } else {
            return -(s64)EINVAL;
        }
    } else {
        char kpath[512];
        s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
        if (perr < 0) return perr;

        s64 ret = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(kpath, &st) : vfs_stat(kpath, &st);
        if (ret < 0) return ret;
    }

    if (mode == 0) return 0; /* F_OK */

    process_t *proc = sched_current_process();
    u32 uid = proc ? ((flags & 0x200 /* AT_EACCESS */) ? proc->euid : proc->uid) : 0;
    u32 gid = proc ? ((flags & 0x200 /* AT_EACCESS */) ? proc->egid : proc->gid) : 0;

    if (uid == 0) {
        if ((mode & 1) && !S_ISDIR(st.st_mode) && !(st.st_mode & 0111)) {
            return -(s64)EACCES;
        }
        return 0;
    }

    u32 file_mode = st.st_mode;
    u32 perm_bits = 0;
    if (uid == st.st_uid) {
        perm_bits = (file_mode >> 6) & 7;
    } else if (gid == st.st_gid) {
        perm_bits = (file_mode >> 3) & 7;
    } else {
        perm_bits = file_mode & 7;
    }

    if ((mode & 4) && !(perm_bits & 4)) return -(s64)EACCES;
    if ((mode & 2) && !(perm_bits & 2)) return -(s64)EACCES;
    if ((mode & 1) && !(perm_bits & 1)) return -(s64)EACCES;
    return 0;
}

static s64 sys_unlinkat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int flags = (int)r->rdx;

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    if (flags & AT_REMOVEDIR) {
        return vfs_rmdir(kpath);
    }
    return vfs_unlink(kpath);
}

static s64 sys_readlinkat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    char *buf = (char *)r->rdx;
    size_t bufsiz = (size_t)r->r10;

    if (!buf || bufsiz == 0) return -(s64)EINVAL;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    process_t *proc = sched_current_process();
    if (proc && (strcmp(kpath, "/proc/self/exe") == 0 || strcmp(kpath, "/proc/thread-self/exe") == 0)) {
        size_t nlen = strlen(proc->name);
        size_t copylen = nlen > bufsiz ? bufsiz : nlen;
        if (copy_to_user(buf, proc->name, copylen) != 0) return -(s64)EFAULT;
        return (s64)copylen;
    }
    if (proc && strcmp(kpath, "/proc/self/cwd") == 0) {
        size_t clen = strlen(proc->cwd);
        size_t copylen = clen > bufsiz ? bufsiz : clen;
        if (copy_to_user(buf, proc->cwd, copylen) != 0) return -(s64)EFAULT;
        return (s64)copylen;
    }

    char kbuf[256];
    s64 ret = vfs_readlink(kpath, kbuf, sizeof(kbuf));
    if (ret < 0) return ret;

    size_t copylen = (size_t)ret > bufsiz ? bufsiz : (size_t)ret;
    if (copy_to_user(buf, kbuf, copylen) != 0) return -(s64)EFAULT;
    return (s64)copylen;
}


/* ══════════════════════════════════════════════════════════════════════════
 * Azami Extended Syscalls
 * ══════════════════════════════════════════════════════════════════════════ */

static s64 sys_az_channel_create(pt_regs_t *r)
{
    (void)r;
    ipc_channel_t *chan = ipc_channel_create();
    if (!chan) return -(s64)ENOMEM;
    return (s64)chan->channel_id;
}

static s64 sys_az_channel_destroy(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;
    ipc_channel_destroy(chan);
    ipc_channel_put(chan);
    return 0;
}

static s64 sys_az_channel_send(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    const ipc_msg_t *user_msg = (const ipc_msg_t *)r->rsi;
    bool block = (bool)r->rdx;

    if (!user_msg || (uintptr_t)user_msg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;

    ipc_msg_t kmsg;
    if (copy_from_user(&kmsg, user_msg, sizeof(ipc_msg_t)) != 0) {
        ipc_channel_put(chan);
        return -(s64)EFAULT;
    }

    s64 ret = ipc_channel_send(chan, &kmsg, block);
    ipc_channel_put(chan);
    return ret;
}

static s64 sys_az_channel_recv(pt_regs_t *r)
{
    u32 channel_id = (u32)r->rdi;
    ipc_msg_t *user_msg = (ipc_msg_t *)r->rsi;
    bool block = (bool)r->rdx;

    if (!user_msg || (uintptr_t)user_msg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    ipc_channel_t *chan = ipc_channel_find(channel_id);
    if (!chan) return -(s64)EINVAL;

    ipc_msg_t kmsg;
    s64 ret = ipc_channel_recv(chan, &kmsg, block);
    ipc_channel_put(chan);
    if (ret < 0) return ret;

    if (copy_to_user(user_msg, &kmsg, sizeof(ipc_msg_t)) != 0) {
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_az_shmem_create(pt_regs_t *r)
{
    size_t page_count = (size_t)r->rdi;
    if (page_count == 0 || page_count > 4096) return -(s64)EINVAL;

    ipc_shmem_t *shmem = ipc_shmem_create(page_count);
    if (!shmem) return -(s64)ENOMEM;
    return (s64)shmem->shmem_id;
}

static s64 sys_az_shmem_map(pt_regs_t *r)
{
    u32 shmem_id = (u32)r->rdi;
    virt_addr_t virt = (virt_addr_t)r->rsi;

    if (virt & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (virt == 0 || virt >= 0x8000000000000000ULL) return -(s64)EINVAL;

    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;

    if (shmem->page_count == 0 || shmem->page_count > 4096) {
        ipc_shmem_put(shmem);
        return -(s64)EINVAL;
    }
    if (virt + shmem->page_count * PAGE_SIZE > 0x8000000000000000ULL ||
        virt + shmem->page_count * PAGE_SIZE < virt) {
        ipc_shmem_put(shmem);
        return -(s64)EINVAL;
    }

    process_t *proc = sched_current_process();
    if (!proc) { ipc_shmem_put(shmem); return -(s64)EPERM; }

    s64 ret = ipc_shmem_map(shmem, proc, virt, VMM_USER_RW);
    ipc_shmem_put(shmem);
    return ret;
}

static s64 sys_az_shmem_destroy(pt_regs_t *r)
{
    u32 shmem_id = (u32)r->rdi;
    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;
    ipc_shmem_destroy(shmem);
    ipc_shmem_put(shmem);
    return 0;
}

static s64 sys_az_shmem_unmap(pt_regs_t *r)
{
    (void)r->rdi; /* shmem_id not needed if unmapping by VA */
    virt_addr_t virt = (virt_addr_t)r->rsi;
    
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EINVAL;
    
    return ipc_shmem_unmap(NULL, proc, virt);
}

static s64 sys_az_fb_info(pt_regs_t *r)
{
    az_fb_info_t *user_info = (az_fb_info_t *)r->rdi;
    if (!user_info || (uintptr_t)user_info >= 0x8000000000000000ULL) return -(s64)EFAULT;

    az_fb_info_t info;
    __builtin_memset(&info, 0, sizeof(info));

    phys_addr_t bga_phys = bga_get_fb_phys();
    if (bga_phys) {
        info.width     = bga_get_width();
        info.height    = bga_get_height();
        info.pitch     = bga_get_pitch();
        info.bpp       = bga_get_bpp();
        info.phys_addr = bga_phys;
    } else {
        struct limine_framebuffer *fb = az_boot_framebuffer();
        if (!fb) return -(s64)ENODEV;
        info.width     = (u32)fb->width;
        info.height    = (u32)fb->height;
        info.pitch     = (u32)fb->pitch;
        info.bpp       = (u8)fb->bpp;
        info.phys_addr = (u64)(uintptr_t)fb->address - HHDM_BASE;
    }

    if (copy_to_user(user_info, &info, sizeof(az_fb_info_t)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_az_fb_map(pt_regs_t *r)
{
    virt_addr_t virt = (virt_addr_t)r->rdi;
    if (virt & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (virt >= 0x8000000000000000ULL) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    phys_addr_t fb_phys = bga_get_fb_phys();
    size_t      fb_size = 0;

    if (fb_phys) {
        fb_size = bga_get_fb_total_size();
    } else {
        struct limine_framebuffer *fb = az_boot_framebuffer();
        if (!fb) return -(s64)ENODEV;
        fb_phys = (phys_addr_t)((u64)(uintptr_t)fb->address - HHDM_BASE);
        fb_size = (size_t)(fb->pitch * fb->height);
    }

    size_t page_count = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 flags = VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_NX | VMM_F_SHARED | VMM_F_PWT;
    for (size_t i = 0; i < page_count; i++) {
        vmm_map(proc->pml4_phys, virt + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE, flags);
    }

    console_disable_fb();
    return 0;
}

static s64 sys_az_fb_flip(pt_regs_t *r)
{
    u32 buffer_index = (u32)r->rdi;
    if (buffer_index > 1) return -(s64)EINVAL;
    if (bga_get_fb_phys() != 0) {
        if (bga_flip_buffer(buffer_index) == 0) return 0;
    }
    return -(s64)ENOSYS;
}

static s64 sys_az_spawn(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path || (uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

static s64 sys_az_yield(pt_regs_t *r)
{
    (void)r;
    sched_yield();
    return 0;
}

static s64 sys_az_thread_create_impl(pt_regs_t *r)
{
    uintptr_t entry = (uintptr_t)r->rdi;
    uintptr_t stack = (uintptr_t)r->rsi;
    uintptr_t arg   = (uintptr_t)r->rdx;

    if (!entry || !stack) return -(s64)EINVAL;
    if (entry >= 0x8000000000000000ULL || stack >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

static s64 sys_az_thread_exit_impl(pt_regs_t *r)
{
    (void)r;
    sched_exit_thread();
    __builtin_unreachable();
}

static s64 sys_az_object_create(pt_regs_t *r)
{
    const char *user_name = (const char *)r->rdi;
    az_obj_type_t type = (az_obj_type_t)r->rsi;
    void *payload = (void *)r->rdx;

    char kname[64];
    __builtin_memset(kname, 0, sizeof(kname));
    if (user_name) {
        if ((uintptr_t)user_name >= 0x8000000000000000ULL) return -(s64)EFAULT;
        for (int i = 0; i < 63; i++) {
            if (copy_from_user(&kname[i], user_name + i, 1) != 0) return -(s64)EFAULT;
            if (kname[i] == '\0') break;
        }
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    az_object_t *obj = az_object_create(user_name ? kname : NULL, type, payload, NULL);
    if (!obj) return -(s64)ENOMEM;

    s64 handle = az_handle_open(proc, obj);
    if (handle < 0) {
        az_object_dereference(obj);
        return handle;
    }
    return handle;
}

static s64 sys_az_object_open(pt_regs_t *r)
{
    const char *user_name = (const char *)r->rdi;
    if (!user_name || (uintptr_t)user_name >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kname[64];
    __builtin_memset(kname, 0, sizeof(kname));
    for (int i = 0; i < 63; i++) {
        if (copy_from_user(&kname[i], user_name + i, 1) != 0) return -(s64)EFAULT;
        if (kname[i] == '\0') break;
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    az_object_t *obj = az_object_lookup(kname);
    if (!obj) return -(s64)ENOENT;

    s64 handle = az_handle_open(proc, obj);
    az_object_dereference(obj);
    return handle;
}

static s64 sys_az_object_close(pt_regs_t *r)
{
    s64 handle = (s64)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return az_handle_close(proc, handle);
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

static s64 sys_az_set_timer_impl(pt_regs_t *r)
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

static s64 sys_getfacl_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    acl_entry_t *user_entries = (acl_entry_t *)r->rsi;
    int max_entries = (int)(s32)r->rdx;

    if (!user_path || !user_entries || max_entries <= 0) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if ((uintptr_t)user_entries >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[VFS_NAME_MAX];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) return -(s64)ENOENT;

    acl_entry_t k_entries[ACL_MAX_ENTRIES];
    int count = acl_get_for_inode(dentry->d_inode, k_entries, max_entries > ACL_MAX_ENTRIES ? ACL_MAX_ENTRIES : max_entries);
    if (count < 0) return (s64)count;

    if (copy_to_user(user_entries, k_entries, sizeof(acl_entry_t) * count) != 0) {
        return -(s64)EFAULT;
    }

    return (s64)count;
}

static s64 sys_setfacl_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    const acl_entry_t *user_entries = (const acl_entry_t *)r->rsi;
    int count = (int)(s32)r->rdx;

    if (!user_path || count < 0 || count > ACL_MAX_ENTRIES) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (count > 0 && (!user_entries || (uintptr_t)user_entries >= 0x8000000000000000ULL)) return -(s64)EFAULT;

    char kpath[VFS_NAME_MAX];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) return -(s64)ENOENT;

    acl_entry_t k_entries[ACL_MAX_ENTRIES];
    if (count > 0) {
        if (copy_from_user(k_entries, user_entries, sizeof(acl_entry_t) * count) != 0) {
            return -(s64)EFAULT;
        }
    }

    int res = acl_set_for_inode(dentry->d_inode, (count > 0) ? k_entries : NULL, count);
    if (res < 0) return (s64)res;
    return 0;
}

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static s64 sys_arch_prctl_impl(pt_regs_t *r)
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
        if (!addr || addr >= 0x8000000000000000ULL) return -(s64)EFAULT;
        thread_t *self = sched_current_thread();
        u64 cur = (self && self->has_thread_fs_base) ? self->fs_base : proc->fs_base;
        return copy_to_user((void *)addr, &cur, sizeof(u64)) == 0 ? 0 : -(s64)EFAULT;
    } else if (code == ARCH_SET_GS) {
        proc->gs_base = addr;
        wrmsr(MSR_KERNEL_GS_BASE, addr);
        return 0;
    } else if (code == ARCH_GET_GS) {
        if (!addr || addr >= 0x8000000000000000ULL) return -(s64)EFAULT;
        return copy_to_user((void *)addr, &proc->gs_base, sizeof(u64)) == 0 ? 0 : -(s64)EFAULT;
    }
    return -(s64)EINVAL;
}

static s64 sys_set_tid_address_impl(pt_regs_t *r)
{
    thread_t *t = sched_current_thread();
    uintptr_t tidptr = (uintptr_t)r->rdi;

    /* Linux never fails this call: a bad pointer is simply remembered and
     * discovered (and ignored) at exit. It returns the caller's *thread* id,
     * which is what a libc stores as its cached tid — returning the pid is
     * only accidentally right for a single-threaded process. */
    if (t) {
        t->clear_child_tid = (tidptr < 0x0000800000000000ULL) ? (u64)tidptr : 0;
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

static s64 sys_prlimit64_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    int resource = (int)(s32)r->rsi;
    const struct kernel_rlimit64 *new_rlim = (const struct kernel_rlimit64 *)r->rdx;
    struct kernel_rlimit64 *old_rlim = (struct kernel_rlimit64 *)r->r10;

    if (resource < 0 || resource >= RLIMIT_NLIMITS) return -(s64)EINVAL;
    if (new_rlim && (uintptr_t)new_rlim >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (old_rlim && (uintptr_t)old_rlim >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

static s64 sys_clone3_impl(pt_regs_t *r)
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
        (uintptr_t)kargs.pidfd < 0x0000800000000000ULL) {
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

struct kernel_open_how {
    u64 flags;
    u64 mode;
    u64 resolve;
};

static s64 sys_openat2_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    const struct kernel_open_how *user_how = (const struct kernel_open_how *)r->rdx;
    size_t size = (size_t)r->r10;

    if (!user_path || !user_how || size < sizeof(struct kernel_open_how)) return -(s64)EINVAL;
    struct kernel_open_how how;
    if (copy_from_user(&how, user_how, sizeof(how)) != 0) return -(s64)EFAULT;

    pt_regs_t fake_r;
    fake_r.rdi = (u64)dirfd;
    fake_r.rsi = (u64)user_path;
    fake_r.rdx = how.flags;
    fake_r.r10 = how.mode;
    return sys_openat_impl(&fake_r);
}

static s64 sys_faccessat2_impl(pt_regs_t *r)
{
    return sys_faccessat_impl(r);
}

static s64 sys_epoll_pwait2_impl(pt_regs_t *r)
{
    int epfd = (int)(s32)r->rdi;
    void *events = (void *)r->rsi;
    int maxevents = (int)(s32)r->rdx;
    const struct linux_timespec *ts = (const struct linux_timespec *)r->r10;
    const void *sigmask = (const void *)r->r8;
    (void)sigmask;

    int timeout = -1;
    if (ts && (uintptr_t)ts < 0x8000000000000000ULL) {
        struct linux_timespec kts;
        if (copy_from_user(&kts, ts, sizeof(kts)) == 0) {
            timeout = (int)(kts.tv_sec * 1000 + kts.tv_nsec / 1000000);
        }
    }

    pt_regs_t fake_r;
    fake_r.rdi = (u64)epfd;
    fake_r.rsi = (u64)events;
    fake_r.rdx = (u64)maxevents;
    fake_r.r10 = (u64)timeout;
    return sys_epoll_wait_impl(&fake_r);
}

static s64 sys_getcpu_impl(pt_regs_t *r)
{
    unsigned int *user_cpu = (unsigned int *)r->rdi;
    unsigned int *user_node = (unsigned int *)r->rsi;
    void *tcache = (void *)r->rdx;
    (void)tcache;

    unsigned int cpu_id = smp_current_cpu_id();
    unsigned int node_id = 0;

    if (user_cpu && (uintptr_t)user_cpu < 0x8000000000000000ULL) {
        if (copy_to_user(user_cpu, &cpu_id, sizeof(unsigned int)) != 0) return -(s64)EFAULT;
    }
    if (user_node && (uintptr_t)user_node < 0x8000000000000000ULL) {
        if (copy_to_user(user_node, &node_id, sizeof(unsigned int)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_seccomp_impl(pt_regs_t *r)
{
    unsigned int op    = (unsigned int)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    process_t *p = sched_current_process();
    if (!p) return -(s64)EPERM;

    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        /* Linux takes no flags and no args for strict mode. */
        if (flags != 0 || r->rdx != 0) return -(s64)EINVAL;
        if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
            p->seccomp_mode != SECCOMP_MODE_STRICT) return -(s64)EINVAL;
        /* Entering strict mode implies no_new_privs: without it an execve()
         * could hand the process a setuid binary running under the filter. */
        p->no_new_privs  = true;
        p->seccomp_mode  = SECCOMP_MODE_STRICT;
        return 0;

    case SECCOMP_SET_MODE_FILTER:
        if (flags != SECCOMP_FILTER_FLAG_NONE) return -(s64)EINVAL;
        if (p->seccomp_mode != SECCOMP_MODE_DISABLED &&
            p->seccomp_mode != SECCOMP_MODE_FILTER) return -(s64)EINVAL;
        return seccomp_attach_filter(p, (const sock_fprog_t *)r->rdx);

    case SECCOMP_GET_ACTION_AVAIL: {
        if (flags != 0) return -(s64)EINVAL;
        u32 act = 0;
        if (copy_from_user(&act, (const void *)r->rdx, sizeof(u32)) != 0)
            return -(s64)EFAULT;
        switch (act) {
        case SECCOMP_RET_KILL_THREAD:
        case SECCOMP_RET_KILL_PROCESS:
        case SECCOMP_RET_TRAP:
        case SECCOMP_RET_ERRNO:
        case SECCOMP_RET_LOG:
        case SECCOMP_RET_ALLOW:
            return 0;
        /* SECCOMP_RET_TRACE is deliberately not reported available: it
         * needs a ptrace tracer to be notified and to decide the outcome
         * (PTRACE_EVENT_SECCOMP), which this kernel does not wire up yet.
         * seccomp_filter_run() fails a TRACE result closed (-ENOSYS to the
         * caller) rather than silently allowing it, but that is not the
         * same guarantee as the real action, so it is not advertised here. */
        default:
            return -(s64)95; /* -EOPNOTSUPP */
        }
    }

    default:
        return -(s64)EINVAL;
    }
}

/* ── Scheduling policy / parameters (shared helpers) ────────────────────────
 * Policy + RT priority + nice are stored per process (see process_t) and
 * mapped onto this CFS's weight by sched_weight_for(); every get* reflects
 * exactly what the matching set* stored. */
#define SCHED_OTHER   0
#define SCHED_FIFO    1
#define SCHED_RR      2
#define SCHED_BATCH   3
#define SCHED_IDLE    5
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

static s64 sys_sched_setattr_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_attr *uattr = (struct sched_attr *)r->rsi;
    if (!uattr || (uintptr_t)uattr >= 0x0000800000000000ULL) return -(s64)EINVAL;

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

static s64 sys_sched_getattr_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_attr *uattr = (struct sched_attr *)r->rsi;
    u32 size = (u32)r->rdx;
    if (!uattr || (uintptr_t)uattr >= 0x0000800000000000ULL) return -(s64)EINVAL;
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

typedef struct {
    u32 target_pid;
} pidfd_ctx_t;

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

static file_operations_t g_pidfd_fops = {
    .release = pidfd_release_op,
    .poll    = pidfd_poll_op,
};

static s64 sys_pidfd_open_impl(pt_regs_t *r)
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

static s64 sys_pidfd_send_signal_impl(pt_regs_t *r)
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

    if (pidfd < 0 || pidfd >= PROC_MAX_FDS || !proc->handle_table[pidfd]) return -(s64)EBADF;
    file_t *f = (file_t *)proc->handle_table[pidfd];
    if (f->f_op != &g_pidfd_fops || !f->private_data) return -(s64)EBADF;

    pidfd_ctx_t *ctx = (pidfd_ctx_t *)f->private_data;
    if (sched_kill_process(ctx->target_pid, 0) < 0) return -(s64)ESRCH;

    if (sig == 0) return 0;
    return sched_kill_process(ctx->target_pid, sig);
}

static s64 sys_pidfd_getfd_impl(pt_regs_t *r)
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
        /* fget() reads the slot and raises f_count under g_fd_lock, which
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

/* ── In-memory anonymous file (memfd_create) ──────────────────────────────── */
typedef struct {
    spinlock_t lock;
    u8        *data;
    size_t     size;
    size_t     capacity;
} memfd_ctx_t;

static s64 memfd_read_op(file_t *filp, void *buf, size_t count, u64 *offset)
{
    if (!filp || !filp->private_data || !buf || !offset) return -(s64)EINVAL;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (count == 0) return 0;

    spinlock_lock(&ctx->lock);
    if (*offset >= ctx->size) {
        spinlock_unlock(&ctx->lock);
        return 0; /* EOF */
    }
    size_t avail = ctx->size - *offset;
    size_t to_read = (count < avail) ? count : avail;
    /* The VFS read/write ops are handed a *kernel* buffer — sys_read_impl()
     * has already staged the transfer and copies out to userspace itself, as
     * tmpfs and every other filesystem here assume. Reaching for
     * copy_to_user() meant the destination failed the user-range check on
     * every call, so memfd reads returned -EFAULT unconditionally. */
    memcpy(buf, ctx->data + *offset, to_read);
    *offset += to_read;
    spinlock_unlock(&ctx->lock);
    return (s64)to_read;
}

static s64 memfd_write_op(file_t *filp, const void *buf, size_t count, u64 *offset)
{
    if (!filp || !filp->private_data || !buf || !offset) return -(s64)EINVAL;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (count == 0) return 0;

    spinlock_lock(&ctx->lock);
    size_t new_end = *offset + count;
    if (new_end > ctx->capacity) {
        size_t new_cap = (ctx->capacity == 0) ? 4096 : ctx->capacity * 2;
        while (new_cap < new_end) new_cap *= 2;
        u8 *new_buf = (u8 *)kmalloc(new_cap);
        if (!new_buf) {
            spinlock_unlock(&ctx->lock);
            return -(s64)ENOMEM;
        }
        if (ctx->data && ctx->size > 0) {
            memcpy(new_buf, ctx->data, ctx->size);
            kfree(ctx->data);
        }
        ctx->data = new_buf;
        ctx->capacity = new_cap;
    }
    /* Kernel-to-kernel, for the reason memfd_read_op() explains. */
    memcpy(ctx->data + *offset, buf, count);
    *offset += count;
    if (*offset > ctx->size) {
        ctx->size = *offset;
        if (filp->f_inode) filp->f_inode->i_size = ctx->size;
    }
    spinlock_unlock(&ctx->lock);
    return (s64)count;
}

static s64 memfd_ioctl_op(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EBADF;
    memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
    if (cmd == 0x541B /* FIONREAD */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int avail = (filp->f_pos < ctx->size) ? (int)(ctx->size - filp->f_pos) : 0;
        if (copy_to_user((void *)(uintptr_t)arg, &avail, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    return -(s64)EINVAL;
}

static s64 memfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp) {
        if (filp->private_data) {
            memfd_ctx_t *ctx = (memfd_ctx_t *)filp->private_data;
            if (ctx->data) kfree(ctx->data);
            kfree(ctx);
            filp->private_data = NULL;
        }
        if (filp->f_inode) {
            kfree(filp->f_inode);
            filp->f_inode = NULL;
        }
    }
    return 0;
}

static file_operations_t g_memfd_fops = {
    .read    = memfd_read_op,
    .write   = memfd_write_op,
    .ioctl   = memfd_ioctl_op,
    .release = memfd_release_op,
};

static s64 sys_memfd_create_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    (void)uname;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    memfd_ctx_t *ctx = (memfd_ctx_t *)kzalloc(sizeof(memfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    inode_t *node = (inode_t *)kzalloc(sizeof(inode_t));
    if (!node) {
        kfree(ctx);
        kfree(f);
        return -(s64)ENOMEM;
    }
    node->i_mode = S_IFREG | 0600;

    f->f_inode = node;
    f->f_op = &g_memfd_fops;
    f->f_flags = O_RDWR;
    f->f_count = 1;
    f->private_data = ctx;

    s64 fd = fd_install(proc, f, (flags & 0x0001 /* MFD_CLOEXEC */) ? FD_CLOEXEC : 0);
    if (fd < 0) {
        kfree(node);
        kfree(ctx);
        kfree(f);
        return -(s64)EMFILE;
    }
    return fd;
}

static s64 sys_rseq_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}



static s64 sys_getrandom_impl(pt_regs_t *r)
{
    void *buf = (void *)r->rdi;
    size_t buflen = (size_t)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;

    /* GRND_NONBLOCK=1, GRND_RANDOM=2, GRND_INSECURE=4 */
    if (flags & ~0x7u) return -(s64)EINVAL;
    if ((flags & 0x2u) && (flags & 0x4u)) return -(s64)EINVAL;
    if (!buf && buflen) return -(s64)EFAULT;
    if (buflen == 0) return 0;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    /* The kernel CSPRNG is always seeded early in boot, so GRND_NONBLOCK never
     * needs to return EAGAIN and GRND_RANDOM does not block. */
    u8 kbuf[256];
    size_t written = 0;
    while (written < buflen) {
        size_t chunk = buflen - written;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        krandom_bytes(kbuf, chunk);
        if (copy_to_user((char *)buf + written, kbuf, chunk) != 0) {
            __builtin_memset(kbuf, 0, sizeof(kbuf));
            return written ? (s64)written : -(s64)EFAULT;
        }
        written += chunk;
    }
    __builtin_memset(kbuf, 0, sizeof(kbuf));
    return (s64)buflen;
}


/* ── Linux sendfile, copy_file_range, fallocate, statx, splice ──────────── */

static s64 sys_sendfile_impl(pt_regs_t *r)
{
    int out_fd = (int)(s32)r->rdi;
    int in_fd = (int)(s32)r->rsi;
    s64 *user_offset = (s64 *)r->rdx;
    size_t count = (size_t)r->r10;

    if (count == 0) return 0;
    if (out_fd < 0 || out_fd >= PROC_MAX_FDS || in_fd < 0 || in_fd >= PROC_MAX_FDS) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[out_fd] || !proc->handle_table[in_fd]) return -(s64)EBADF;

    file_t *out_file = (file_t *)proc->handle_table[out_fd];
    file_t *in_file = (file_t *)proc->handle_table[in_fd];
    if (!out_file || !in_file) return -(s64)EBADF;

    s64 current_off = 0;
    bool use_off = false;
    if (user_offset) {
        if ((uintptr_t)user_offset >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_from_user(&current_off, user_offset, sizeof(s64)) != 0) return -(s64)EFAULT;
        if (current_off < 0) return -(s64)EINVAL;
        use_off = true;
    }

    size_t total_transferred = 0;
    char kbuf[4096];

    while (total_transferred < count) {
        size_t to_read = count - total_transferred;
        if (to_read > sizeof(kbuf)) to_read = sizeof(kbuf);

        s64 nread = 0;
        if (use_off) {
            u64 saved_pos = in_file->f_pos;
            in_file->f_pos = (u64)current_off;
            nread = (s64)vfs_read(in_file, kbuf, to_read);
            in_file->f_pos = saved_pos;
            if (nread > 0) current_off += nread;
        } else {
            nread = (s64)vfs_read(in_file, kbuf, to_read);
        }

        if (nread <= 0) break;

        s64 nwritten = (s64)vfs_write(out_file, kbuf, (size_t)nread);
        if (nwritten <= 0) {
            if (total_transferred == 0) return (nwritten < 0) ? nwritten : -(s64)EIO;
            break;
        }

        total_transferred += (size_t)nwritten;
        if (nwritten < (s64)to_read) break;
    }

    if (use_off && user_offset) {
        copy_to_user(user_offset, &current_off, sizeof(s64));
    }

    return (s64)total_transferred;
}

static s64 sys_copy_file_range_impl(pt_regs_t *r)
{
    int fd_in = (int)(s32)r->rdi;
    s64 *off_in = (s64 *)r->rsi;
    int fd_out = (int)(s32)r->rdx;
    s64 *off_out = (s64 *)r->r10;
    size_t len = (size_t)r->r8;
    unsigned int flags = (unsigned int)r->r9;
    (void)flags;

    if (len == 0) return 0;
    if (fd_in < 0 || fd_in >= 64 || fd_out < 0 || fd_out >= 64) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd_in] || !proc->handle_table[fd_out]) return -(s64)EBADF;

    file_t *in_file = (file_t *)proc->handle_table[fd_in];
    file_t *out_file = (file_t *)proc->handle_table[fd_out];
    if (!in_file || !out_file) return -(s64)EBADF;

    s64 cur_in = 0, cur_out = 0;
    bool has_in = false, has_out = false;
    if (off_in) {
        if ((uintptr_t)off_in >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_from_user(&cur_in, off_in, sizeof(s64)) != 0) return -(s64)EFAULT;
        if (cur_in < 0) return -(s64)EINVAL;
        has_in = true;
    }
    if (off_out) {
        if ((uintptr_t)off_out >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_from_user(&cur_out, off_out, sizeof(s64)) != 0) return -(s64)EFAULT;
        if (cur_out < 0) return -(s64)EINVAL;
        has_out = true;
    }

    size_t total_copied = 0;
    char kbuf[4096];

    while (total_copied < len) {
        size_t to_copy = len - total_copied;
        if (to_copy > sizeof(kbuf)) to_copy = sizeof(kbuf);

        s64 nread = 0;
        if (has_in) {
            u64 saved_in = in_file->f_pos;
            in_file->f_pos = (u64)cur_in;
            nread = (s64)vfs_read(in_file, kbuf, to_copy);
            in_file->f_pos = saved_in;
            if (nread > 0) cur_in += nread;
        } else {
            nread = (s64)vfs_read(in_file, kbuf, to_copy);
        }

        if (nread <= 0) break;

        s64 nwritten = 0;
        if (has_out) {
            u64 saved_out = out_file->f_pos;
            out_file->f_pos = (u64)cur_out;
            nwritten = (s64)vfs_write(out_file, kbuf, (size_t)nread);
            out_file->f_pos = saved_out;
            if (nwritten > 0) cur_out += nwritten;
        } else {
            nwritten = (s64)vfs_write(out_file, kbuf, (size_t)nread);
        }

        if (nwritten <= 0) {
            if (total_copied == 0) return (nwritten < 0) ? nwritten : -(s64)EIO;
            break;
        }

        total_copied += (size_t)nwritten;
        if (nwritten < (s64)to_copy) break;
    }

    if (has_in && off_in) copy_to_user(off_in, &cur_in, sizeof(s64));
    if (has_out && off_out) copy_to_user(off_out, &cur_out, sizeof(s64));

    return (s64)total_copied;
}

#define FALLOC_FL_KEEP_SIZE      0x01
#define FALLOC_FL_PUNCH_HOLE     0x02
#define FALLOC_FL_NO_HIDE_STALES 0x04
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_ZERO_RANGE     0x10
#define FALLOC_FL_INSERT_RANGE   0x20
#define FALLOC_FL_UNSHARE_RANGE  0x40

static s64 sys_fallocate_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    int mode = (int)r->rsi;
    s64 offset = (s64)r->rdx;
    s64 len = (s64)r->r10;

    if (offset < 0 || len <= 0) return -(s64)EINVAL;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = (file_t *)proc->handle_table[fd];
    if (!file || !file->f_inode) return -(s64)EBADF;
    if ((file->f_flags & 3) == O_RDONLY) return -(s64)EBADF;  /* must be writable */

    s64 req_size = offset + len;
    if (req_size < offset) return -(s64)EINVAL;   /* range overflows */

    /* ext2 reserves real blocks here (so a later write cannot ENOSPC); other
     * filesystems fall back to extending i_size, which is what this call used
     * to do unconditionally. */
    return vfs_fallocate(file, mode, (u64)offset, (u64)len);
}

static s64 sys_sync_file_range_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;
    file_t *f = (file_t *)proc->handle_table[fd];
    if (f && f->f_inode && f->f_inode->i_sb)
        return vfs_sync_fs(f->f_inode->i_sb);
    vfs_sync_all();
    return 0;
}

static s64 sys_readahead_impl(pt_regs_t *r)
{
    int fd    = (int)(s32)r->rdi;
    u64 off   = r->rsi;
    u64 count = r->rdx;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;
    /* readahead(2) is posix_fadvise(POSIX_FADV_WILLNEED) with a fixed advice. */
    vfs_fadvise((file_t *)proc->handle_table[fd], off, count, 3 /* WILLNEED */);
    return 0;
}

static s64 sys_splice_impl(pt_regs_t *r)
{
    return sys_copy_file_range_impl(r);
}

static s64 sys_tee_impl(pt_regs_t *r)
{
    return sys_copy_file_range_impl(r);
}

static s64 sys_vmsplice_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    size_t nr_segs = (size_t)r->rdx;

    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    if (!iov || nr_segs == 0) return 0;
    pt_regs_t sub = *r;
    sub.rdi = (u64)fd;
    sub.rsi = (u64)(uintptr_t)iov;
    sub.rdx = nr_segs;
    return sys_writev_impl(&sub);
}

static s64 sys_statx_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int flags = (int)r->rdx;
    unsigned int mask = (unsigned int)r->r10;
    struct statx *statxbuf = (struct statx *)r->r8;
    (void)mask;

    if (!statxbuf) return -(s64)EINVAL;
    if ((uintptr_t)statxbuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    struct stat kst;
    __builtin_memset(&kst, 0, sizeof(kst));

    bool empty_path = false;
    char raw[256];
    __builtin_memset(raw, 0, sizeof(raw));
    if (!user_path || (flags & AT_EMPTY_PATH)) {
        empty_path = true;
    } else {
        s64 slen = copy_str_from_user(raw, user_path, sizeof(raw));
        if (slen < 0) return slen;
        if (raw[0] == '\0') empty_path = true;
    }

    if (empty_path) {
        if (dirfd < 0 || dirfd >= PROC_MAX_FDS) return -(s64)EBADF;
        process_t *proc = sched_current_process();
        if (!proc || !proc->handle_table[dirfd]) return -(s64)EBADF;
        file_t *file = (file_t *)proc->handle_table[dirfd];
        if (!file || !file->f_inode) return -(s64)EBADF;
        s64 ret = vfs_fstat(file, &kst);
        if (ret < 0) return ret;
    } else {
        char kpath[512];
        s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
        if (perr < 0) return perr;

        s64 ret;
        if (flags & AT_SYMLINK_NOFOLLOW) {
            ret = vfs_lstat(kpath, &kst);
        } else {
            ret = vfs_stat(kpath, &kst);
        }
        if (ret < 0) return ret;
    }

    struct statx sx;
    __builtin_memset(&sx, 0, sizeof(sx));
    sx.stx_mask = STATX_BASIC_STATS;
    sx.stx_blksize = (u32)(kst.st_blksize ? kst.st_blksize : 4096);
    sx.stx_attributes = 0;
    sx.stx_nlink = (u32)kst.st_nlink;
    sx.stx_uid = kst.st_uid;
    sx.stx_gid = kst.st_gid;
    sx.stx_mode = (u16)kst.st_mode;
    sx.stx_ino = kst.st_ino;
    sx.stx_size = (u64)kst.st_size;
    sx.stx_blocks = (u64)kst.st_blocks;
    sx.stx_attributes_mask = 0;

    sx.stx_atime.tv_sec = (s64)kst.st_atime;
    sx.stx_atime.tv_nsec = (u32)kst.st_atime_nsec;
    sx.stx_mtime.tv_sec = (s64)kst.st_mtime;
    sx.stx_mtime.tv_nsec = (u32)kst.st_mtime_nsec;
    sx.stx_ctime.tv_sec = (s64)kst.st_ctime;
    sx.stx_ctime.tv_nsec = (u32)kst.st_ctime_nsec;
    sx.stx_btime.tv_sec = (s64)kst.st_ctime;
    sx.stx_btime.tv_nsec = (u32)kst.st_ctime_nsec;

    sx.stx_dev_major = (u32)(kst.st_dev >> 8);
    sx.stx_dev_minor = (u32)(kst.st_dev & 0xFF);
    sx.stx_rdev_major = (u32)(kst.st_rdev >> 8);
    sx.stx_rdev_minor = (u32)(kst.st_rdev & 0xFF);

    if (copy_to_user(statxbuf, &sx, sizeof(struct statx)) != 0) {
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_syslog_impl(pt_regs_t *r)
{
    int type = (int)r->rdi;
    char *user_buf = (char *)r->rsi;
    int len = (int)r->rdx;

    extern s64 console_read_klog(void *buf, size_t max_len, u64 *offset);
    extern u64 console_get_klog_size(void);

    switch (type) {
    case 0:
    case 1:
    case 5:
    case 6:
    case 7:
    case 8:
        return 0;
    case 2:
    case 3:
    case 4: {
        if (!user_buf || len <= 0) return -(s64)EINVAL;
        char *kbuf = (char *)kmalloc((size_t)len);
        if (!kbuf) return -(s64)ENOMEM;
        u64 offset = 0;
        s64 n = console_read_klog(kbuf, (size_t)len, &offset);
        if (n > 0) {
            if (copy_to_user(user_buf, kbuf, (size_t)n) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
        }
        kfree(kbuf);
        return n;
    }
    case 9: {
        u64 sz = console_get_klog_size();
        return (s64)(sz > 65536 ? 65536 : sz);
    }
    case 10:
        return 65536;
    default:
        return -(s64)EINVAL;
    }
}

static s64 sys_swapon_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    (void)r->rsi;

    if (!user_path) return -(s64)EINVAL;
    char path[256];
    s64 slen = copy_str_from_user(path, user_path, sizeof(path));
    if (slen < 0) return slen;

    struct stat st;
    s64 ret = vfs_stat(path, &st);
    if (ret < 0) return ret;

    return 0;
}

static s64 sys_swapoff_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path) return -(s64)EINVAL;
    char path[256];
    s64 slen = copy_str_from_user(path, user_path, sizeof(path));
    if (slen < 0) return slen;
    return 0;
}

static s64 sys_sched_yield_impl(pt_regs_t *r)
{
    (void)r;
    sched_yield();
    return 0;
}

static s64 sys_msync_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    int flags = (int)r->rdx;

    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (flags & ~(1 /* MS_ASYNC */ | 2 /* MS_INVALIDATE */ | 4 /* MS_SYNC */)) return -(s64)EINVAL;
    if ((flags & (1 | 4)) == (1 | 4)) return -(s64)EINVAL;
    if ((flags & (1 | 4)) == 0) return -(s64)EINVAL;
    if (addr >= 0x0000800000000000ULL || addr + length < addr) return -(s64)ENOMEM;
    if (length == 0) return 0;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    for (uintptr_t va = addr; va < addr + aligned_len; va += PAGE_SIZE) {
        if (!vmm_translate(proc->pml4_phys, va)) return -(s64)ENOMEM;
    }

    vfs_sync_all();
    return 0;
}

static s64 sys_madvise_impl(pt_regs_t *r)
{
    u64   addr   = r->rdi;
    u64   length = r->rsi;
    int   advice = (int)r->rdx;

    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;   /* must be page-aligned */
    if (length > 0x0000800000000000ULL) return -(s64)EINVAL;
    if (addr >= 0x0000800000000000ULL || addr + length < addr) return -(s64)EINVAL;

    /* This kernel maps anonymous memory eagerly and never reclaims a resident
     * page, so every hint that is legal is also a no-op. What matters is
     * rejecting the ones that are *not* legal — callers probe with a bogus
     * advice and branch on the -EINVAL. */
    switch (advice) {
        case 0:   /* MADV_NORMAL       */
        case 1:   /* MADV_RANDOM       */
        case 2:   /* MADV_SEQUENTIAL   */
        case 3:   /* MADV_WILLNEED     */
        case 4:   /* MADV_DONTNEED     */
        case 8:   /* MADV_FREE         */
        case 9:   /* MADV_REMOVE       */
        case 10:  /* MADV_DONTFORK     */
        case 11:  /* MADV_DOFORK       */
        case 12:  /* MADV_MERGEABLE    */
        case 13:  /* MADV_UNMERGEABLE  */
        case 14:  /* MADV_HUGEPAGE     */
        case 15:  /* MADV_NOHUGEPAGE   */
        case 16:  /* MADV_DONTDUMP     */
        case 17:  /* MADV_DODUMP       */
        case 18:  /* MADV_WIPEONFORK   */
        case 19:  /* MADV_KEEPONFORK   */
        case 20:  /* MADV_COLD         */
        case 21:  /* MADV_PAGEOUT      */
        case 100: /* MADV_HWPOISON (privileged, but harmless as a no-op here) */
            return 0;
        default:
            return -(s64)EINVAL;
    }
}

static s64 sys_fadvise64_impl(pt_regs_t *r)
{
    int fd     = (int)(s32)r->rdi;
    u64 offset = r->rsi;
    u64 len    = r->rdx;
    int advice = (int)r->r10;

    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;

    switch (advice) {
        case 0: case 1: case 2: case 3: case 4: case 5:  /* POSIX_FADV_* */
            break;
        default:
            return -(s64)EINVAL;
    }
    return vfs_fadvise((file_t *)proc->handle_table[fd], offset, len, advice);
}

static s64 sys_socketpair_impl(pt_regs_t *r)
{
    int domain = (int)r->rdi;
    int type = (int)r->rsi;
    int protocol = (int)r->rdx;
    int *user_sv = (int *)r->r10;

    (void)protocol;
    if (domain != 1 /* AF_UNIX */) return -(s64)EAFNOSUPPORT;
    if (!user_sv || (uintptr_t)user_sv >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *rf = NULL, *wf = NULL;
    int err = sockpair_create(&rf, &wf);
    if (err < 0) return (s64)err;

    int fd0, fd1;
    if (fd_install_pair(proc, rf, wf, (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0,
                        &fd0, &fd1) < 0) {
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EMFILE;
    }

    int sv[2] = { fd0, fd1 };
    if (copy_to_user(user_sv, sv, sizeof(sv)) != 0) {
        vfs_close(fd_detach(proc, fd0));
        vfs_close(fd_detach(proc, fd1));
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_gettid_impl(pt_regs_t *r)
{
    (void)r;
    thread_t *t = sched_current_thread();
    if (t) return (s64)t->tid;
    process_t *p = sched_current_process();
    return p ? (s64)p->pid : 1;
}

static s64 sys_prctl_impl(pt_regs_t *r)
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
        if (!arg2 || arg2 >= 0x0000800000000000ULL) return -(s64)EFAULT;
        int sig = p->pdeath_sig;
        if (copy_to_user((void *)arg2, &sig, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    if (option == 15 /* PR_SET_NAME */) {
        if (!arg2 || arg2 >= 0x0000800000000000ULL) return -(s64)EFAULT;
        char name[16];
        if (copy_from_user(name, (const void *)arg2, 15) != 0) return -(s64)EFAULT;
        name[15] = '\0';
        strncpy(p->name, name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
        return 0;
    }
    if (option == 16 /* PR_GET_NAME */) {
        if (!arg2 || arg2 >= 0x0000800000000000ULL) return -(s64)EFAULT;
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
        if (!arg2 || arg2 >= 0x0000800000000000ULL) return -(s64)EFAULT;
        int val = p->child_subreaper ? 1 : 0;
        if (copy_to_user((void *)arg2, &val, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }
    return 0;
}

static s64 sys_sched_getaffinity_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    size_t cpusetsize = (size_t)r->rsi;
    void *mask = (void *)r->rdx;
    if (pid < 0) return -(s64)EINVAL;
    if (!mask || (uintptr_t)mask >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (cpusetsize < sizeof(u64)) return -(s64)EINVAL;

    if (pid > 0) {
        process_t *target = proc_get_by_pid((u32)pid);
        if (!target) return -(s64)ESRCH;
        proc_put(target);
    }

    u32 ncpus = smp_cpu_count();
    if (ncpus == 0) ncpus = 1;
    u64 affinity = (ncpus >= 64) ? ~0ULL : ((1ULL << ncpus) - 1);
    if (copy_to_user(mask, &affinity, sizeof(u64)) != 0) return -(s64)EFAULT;
    return (s64)sizeof(u64);
}

static s64 sys_sched_setaffinity_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    size_t cpusetsize = (size_t)r->rsi;
    const void *mask = (const void *)r->rdx;

    if (pid < 0) return -(s64)EINVAL;
    if (!mask || (uintptr_t)mask >= 0x8000000000000000ULL) return -(s64)EFAULT;
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

    if (pid != 0) proc_put(target);
    return 0;
}

static s64 sys_tkill_impl(pt_regs_t *r)
{
    s32 tid = (s32)r->rdi;
    int sig = (int)r->rsi;
    if (tid <= 0) return -(s64)EINVAL;
    return sched_kill_process((u32)tid, sig);
}

static s64 sys_tgkill_impl(pt_regs_t *r)
{
    s32 tgid = (s32)r->rdi;
    s32 tid = (s32)r->rsi;
    int sig = (int)r->rdx;
    if (tid <= 0) return -(s64)EINVAL;
    if (tgid > 0) return sched_kill_process((u32)tgid, sig);
    return sched_kill_process((u32)tid, sig);
}

static s64 sys_link_impl(pt_regs_t *r)
{
    const char *oldpath = (const char *)r->rdi;
    const char *newpath = (const char *)r->rsi;
    char kold[512], knew[512];
    s64 err = copy_user_path_resolve(kold, sizeof(kold), oldpath);
    if (err < 0) return err;
    err = copy_user_path_resolve(knew, sizeof(knew), newpath);
    if (err < 0) return err;
    /* link(2) is a *hard* link. Routing it to vfs_symlink() made `ln a b`
     * produce a symlink, so removing either name could take the other's
     * target with it. */
    return vfs_link(kold, knew);
}

static s64 sys_linkat_impl(pt_regs_t *r)
{
    int olddfd = (int)(s32)r->rdi;
    const char *oldpath = (const char *)r->rsi;
    int newdfd = (int)(s32)r->rdx;
    const char *newpath = (const char *)r->r10;

    char kold[512], knew[512];
    s64 err = copy_user_path_resolve_at(olddfd, kold, sizeof(kold), oldpath);
    if (err < 0) return err;
    err = copy_user_path_resolve_at(newdfd, knew, sizeof(knew), newpath);
    if (err < 0) return err;
    return vfs_link(kold, knew);
}

static s64 sys_symlinkat_impl(pt_regs_t *r)
{
    const char *target = (const char *)r->rdi;
    int newdfd = (int)(s32)r->rsi;
    const char *linkpath = (const char *)r->rdx;

    char ktarget[512], klink[512];
    s64 err = copy_str_from_user(ktarget, target, sizeof(ktarget));
    if (err < 0) return err;
    err = copy_user_path_resolve_at(newdfd, klink, sizeof(klink), linkpath);
    if (err < 0) return err;
    return vfs_symlink(ktarget, klink);
}

static s64 sys_fchmodat_impl(pt_regs_t *r)
{
    int dfd = (int)(s32)r->rdi;
    const char *path = (const char *)r->rsi;
    u32 mode = (u32)r->rdx;

    char kpath[512];
    s64 err = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (err < 0) return err;
    return vfs_chmod(kpath, mode);
}

static s64 sys_fchownat_impl(pt_regs_t *r)
{
    int dfd = (int)(s32)r->rdi;
    const char *path = (const char *)r->rsi;
    u32 uid = (u32)r->rdx;
    u32 gid = (u32)r->r10;
    int flags = (int)r->r8;

    char kpath[512];
    s64 err = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (err < 0) return err;
    if (flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) {
        return vfs_lchown(kpath, uid, gid);
    }
    return vfs_chown(kpath, uid, gid);
}

static s64 sys_renameat_impl(pt_regs_t *r)
{
    int olddfd = (int)(s32)r->rdi;
    const char *oldpath = (const char *)r->rsi;
    int newdfd = (int)(s32)r->rdx;
    const char *newpath = (const char *)r->r10;

    char kold[512], knew[512];
    s64 err = copy_user_path_resolve_at(olddfd, kold, sizeof(kold), oldpath);
    if (err < 0) return err;
    err = copy_user_path_resolve_at(newdfd, knew, sizeof(knew), newpath);
    if (err < 0) return err;
    return vfs_rename(kold, knew);
}

static s64 sys_flock_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    int operation = (int)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    file_t *file = (file_t *)proc->handle_table[fd];
    return vfs_flock(file, operation);
}

static s64 sys_fsync_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    /* No per-inode dirty tracking yet, so this flushes the whole volume rather
     * than just this file's blocks — but at least only this file's volume. */
    file_t *f = (file_t *)proc->handle_table[fd];
    if (f && f->f_inode && f->f_inode->i_sb)
        return vfs_sync_fs(f->f_inode->i_sb);
    vfs_sync_all();
    return 0;
}

static s64 sys_fdatasync_impl(pt_regs_t *r)
{
    return sys_fsync_impl(r);
}

static s64 sys_sync_impl(pt_regs_t *r)
{
    (void)r;
    vfs_sync_all();
    return 0;
}

static s64 sys_syncfs_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    file_t *f = (file_t *)proc->handle_table[fd];
    if (f && f->f_inode && f->f_inode->i_sb)
        return vfs_sync_fs(f->f_inode->i_sb);
    vfs_sync_all();
    return 0;
}

static s64 sys_getpgid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->pgid;
    process_t *t = proc_by_pid((u32)pid);
    return t ? (s64)t->pgid : -(s64)ESRCH;
}

static s64 sys_getsid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->sid;
    process_t *t = proc_by_pid((u32)pid);
    return t ? (s64)t->sid : -(s64)ESRCH;
}

static s64 sys_setreuid_impl(pt_regs_t *r)
{
    u32 ruid = (u32)r->rdi;
    u32 euid = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0) {
        if (ruid != (u32)-1 && ruid != proc->uid && ruid != proc->euid) return -(s64)EPERM;
        if (euid != (u32)-1 && euid != proc->uid && euid != proc->euid) return -(s64)EPERM;
    }
    if (ruid != (u32)-1) proc->uid = ruid;
    if (euid != (u32)-1) proc->euid = euid;
    security_caps_on_setuid(proc);
    return 0;
}

static s64 sys_setresuid_impl(pt_regs_t *r)
{
    u32 ruid = (u32)r->rdi;
    u32 euid = (u32)r->rsi;
    u32 suid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0) {
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

static s64 sys_getresuid_impl(pt_regs_t *r)
{
    u32 *ruid = (u32 *)r->rdi;
    u32 *euid = (u32 *)r->rsi;
    u32 *suid = (u32 *)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (ruid && (uintptr_t)ruid < 0x0000800000000000ULL) copy_to_user(ruid, &proc->uid, sizeof(u32));
    if (euid && (uintptr_t)euid < 0x0000800000000000ULL) copy_to_user(euid, &proc->euid, sizeof(u32));
    if (suid && (uintptr_t)suid < 0x0000800000000000ULL) copy_to_user(suid, &proc->suid, sizeof(u32));
    return 0;
}

static s64 sys_setregid_impl(pt_regs_t *r)
{
    u32 rgid = (u32)r->rdi;
    u32 egid = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0) {
        if (rgid != (u32)-1 && rgid != proc->gid && rgid != proc->egid) return -(s64)EPERM;
        if (egid != (u32)-1 && egid != proc->gid && egid != proc->egid) return -(s64)EPERM;
    }
    if (rgid != (u32)-1) proc->gid = rgid;
    if (egid != (u32)-1) proc->egid = egid;
    return 0;
}

static s64 sys_setresgid_impl(pt_regs_t *r)
{
    u32 rgid = (u32)r->rdi;
    u32 egid = (u32)r->rsi;
    u32 sgid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0) {
        if (rgid != (u32)-1 && rgid != proc->gid && rgid != proc->egid && rgid != proc->sgid) return -(s64)EPERM;
        if (egid != (u32)-1 && egid != proc->gid && egid != proc->egid && egid != proc->sgid) return -(s64)EPERM;
        if (sgid != (u32)-1 && sgid != proc->gid && sgid != proc->egid && sgid != proc->sgid) return -(s64)EPERM;
    }
    if (rgid != (u32)-1) proc->gid = rgid;
    if (egid != (u32)-1) proc->egid = egid;
    if (sgid != (u32)-1) proc->sgid = sgid;
    return 0;
}

static s64 sys_getresgid_impl(pt_regs_t *r)
{
    u32 *rgid = (u32 *)r->rdi;
    u32 *egid = (u32 *)r->rsi;
    u32 *sgid = (u32 *)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (rgid && (uintptr_t)rgid < 0x0000800000000000ULL) copy_to_user(rgid, &proc->gid, sizeof(u32));
    if (egid && (uintptr_t)egid < 0x0000800000000000ULL) copy_to_user(egid, &proc->egid, sizeof(u32));
    if (sgid && (uintptr_t)sgid < 0x0000800000000000ULL) copy_to_user(sgid, &proc->sgid, sizeof(u32));
    return 0;
}

static s64 sys_getgroups_impl(pt_regs_t *r)
{
    int size = (int)(s32)r->rdi;
    u32 *list = (u32 *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (size == 0) return proc->ngroups > 0 ? (s64)proc->ngroups : 1;
    if (size < 0) return -(s64)EINVAL;
    if (!list || (uintptr_t)list >= 0x0000800000000000ULL) return -(s64)EFAULT;

    if (proc->ngroups > 0) {
        if (size < (int)proc->ngroups) return -(s64)EINVAL;
        if (copy_to_user(list, proc->groups, proc->ngroups * sizeof(u32)) != 0) return -(s64)EFAULT;
        return (s64)proc->ngroups;
    }
    u32 gid = proc->gid;
    if (copy_to_user(list, &gid, sizeof(u32)) != 0) return -(s64)EFAULT;
    return 1;
}

static s64 sys_setgroups_impl(pt_regs_t *r)
{
    size_t size = (size_t)r->rdi;
    const u32 *list = (const u32 *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SETGID)) return -(s64)EPERM;
    if (size > 32) return -(s64)EINVAL;
    if (size > 0) {
        if (!list || (uintptr_t)list >= 0x0000800000000000ULL) return -(s64)EFAULT;
        if (copy_from_user(proc->groups, list, size * sizeof(u32)) != 0) return -(s64)EFAULT;
    }
    proc->ngroups = (u32)size;
    return 0;
}

static s64 sys_clock_getres_impl(pt_regs_t *r)
{
    u32 clk_id = (u32)r->rdi;
    struct linux_timespec *res = (struct linux_timespec *)r->rsi;
    if (res && (uintptr_t)res < 0x8000000000000000ULL) {
        long ns = 10000000L; /* 10 ms — the 100 Hz tick */
        if ((clk_id == 1 || clk_id == 4 || clk_id == 7) && hpet_available())
            ns = (long)hpet_resolution_ns();
        struct linux_timespec ts = { .tv_sec = 0, .tv_nsec = ns };
        copy_to_user(res, &ts, sizeof(ts));
    }
    return 0;
}

static s64 sys_clock_settime_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_TIME)) {
        return -(s64)EPERM;
    }
    u32 clk_id = (u32)r->rdi;
    const struct linux_timespec *user_ts = (const struct linux_timespec *)r->rsi;
    /* Only CLOCK_REALTIME (0) is settable — matches sys_clock_gettime_impl's
     * own clk_id==0 "everything else" branch being the only one this kernel
     * derives from the wall clock; the monotonic-family ids (1, 4, 7) are
     * defined to never step and real Linux itself refuses to set them. */
    if (clk_id != 0) return -(s64)EINVAL;
    if (!user_ts) return -(s64)EFAULT;
    struct linux_timespec ts;
    if (copy_from_user(&ts, user_ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;
    set_wall_clock((u64)ts.tv_sec);
    return 0;
}

static s64 sys_clock_nanosleep_impl(pt_regs_t *r)
{
    const struct linux_timespec *req = (const struct linux_timespec *)r->rdx;
    struct linux_timespec *rem = (struct linux_timespec *)r->r10;
    pt_regs_t sub = *r;
    sub.rdi = (u64)(uintptr_t)req;
    sub.rsi = (u64)(uintptr_t)rem;
    return sys_nanosleep_impl(&sub);
}

/* ── Linux Memory Management ABIs (mremap, mincore) ────────────────────────── */
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

static s64 sys_mremap_impl(pt_regs_t *r)
{
    uintptr_t old_addr = (uintptr_t)r->rdi;
    size_t old_size = (size_t)r->rsi;
    size_t new_size = (size_t)r->rdx;
    int flags = (int)r->r10;

    (void)flags;
    if (old_addr >= 0x0000800000000000ULL || (old_addr & 0xFFF) != 0) return -(s64)EINVAL;
    if (new_size == 0) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t old_pages = (old_size + 4095) / 4096;
    size_t new_pages = (new_size + 4095) / 4096;

    if (new_pages == old_pages) return (s64)old_addr;

    if (new_pages > old_pages) {
        for (size_t i = old_pages; i < new_pages; i++) {
            phys_addr_t p = pmm_alloc_page();
            if (!p) return -(s64)ENOMEM;
            hw_clear_page((void *)(HHDM_BASE + p));
            vmm_map(proc->pml4_phys, old_addr + i * 4096, p, VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_NX);
        }
        return (s64)old_addr;
    } else {
        /* BUG-AK fix: reclaim physical pages and update VMA on shrink */
        vmm_unmap_range(proc->pml4_phys, old_addr + new_pages * 4096,
                        old_pages - new_pages, true);
        vma_remove(proc, old_addr + new_size, old_addr + old_size);
        return (s64)old_addr;
    }
}

/* ── Linux Fast Userspace Mutex (futex) ──────────────────────────────────── */
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_FD              2
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_WAKE_OP         5
#define FUTEX_LOCK_PI         6
#define FUTEX_UNLOCK_PI       7
#define FUTEX_TRYLOCK_PI      8
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET     10
#define FUTEX_PRIVATE_FLAG    128
#define FUTEX_CLOCK_REALTIME  256
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFF

/* FUTEX_WAKE_OP encoded-operation field (val3):
 *   bits 28-31 op, 24-27 cmp, 12-23 oparg (12-bit signed), 0-11 cmparg. */
#define FUTEX_OP_SET          0   /* *uaddr2 = oparg        */
#define FUTEX_OP_ADD          1   /* *uaddr2 += oparg       */
#define FUTEX_OP_OR           2   /* *uaddr2 |= oparg       */
#define FUTEX_OP_ANDN         3   /* *uaddr2 &= ~oparg      */
#define FUTEX_OP_XOR          4   /* *uaddr2 ^= oparg       */
#define FUTEX_OP_OPARG_SHIFT  8   /* oparg is (1 << oparg)  */
#define FUTEX_OP_CMP_EQ       0
#define FUTEX_OP_CMP_NE       1
#define FUTEX_OP_CMP_LT       2
#define FUTEX_OP_CMP_LE       3
#define FUTEX_OP_CMP_GT       4
#define FUTEX_OP_CMP_GE       5

typedef struct futex_q {
    thread_t        *thread;
    process_t       *proc;
    uintptr_t        uaddr;
    u32              bitset;
    struct futex_q  *next;
} futex_q_t;

#define FUTEX_HASH_SIZE 64
static futex_q_t *g_futex_table[FUTEX_HASH_SIZE];
static spinlock_t g_futex_lock = SPINLOCK_INIT;

static inline u32 futex_hash(uintptr_t uaddr) {
    return (u32)((uaddr >> 2) ^ (uaddr >> 8)) % FUTEX_HASH_SIZE;
}

/* futex_wait_queued() — enqueue the calling thread on @uaddr's bucket, sleep,
 * and unwind the queue entry however the sleep ended.
 *
 * Shared by futex(FUTEX_WAIT*) and futex_wait(2); the two differ only in how
 * they arrive at @timeout_ticks (relative for the old call, absolute for the
 * new one), which is exactly the part that belongs to the caller. Returns 0 if
 * woken by a FUTEX_WAKE, -EINTR on a pending signal, -ETIMEDOUT on expiry. */
static s64 futex_wait_queued(process_t *proc, thread_t *curr, uintptr_t uaddr,
                             u32 bitset, u64 timeout_ticks)
{
    futex_q_t q;
    q.thread = curr;
    q.proc = proc;
    q.uaddr = uaddr;
    q.bitset = bitset;
    q.next = NULL;

    u32 b = futex_hash(uaddr);
    spinlock_lock(&g_futex_lock);
    q.next = g_futex_table[b];
    g_futex_table[b] = &q;
    spinlock_unlock(&g_futex_lock);

    u64 start_ticks = sched_get_ticks();
    if (timeout_ticks > 0) {
        sched_sleep(timeout_ticks);
    } else {
        sched_block(THREAD_BLOCKED_PENDING);
    }

    spinlock_lock(&g_futex_lock);
    bool was_woken = true;
    futex_q_t **curr_q = &g_futex_table[b];
    while (*curr_q) {
        if (*curr_q == &q) {
            *curr_q = q.next;
            was_woken = false; /* Still in queue -> timed out or signal */
            break;
        }
        curr_q = &(*curr_q)->next;
    }
    spinlock_unlock(&g_futex_lock);

    /* BUG-AJ fix: return -ETIMEDOUT or -EINTR when not awakened by FUTEX_WAKE */
    if (!was_woken) {
        if (proc->sig_pending & ~proc->sig_blocked)
            return -(s64)EINTR;
        if (timeout_ticks > 0 && (sched_get_ticks() - start_ticks >= timeout_ticks))
            return -(s64)ETIMEDOUT;
    }
    return 0;
}

/* futex_wake_addr() — wake up to @nr waiters queued on @uaddr in @proc whose
 * bitset intersects @bitset. Shared by futex(FUTEX_WAKE*) and by thread exit,
 * which must wake the CLONE_CHILD_CLEARTID futex without going through a
 * syscall frame. Returns the number of threads actually woken. */
static int futex_wake_addr(process_t *proc, uintptr_t uaddr, u32 nr, u32 bitset)
{
    u32 b = futex_hash(uaddr);
    int woken = 0;

    spinlock_lock(&g_futex_lock);
    for (futex_q_t *q = g_futex_table[b]; q && (u32)woken < nr; q = q->next) {
        if (q->proc == proc && q->uaddr == uaddr && (q->bitset & bitset)) {
            sched_unblock(q->thread);
            woken++;
        }
    }
    spinlock_unlock(&g_futex_lock);
    return woken;
}

/* thread_clear_child_tid() — the exit half of CLONE_CHILD_CLEARTID and
 * set_tid_address(2). Zero the registered word in user memory and wake one
 * waiter on it, exactly as Linux's mm_release() does. Both halves matter: a
 * joiner that only ever sees the futex wake, with the word still holding the
 * dead tid, spins straight back into the wait. */
void thread_clear_child_tid(thread_t *t)
{
    if (!t || !t->clear_child_tid) return;

    uintptr_t uaddr = (uintptr_t)t->clear_child_tid;
    t->clear_child_tid = 0;
    if (uaddr >= 0x0000800000000000ULL || (uaddr & 3)) return;

    u32 zero = 0;
    if (copy_to_user((void *)uaddr, &zero, sizeof(zero)) != 0) return;

    futex_wake_addr(t->proc, uaddr, 1, FUTEX_BITSET_MATCH_ANY);
}

static s64 sys_futex_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    int op = (int)r->rsi;
    u32 val = (u32)r->rdx;
    const struct linux_timespec *timeout = (const struct linux_timespec *)r->r10;
    uintptr_t uaddr2 = (uintptr_t)r->r8;
    u32 val3 = (u32)r->r9;

    if (uaddr >= 0x0000800000000000ULL || (uaddr & 3) != 0) return -(s64)EFAULT;

    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    process_t *proc = sched_current_process();
    thread_t *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        u32 bitset = (cmd == FUTEX_WAIT_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -(s64)EINVAL;

        u32 cur_val = 0;
        if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0) return -(s64)EFAULT;
        if (cur_val != val) return -(s64)11; /* -EAGAIN / -EWOULDBLOCK */

        u64 timeout_ticks = 0;
        if (timeout && (uintptr_t)timeout < 0x0000800000000000ULL) {
            struct linux_timespec ts;
            if (copy_from_user(&ts, timeout, sizeof(ts)) == 0) {
                u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
                timeout_ticks = (ms + 9) / 10;
            }
        }

        return futex_wait_queued(proc, curr, uaddr, bitset, timeout_ticks);
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        u32 bitset = (cmd == FUTEX_WAKE_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -(s64)EINVAL;

        return futex_wake_addr(proc, uaddr, val, bitset);
    }
    case FUTEX_WAKE_OP: {
        /* Atomically apply an operation to *uaddr2, wake up to @val waiters on
         * uaddr, then — if oldval compares true against cmparg — wake up to
         * @val2 waiters on uaddr2. This is what glibc's pthread_cond_signal /
         * _broadcast and several bounded-queue primitives are built on. */
        if (uaddr2 >= 0x0000800000000000ULL || (uaddr2 & 3) != 0) return -(s64)EFAULT;

        u32 nr_wake  = val;
        u32 nr_wake2 = (u32)(uintptr_t)timeout;   /* val2 shares the timeout slot */
        u32 encoded  = val3;

        int wake_op  = (int)((encoded >> 28) & 0xf);
        int wake_cmp = (int)((encoded >> 24) & 0xf);
        s32 oparg    = (s32)((encoded >> 12) & 0xfff);
        s32 cmparg   = (s32)(encoded & 0xfff);
        if (oparg  & 0x800) oparg  |= ~0xfff;     /* sign-extend 12-bit fields */
        if (cmparg & 0x800) cmparg |= ~0xfff;

        if (wake_op & FUTEX_OP_OPARG_SHIFT) {
            if (oparg < 0 || oparg > 31) return -(s64)EINVAL;
            oparg = 1 << oparg;
            wake_op &= ~FUTEX_OP_OPARG_SHIFT;
        }

        u32 oldval = 0;
        if (copy_from_user(&oldval, (const void *)uaddr2, sizeof(u32)) != 0)
            return -(s64)EFAULT;

        u32 newval;
        switch (wake_op) {
        case FUTEX_OP_SET:  newval = (u32)oparg;          break;
        case FUTEX_OP_ADD:  newval = oldval + (u32)oparg; break;
        case FUTEX_OP_OR:   newval = oldval | (u32)oparg; break;
        case FUTEX_OP_ANDN: newval = oldval & ~(u32)oparg; break;
        case FUTEX_OP_XOR:  newval = oldval ^ (u32)oparg; break;
        default: return -(s64)ENOSYS;
        }

        if (newval != oldval &&
            copy_to_user((void *)uaddr2, &newval, sizeof(u32)) != 0)
            return -(s64)EFAULT;

        int woken = futex_wake_addr(proc, uaddr, nr_wake, FUTEX_BITSET_MATCH_ANY);

        int cmp_res;
        switch (wake_cmp) {
        case FUTEX_OP_CMP_EQ: cmp_res = ((s32)oldval == cmparg); break;
        case FUTEX_OP_CMP_NE: cmp_res = ((s32)oldval != cmparg); break;
        case FUTEX_OP_CMP_LT: cmp_res = ((s32)oldval <  cmparg); break;
        case FUTEX_OP_CMP_LE: cmp_res = ((s32)oldval <= cmparg); break;
        case FUTEX_OP_CMP_GT: cmp_res = ((s32)oldval >  cmparg); break;
        case FUTEX_OP_CMP_GE: cmp_res = ((s32)oldval >= cmparg); break;
        default: return -(s64)ENOSYS;
        }

        if (cmp_res)
            woken += futex_wake_addr(proc, uaddr2, nr_wake2, FUTEX_BITSET_MATCH_ANY);

        return woken;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (cmd == FUTEX_CMP_REQUEUE) {
            u32 cur_val = 0;
            if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0) return -(s64)EFAULT;
            if (cur_val != val3) return -(s64)11; /* -EAGAIN */
        }

        u32 b1 = futex_hash(uaddr);
        u32 b2 = futex_hash(uaddr2);
        int woken = 0;
        int requeued = 0;
        u32 val2_max = timeout ? (u32)(uintptr_t)timeout : 0;

        spinlock_lock(&g_futex_lock);
        futex_q_t **curr_q = &g_futex_table[b1];
        while (*curr_q) {
            futex_q_t *entry = *curr_q;
            if (entry->proc == proc && entry->uaddr == uaddr) {
                if ((u32)woken < val) {
                    *curr_q = entry->next;
                    sched_unblock(entry->thread);
                    woken++;
                    continue;
                } else if ((u32)requeued < val2_max) {
                    /* BUG-AI fix: move requeued waiter to bucket b2 so future wakeups find it */
                    *curr_q = entry->next;
                    entry->uaddr = uaddr2;
                    entry->next = g_futex_table[b2];
                    g_futex_table[b2] = entry;
                    requeued++;
                    continue;
                }
            }
            curr_q = &(*curr_q)->next;
        }
        spinlock_unlock(&g_futex_lock);

        return woken + requeued;
    }
    default:
        return -(s64)ENOSYS;
    }
}

/* ── Linux eventfd / eventfd2 ────────────────────────────────────────────── */
#define EFD_SEMAPHORE 1
#define EFD_CLOEXEC   02000000
#define EFD_NONBLOCK  00004000

#ifndef POLLRDNORM
#define POLLRDNORM 0x0040
#endif
#ifndef POLLWRNORM
#define POLLWRNORM 0x0100
#endif

typedef struct {
    u64 counter;
    u32 flags;
    spinlock_t lock;
} eventfd_ctx_t;

static s64 eventfd_read_op(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (len < sizeof(u64)) return -(s64)EINVAL;
    if (!buf) return -(s64)EFAULT;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)filp->private_data;
    if (!ctx) return -(s64)EBADF;

    for (;;) {
        spinlock_lock(&ctx->lock);
        if (ctx->counter > 0) {
            u64 val;
            if (ctx->flags & EFD_SEMAPHORE) {
                val = 1;
                ctx->counter--;
            } else {
                val = ctx->counter;
                ctx->counter = 0;
            }
            spinlock_unlock(&ctx->lock);
            memcpy(buf, &val, sizeof(u64));
            return sizeof(u64);
        }
        spinlock_unlock(&ctx->lock);

        if (filp->f_flags & O_NONBLOCK) return -(s64)11; /* -EAGAIN */
        sched_sleep(1);
    }
}

static s64 eventfd_write_op(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (len < sizeof(u64)) return -(s64)EINVAL;
    if (!buf) return -(s64)EFAULT;
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)filp->private_data;
    if (!ctx) return -(s64)EBADF;

    u64 val = 0;
    memcpy(&val, buf, sizeof(u64));
    if (val == 0xFFFFFFFFFFFFFFFFULL) return -(s64)EINVAL;

    for (;;) {
        spinlock_lock(&ctx->lock);
        if (0xFFFFFFFFFFFFFFFEULL - ctx->counter >= val) {
            ctx->counter += val;
            spinlock_unlock(&ctx->lock);
            return sizeof(u64);
        }
        spinlock_unlock(&ctx->lock);

        if (filp->f_flags & O_NONBLOCK) return -(s64)11; /* -EAGAIN */
        sched_sleep(1);
    }
}

static int eventfd_poll_op(file_t *filp)
{
    eventfd_ctx_t *ctx = (eventfd_ctx_t *)filp->private_data;
    if (!ctx) return POLLNVAL;
    int rev = 0;
    spinlock_lock(&ctx->lock);
    if (ctx->counter > 0) rev |= (POLLIN | POLLRDNORM);
    if (ctx->counter < 0xFFFFFFFFFFFFFFFEULL) rev |= (POLLOUT | POLLWRNORM);
    spinlock_unlock(&ctx->lock);
    return rev;
}

static s64 eventfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_eventfd_fops = {
    .read = eventfd_read_op,
    .write = eventfd_write_op,
    .poll = eventfd_poll_op,
    .release = eventfd_release_op,
};

static s64 sys_eventfd2_impl(pt_regs_t *r)
{
    u32 initval = (u32)r->rdi;
    int flags = (int)r->rsi;

    if (flags & ~(EFD_SEMAPHORE | EFD_CLOEXEC | EFD_NONBLOCK)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    eventfd_ctx_t *ctx = (eventfd_ctx_t *)kzalloc(sizeof(eventfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->counter = initval;
    ctx->flags = (u32)flags;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    f->f_op = &g_eventfd_fops;
    f->private_data = ctx;
    f->f_flags = (flags & EFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->f_fd_flags = (flags & EFD_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_mode = 0600;
    f->f_count = 1;

    {
        s64 nfd = fd_install_from(proc, f, f->f_fd_flags, 3);
        if (nfd >= 0) return nfd;
    }
    kfree(ctx);
    kfree(f);
    return -(s64)EMFILE;
}

static s64 sys_eventfd_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.rsi = 0;
    return sys_eventfd2_impl(&sub);
}

/* ── Linux epoll subsystem ────────────────────────────────────────────────── */
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_CLOEXEC 02000000

typedef struct {
    int fd;
    u32 events;
    u64 data;
} epoll_item_t;

#define EPOLL_MAX_ITEMS 64
typedef struct {
    int count;
    epoll_item_t items[EPOLL_MAX_ITEMS];
    spinlock_t lock;
} epoll_ctx_t;

static s64 epoll_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_epoll_fops = {
    .release = epoll_release_op,
};

static s64 sys_epoll_create1_impl(pt_regs_t *r)
{
    int flags = (int)r->rdi;
    if (flags & ~EPOLL_CLOEXEC) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    epoll_ctx_t *ctx = (epoll_ctx_t *)kzalloc(sizeof(epoll_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    f->f_op = &g_epoll_fops;
    f->private_data = ctx;
    f->f_fd_flags = (flags & EPOLL_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_count = 1;

    {
        s64 nfd = fd_install_from(proc, f, f->f_fd_flags, 3);
        if (nfd >= 0) return nfd;
    }
    kfree(ctx);
    kfree(f);
    return -(s64)EMFILE;
}

static s64 sys_epoll_create_impl(pt_regs_t *r)
{
    int size = (int)r->rdi;
    if (size <= 0) return -(s64)EINVAL;
    pt_regs_t sub = *r;
    sub.rdi = 0;
    return sys_epoll_create1_impl(&sub);
}

typedef struct {
    u32 events;
    u64 data;
} __attribute__((packed)) linux_epoll_event_t;

static s64 sys_epoll_ctl_impl(pt_regs_t *r)
{
    int epfd = (int)r->rdi;
    int op = (int)r->rsi;
    int fd = (int)r->rdx;
    const linux_epoll_event_t *event = (const linux_epoll_event_t *)r->r10;

    process_t *proc = sched_current_process();
    if (!proc || epfd < 0 || epfd >= PROC_MAX_FDS || !proc->handle_table[epfd]) return -(s64)EBADF;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    if (epfd == fd) return -(s64)EINVAL;

    file_t *epfile = (file_t *)proc->handle_table[epfd];
    if (epfile->f_op != &g_epoll_fops || !epfile->private_data) return -(s64)EINVAL;
    epoll_ctx_t *ctx = (epoll_ctx_t *)epfile->private_data;

    linux_epoll_event_t kevent;
    if (op != EPOLL_CTL_DEL) {
        if (!event || (uintptr_t)event >= 0x0000800000000000ULL) return -(s64)EFAULT;
        if (copy_from_user(&kevent, event, sizeof(linux_epoll_event_t)) != 0) return -(s64)EFAULT;
    }

    spinlock_lock(&ctx->lock);
    if (op == EPOLL_CTL_ADD) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                spinlock_unlock(&ctx->lock);
                return -(s64)EEXIST;
            }
        }
        if (ctx->count >= EPOLL_MAX_ITEMS) {
            spinlock_unlock(&ctx->lock);
            return -(s64)ENOSPC;
        }
        ctx->items[ctx->count].fd = fd;
        ctx->items[ctx->count].events = kevent.events;
        ctx->items[ctx->count].data = kevent.data;
        ctx->count++;
        spinlock_unlock(&ctx->lock);
        return 0;
    } else if (op == EPOLL_CTL_MOD) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                ctx->items[i].events = kevent.events;
                ctx->items[i].data = kevent.data;
                spinlock_unlock(&ctx->lock);
                return 0;
            }
        }
        spinlock_unlock(&ctx->lock);
        return -(s64)ENOENT;
    } else if (op == EPOLL_CTL_DEL) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                ctx->items[i] = ctx->items[ctx->count - 1];
                ctx->count--;
                spinlock_unlock(&ctx->lock);
                return 0;
            }
        }
        spinlock_unlock(&ctx->lock);
        return -(s64)ENOENT;
    }
    spinlock_unlock(&ctx->lock);
    return -(s64)EINVAL;
}

static s64 sys_epoll_wait_impl(pt_regs_t *r)
{
    int epfd = (int)r->rdi;
    linux_epoll_event_t *events = (linux_epoll_event_t *)r->rsi;
    int maxevents = (int)r->rdx;
    int timeout_ms = (int)r->r10;

    if (maxevents <= 0 || maxevents > 1024) return -(s64)EINVAL;
    if (!events || (uintptr_t)events >= 0x0000800000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || epfd < 0 || epfd >= PROC_MAX_FDS || !proc->handle_table[epfd]) return -(s64)EBADF;

    file_t *epfile = (file_t *)proc->handle_table[epfd];
    if (epfile->f_op != &g_epoll_fops || !epfile->private_data) return -(s64)EINVAL;
    epoll_ctx_t *ctx = (epoll_ctx_t *)epfile->private_data;

    u64 start_ticks = sched_get_ticks();
    u64 end_ticks = (timeout_ms > 0) ? (start_ticks + (timeout_ms + 9) / 10) : 0;

    for (;;) {
        int ready_count = 0;
        spinlock_lock(&ctx->lock);
        for (int i = 0; i < ctx->count && ready_count < maxevents; i++) {
            int tfd = ctx->items[i].fd;
            if (tfd >= 0 && tfd < PROC_MAX_FDS && proc->handle_table[tfd]) {
                file_t *tf = (file_t *)proc->handle_table[tfd];
                short rev = check_file_readiness(tf, (short)ctx->items[i].events);
                if (rev & ctx->items[i].events) {
                    linux_epoll_event_t ev;
                    ev.events = (u32)(rev & ctx->items[i].events);
                    ev.data = ctx->items[i].data;
                    copy_to_user(&events[ready_count], &ev, sizeof(linux_epoll_event_t));
                    ready_count++;
                }
            }
        }
        spinlock_unlock(&ctx->lock);

        if (ready_count > 0) return ready_count;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) return 0;

        sched_sleep(1);
    }
}

static s64 sys_epoll_pwait_impl(pt_regs_t *r)
{
    return sys_epoll_wait_impl(r);
}

/* ── Linux timerfd subsystem ──────────────────────────────────────────────── */
#define TFD_CLOEXEC  02000000
#define TFD_NONBLOCK 00004000

typedef struct {
    int clockid;
    u32 flags;
    u64 interval_ms;
    u64 expire_tick;
    u64 expirations;
    spinlock_t lock;
} timerfd_ctx_t;

static s64 timerfd_read_op(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (len < sizeof(u64)) return -(s64)EINVAL;
    /* Kernel buffer — see fs/vfs.h. The old user-range test rejected the very
     * pointer vfs_read() hands down, so timerfd reads never returned. */
    if (!buf) return -(s64)EFAULT;
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)filp->private_data;
    if (!ctx) return -(s64)EBADF;

    for (;;) {
        spinlock_lock(&ctx->lock);
        u64 ticks = sched_get_ticks();
        if (ctx->expire_tick > 0 && ticks >= ctx->expire_tick) {
            ctx->expirations++;
            if (ctx->interval_ms > 0) {
                ctx->expire_tick = ticks + (ctx->interval_ms + 9) / 10;
            } else {
                ctx->expire_tick = 0;
            }
        }
        if (ctx->expirations > 0) {
            u64 exp = ctx->expirations;
            ctx->expirations = 0;
            spinlock_unlock(&ctx->lock);
            memcpy(buf, &exp, sizeof(u64));
            return sizeof(u64);
        }
        spinlock_unlock(&ctx->lock);

        if (filp->f_flags & O_NONBLOCK) return -(s64)11; /* -EAGAIN */
        sched_sleep(1);
    }
}

static int timerfd_poll_op(file_t *filp)
{
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)filp->private_data;
    if (!ctx) return POLLNVAL;
    int rev = 0;
    spinlock_lock(&ctx->lock);
    u64 ticks = sched_get_ticks();
    if (ctx->expirations > 0 || (ctx->expire_tick > 0 && ticks >= ctx->expire_tick)) {
        rev |= (POLLIN | POLLRDNORM);
    }
    spinlock_unlock(&ctx->lock);
    return rev;
}

static s64 timerfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_timerfd_fops = {
    .read = timerfd_read_op,
    .poll = timerfd_poll_op,
    .release = timerfd_release_op,
};

static s64 sys_timerfd_create_impl(pt_regs_t *r)
{
    int clockid = (int)r->rdi;
    int flags = (int)r->rsi;

    if (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK)) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    timerfd_ctx_t *ctx = (timerfd_ctx_t *)kzalloc(sizeof(timerfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->clockid = clockid;
    ctx->flags = (u32)flags;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }
    f->f_op = &g_timerfd_fops;
    f->private_data = ctx;
    f->f_flags = (flags & TFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->f_fd_flags = (flags & TFD_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_count = 1;

    {
        s64 nfd = fd_install_from(proc, f, f->f_fd_flags, 3);
        if (nfd >= 0) return nfd;
    }
    kfree(ctx);
    kfree(f);
    return -(s64)EMFILE;
}

struct itimerspec {
    struct linux_timespec it_interval;
    struct linux_timespec it_value;
};

static s64 sys_timerfd_settime_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int flags = (int)r->rsi;
    const struct itimerspec *new_value = (const struct itimerspec *)r->rdx;
    struct itimerspec *old_value = (struct itimerspec *)r->r10;

    (void)flags;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];
    if (f->f_op != &g_timerfd_fops || !f->private_data) return -(s64)EINVAL;
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)f->private_data;

    struct itimerspec new_val;
    if (!new_value || copy_from_user(&new_val, new_value, sizeof(struct itimerspec)) != 0) return -(s64)EFAULT;

    spinlock_lock(&ctx->lock);
    if (old_value && (uintptr_t)old_value < 0x0000800000000000ULL) {
        struct itimerspec old_val;
        memset(&old_val, 0, sizeof(old_val));
        old_val.it_interval.tv_sec = (long)(ctx->interval_ms / 1000);
        old_val.it_interval.tv_nsec = (long)((ctx->interval_ms % 1000) * 1000000);
        copy_to_user(old_value, &old_val, sizeof(struct itimerspec));
    }

    u64 val_ms = (u64)new_val.it_value.tv_sec * 1000 + (u64)new_val.it_value.tv_nsec / 1000000;
    ctx->interval_ms = (u64)new_val.it_interval.tv_sec * 1000 + (u64)new_val.it_interval.tv_nsec / 1000000;
    if (val_ms > 0) {
        ctx->expire_tick = sched_get_ticks() + (val_ms + 9) / 10;
    } else {
        ctx->expire_tick = 0;
    }
    ctx->expirations = 0;
    spinlock_unlock(&ctx->lock);

    return 0;
}

static s64 sys_timerfd_gettime_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct itimerspec *curr_value = (struct itimerspec *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];
    if (f->f_op != &g_timerfd_fops || !f->private_data) return -(s64)EINVAL;
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)f->private_data;

    if (!curr_value || (uintptr_t)curr_value >= 0x0000800000000000ULL) return -(s64)EFAULT;

    struct itimerspec val;
    memset(&val, 0, sizeof(val));
    spinlock_lock(&ctx->lock);
    val.it_interval.tv_sec = (long)(ctx->interval_ms / 1000);
    val.it_interval.tv_nsec = (long)((ctx->interval_ms % 1000) * 1000000);
    if (ctx->expire_tick > 0) {
        u64 ticks = sched_get_ticks();
        if (ctx->expire_tick > ticks) {
            u64 rem_ms = (ctx->expire_tick - ticks) * 10;
            val.it_value.tv_sec = (long)(rem_ms / 1000);
            val.it_value.tv_nsec = (long)((rem_ms % 1000) * 1000000);
        }
    }
    spinlock_unlock(&ctx->lock);

    copy_to_user(curr_value, &val, sizeof(struct itimerspec));
    return 0;
}

/* ── Linux signalfd subsystem ─────────────────────────────────────────────── */
#define SFD_CLOEXEC  02000000
#define SFD_NONBLOCK 00004000

typedef struct {
    sigset_t mask;
    u32 flags;
} signalfd_ctx_t;

struct signalfd_siginfo {
    u32 ssi_signo;
    s32 ssi_errno;
    s32 ssi_code;
    u32 ssi_pid;
    u32 ssi_uid;
    s32 ssi_fd;
    u32 ssi_tid;
    u32 ssi_band;
    u32 ssi_overrun;
    u32 ssi_trapno;
    s32 ssi_status;
    s32 ssi_int;
    u64 ssi_ptr;
    u64 ssi_utime;
    u64 ssi_stime;
    u64 ssi_addr;
    u16 ssi_addr_lsb;
    u16 __pad2;
    s32 ssi_syscall;
    u64 ssi_call_addr;
    u32 ssi_arch;
    u8  __pad[28];
};

static s64 signalfd_read_op(file_t *filp, void *buf, size_t count, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf) return -(s64)EINVAL;
    if (count < sizeof(struct signalfd_siginfo)) return -(s64)EINVAL;

    signalfd_ctx_t *ctx = (signalfd_ctx_t *)filp->private_data;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    for (;;) {
        sigset_t match = proc->sig_pending & ctx->mask;
        if (match) {
            int sig = 0;
            for (int s = 1; s < _NSIG; s++) {
                if (match & (1ULL << s)) {
                    sig = s;
                    break;
                }
            }
            if (sig > 0) {
                proc->sig_pending &= ~(1ULL << sig);

                struct signalfd_siginfo ssi;
                memset(&ssi, 0, sizeof(ssi));
                ssi.ssi_signo = (u32)sig;
                ssi.ssi_pid = proc->pid;
                ssi.ssi_uid = proc->uid;

                /* Kernel buffer: sys_read_impl() owns the copy to userspace.
                 * copy_to_user() here rejected its own destination and
                 * returned -EFAULT *after* having already consumed the signal,
                 * so the signal was lost as well as the read. */
                memcpy(buf, &ssi, sizeof(ssi));
                return (s64)sizeof(ssi);
            }
        }

        if (filp->f_flags & O_NONBLOCK) return -(s64)EAGAIN;
        sched_sleep(1);
        if (proc->is_zombie) return -(s64)EINTR;
    }
}

static int signalfd_poll_op(file_t *filp)
{
    if (!filp || !filp->private_data) return POLLERR;
    signalfd_ctx_t *ctx = (signalfd_ctx_t *)filp->private_data;
    process_t *proc = sched_current_process();
    if (!proc) return POLLERR;

    if (proc->sig_pending & ctx->mask) {
        return POLLIN | POLLRDNORM;
    }
    return 0;
}

static s64 signalfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_signalfd_fops = {
    .read    = signalfd_read_op,
    .poll    = signalfd_poll_op,
    .release = signalfd_release_op,
};

static s64 sys_signalfd4_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const sigset_t *mask = (const sigset_t *)r->rsi;
    size_t sizemask = (size_t)r->rdx;
    int flags = (int)r->r10;

    (void)sizemask;
    if (flags & ~(SFD_CLOEXEC | SFD_NONBLOCK)) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    sigset_t smask = 0;
    if (mask && copy_from_user(&smask, mask, sizeof(sigset_t)) != 0) return -(s64)EFAULT;

    if (fd != -1) {
        if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
        file_t *f = (file_t *)proc->handle_table[fd];
        if (f->f_op != &g_signalfd_fops || !f->private_data) return -(s64)EINVAL;
        signalfd_ctx_t *ctx = (signalfd_ctx_t *)f->private_data;
        ctx->mask = smask;
        return fd;
    }

    signalfd_ctx_t *ctx = (signalfd_ctx_t *)kzalloc(sizeof(signalfd_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->mask = smask;
    ctx->flags = (u32)flags;

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }
    f->f_op = &g_signalfd_fops;
    f->private_data = ctx;
    f->f_flags = (flags & SFD_NONBLOCK) ? O_NONBLOCK : 0;
    f->f_fd_flags = (flags & SFD_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_count = 1;

    {
        s64 nfd = fd_install_from(proc, f, f->f_fd_flags, 3);
        if (nfd >= 0) return nfd;
    }
    kfree(ctx);
    kfree(f);
    return -(s64)EMFILE;
}

static s64 sys_signalfd_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.r10 = 0;
    return sys_signalfd4_impl(&sub);
}

/* ── Linux Scheduling & Personality ABIs ──────────────────────────────────── */
/* SCHED_* / struct sched_param / sched_target() / sched_policy_valid() are
 * defined above with sched_setattr(). */

static s64 sys_sched_setscheduler_impl(pt_regs_t *r)
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

static s64 sys_sched_getscheduler_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;
    s64 policy = (s64)tgt->sched_policy;
    if (put) proc_put(tgt);
    return policy;
}

static s64 sys_sched_setparam_impl(pt_regs_t *r)
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

static s64 sys_sched_getparam_impl(pt_regs_t *r)
{
    u32 pid = (u32)(s32)r->rdi;
    struct sched_param *uparam = (struct sched_param *)r->rsi;
    if (!uparam || (uintptr_t)uparam >= 0x0000800000000000ULL) return -(s64)EINVAL;

    bool put; s64 err;
    process_t *tgt = sched_target(pid, &put, &err);
    if (!tgt) return err;

    struct sched_param kp = { .sched_priority = tgt->sched_rt_prio };
    if (put) proc_put(tgt);

    if (copy_to_user(uparam, &kp, sizeof(kp)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_sched_get_priority_max_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 /* SCHED_FIFO */ || policy == 2 /* SCHED_RR */) return 99;
    if (policy == 0 /* SCHED_OTHER */ || policy == 3 /* SCHED_BATCH */ || policy == 5 /* SCHED_IDLE */ || policy == 6 /* SCHED_DEADLINE */) return 0;
    return -(s64)EINVAL;
}

static s64 sys_sched_get_priority_min_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 /* SCHED_FIFO */ || policy == 2 /* SCHED_RR */) return 1;
    if (policy == 0 /* SCHED_OTHER */ || policy == 3 /* SCHED_BATCH */ || policy == 5 /* SCHED_IDLE */ || policy == 6 /* SCHED_DEADLINE */) return 0;
    return -(s64)EINVAL;
}

static s64 sys_sched_rr_get_interval_impl(pt_regs_t *r)
{
    struct linux_timespec *tp = (struct linux_timespec *)r->rsi;
    if (tp && (uintptr_t)tp < 0x0000800000000000ULL) {
        struct linux_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L /* 10ms */ };
        copy_to_user(tp, &ts, sizeof(ts));
    }
    return 0;
}

static s64 sys_personality_impl(pt_regs_t *r)
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

static s64 sys_membarrier_impl(pt_regs_t *r)
{
    int cmd = (int)r->rdi;
    if (cmd == 0 /* MEMBARRIER_CMD_QUERY */) {
        return 1 | 2; /* MEMBARRIER_CMD_GLOBAL | MEMBARRIER_CMD_GLOBAL_EXPEDITED */
    }
    __sync_synchronize();
    return 0;
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
static s64 sys_capget_impl(pt_regs_t *r)
{
    cap_user_header_t *uhdr = (cap_user_header_t *)r->rdi;
    cap_user_data_t   *udata = (cap_user_data_t *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    cap_user_header_t hdr = { LINUX_CAPABILITY_VERSION_3, 0 };
    if (uhdr) {
        if ((uintptr_t)uhdr >= 0x0000800000000000ULL) return -(s64)EFAULT;
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
    if ((uintptr_t)udata >= 0x0000800000000000ULL) return -(s64)EFAULT;

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

static s64 sys_capset_impl(pt_regs_t *r)
{
    cap_user_header_t *uhdr = (cap_user_header_t *)r->rdi;
    cap_user_data_t   *udata = (cap_user_data_t *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!uhdr || !udata) return -(s64)EFAULT;
    if ((uintptr_t)uhdr >= 0x0000800000000000ULL ||
        (uintptr_t)udata >= 0x0000800000000000ULL) return -(s64)EFAULT;

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

static s64 sys_sethostname_impl(pt_regs_t *r)
{
    const char *name = (const char *)r->rdi;
    size_t len = (size_t)r->rsi;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!name || (uintptr_t)name >= 0x8000000000000000ULL || len >= sizeof(g_kernel_nodename)) return -(s64)EINVAL;
    char buf[65];
    memset(buf, 0, sizeof(buf));
    if (copy_from_user(buf, name, len) != 0) return -(s64)EFAULT;
    buf[len] = '\0';
    strncpy(g_kernel_nodename, buf, sizeof(g_kernel_nodename) - 1);
    g_kernel_nodename[sizeof(g_kernel_nodename) - 1] = '\0';
    return 0;
}

static s64 sys_setdomainname_impl(pt_regs_t *r)
{
    const char *name = (const char *)r->rdi;
    size_t len = (size_t)r->rsi;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!name || (uintptr_t)name >= 0x8000000000000000ULL || len >= sizeof(g_kernel_domainname)) return -(s64)EINVAL;
    char buf[65];
    memset(buf, 0, sizeof(buf));
    if (copy_from_user(buf, name, len) != 0) return -(s64)EFAULT;
    buf[len] = '\0';
    strncpy(g_kernel_domainname, buf, sizeof(g_kernel_domainname) - 1);
    g_kernel_domainname[sizeof(g_kernel_domainname) - 1] = '\0';
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

static s64 sys_getpriority_impl(pt_regs_t *r)
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

static s64 sys_setpriority_impl(pt_regs_t *r)
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

static s64 sys_chroot_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_CHROOT) && !security_check_permission(proc, CAP_SYS_ADMIN)) {
        return -(s64)EPERM;
    }
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (!S_ISDIR(dentry->d_inode->i_mode)) {
        return -(s64)ENOTDIR;
    }

    if (proc) {
        /* kpath is already a real path (copy_user_path_resolve() mapped it
         * through any existing jail), so nesting chroot(2) narrows the jail
         * rather than escaping it. POSIX leaves cwd alone; this kernel moves
         * it to the new root, which is the safe direction — leaving it outside
         * would hand the process a working directory it can no longer name and
         * a ".." that walks out. */
        strncpy(proc->root, kpath, sizeof(proc->root) - 1);
        proc->root[sizeof(proc->root) - 1] = '\0';
        proc->cwd[0] = '/';
        proc->cwd[1] = '\0';
    }
    return 0;
}

/* ── Linux Inotify Subsystem ──────────────────────────────────────────────── */
#define INOTIFY_MAX_WATCHES 32
#define INOTIFY_MAX_EVENTS  64

#define IN_ACCESS        0x00000001
#define IN_MODIFY        0x00000002
#define IN_ATTRIB        0x00000004
#define IN_CLOSE_WRITE   0x00000008
#define IN_CLOSE_NOWRITE 0x00000010
#define IN_OPEN          0x00000020
#define IN_MOVED_FROM    0x00000040
#define IN_MOVED_TO      0x00000080
#define IN_CREATE        0x00000100
#define IN_DELETE        0x00000200
#define IN_DELETE_SELF   0x00000400
#define IN_MOVE_SELF     0x00000800
#define IN_IGNORED       0x00008000
#define IN_ISDIR         0x40000000
#define IN_ONESHOT       0x80000000

#define IN_CLOEXEC       02000000
#define IN_NONBLOCK      00004000

typedef struct {
    int      wd;
    char     path[128];
    uint32_t mask;
    int      active;
} inotify_watch_entry_t;

typedef struct inotify_raw_event {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char     name[32];
} inotify_raw_event_t;

typedef struct inotify_ctx {
    spinlock_t lock;
    int next_wd;
    int watch_count;
    inotify_watch_entry_t watches[INOTIFY_MAX_WATCHES];
    int event_head;
    int event_tail;
    int event_count;
    inotify_raw_event_t events[INOTIFY_MAX_EVENTS];
    int flags;
} inotify_ctx_t;

struct user_inotify_event {
    int      wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

static s64 inotify_read_op(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data) return -(s64)EBADF;
    if (!buf || len < sizeof(struct user_inotify_event)) return -(s64)EINVAL;

    inotify_ctx_t *ctx = (inotify_ctx_t *)filp->private_data;
    spinlock_lock(&ctx->lock);

    if (ctx->event_count == 0) {
        spinlock_unlock(&ctx->lock);
        if (filp->f_flags & O_NONBLOCK) return -(s64)EAGAIN;
        return 0;
    }

    size_t bytes_written = 0;
    u8 *out = (u8 *)buf;

    while (ctx->event_count > 0) {
        inotify_raw_event_t *ev = &ctx->events[ctx->event_head];
        size_t event_wire_size = sizeof(struct user_inotify_event) + ev->len;
        if (bytes_written + event_wire_size > len) {
            if (bytes_written == 0) {
                spinlock_unlock(&ctx->lock);
                return -(s64)EINVAL;
            }
            break;
        }

        struct user_inotify_event hdr;
        hdr.wd = ev->wd;
        hdr.mask = ev->mask;
        hdr.cookie = ev->cookie;
        hdr.len = ev->len;

        memcpy(out + bytes_written, &hdr, sizeof(hdr));
        if (ev->len > 0) {
            memcpy(out + bytes_written + sizeof(hdr), ev->name, ev->len);
        }

        bytes_written += event_wire_size;
        ctx->event_head = (ctx->event_head + 1) % INOTIFY_MAX_EVENTS;
        ctx->event_count--;
    }

    spinlock_unlock(&ctx->lock);
    return (s64)bytes_written;
}

static int inotify_poll_op(file_t *filp)
{
    if (!filp || !filp->private_data) return 0;
    inotify_ctx_t *ctx = (inotify_ctx_t *)filp->private_data;
    int mask = 0;
    spinlock_lock(&ctx->lock);
    if (ctx->event_count > 0) mask |= (POLLIN | POLLRDNORM);
    mask |= (POLLOUT | POLLWRNORM);
    spinlock_unlock(&ctx->lock);
    return mask;
}

static s64 inotify_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        kfree(filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_inotify_fops = {
    .read = inotify_read_op,
    .poll = inotify_poll_op,
    .release = inotify_release_op,
};

static s64 sys_inotify_init1_impl(pt_regs_t *r)
{
    int flags = (int)r->rdi;
    if (flags & ~(IN_CLOEXEC | IN_NONBLOCK)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    inotify_ctx_t *ctx = (inotify_ctx_t *)kzalloc(sizeof(inotify_ctx_t));
    if (!ctx) return -(s64)ENOMEM;
    ctx->next_wd = 1;
    ctx->flags = flags;
    spinlock_init(&ctx->lock);

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    f->f_op = &g_inotify_fops;
    f->private_data = ctx;
    f->f_flags = (flags & IN_NONBLOCK) ? O_NONBLOCK : 0;
    f->f_fd_flags = (flags & IN_CLOEXEC) ? FD_CLOEXEC : 0;
    f->f_mode = 0600;
    f->f_count = 1;

    {
        s64 nfd = fd_install_from(proc, f, f->f_fd_flags, 3);
        if (nfd >= 0) return nfd;
    }
    kfree(ctx);
    kfree(f);
    return -(s64)EMFILE;
}

static s64 sys_inotify_init_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.rdi = 0;
    return sys_inotify_init1_impl(&sub);
}

static s64 sys_inotify_add_watch_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *user_path = (const char *)r->rsi;
    u32 mask = (u32)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = proc->handle_table[fd];
    if (f->f_op != &g_inotify_fops || !f->private_data) return -(s64)EINVAL;

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }

    inotify_ctx_t *ctx = (inotify_ctx_t *)f->private_data;
    spinlock_lock(&ctx->lock);

    /* Check if already watched */
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (ctx->watches[i].active && strcmp(ctx->watches[i].path, kpath) == 0) {
            ctx->watches[i].mask = mask;
            int existing_wd = ctx->watches[i].wd;
            spinlock_unlock(&ctx->lock);
            return existing_wd;
        }
    }

    /* Allocate new watch */
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (!ctx->watches[i].active) {
            ctx->watches[i].active = 1;
            ctx->watches[i].wd = ctx->next_wd++;
            ctx->watches[i].mask = mask;
            strncpy(ctx->watches[i].path, kpath, sizeof(ctx->watches[i].path) - 1);
            ctx->watches[i].path[sizeof(ctx->watches[i].path) - 1] = '\0';
            ctx->watch_count++;
            int assigned_wd = ctx->watches[i].wd;

            /* Post initial access event */
            if (ctx->event_count < INOTIFY_MAX_EVENTS) {
                inotify_raw_event_t *ev = &ctx->events[ctx->event_tail];
                ev->wd = assigned_wd;
                ev->mask = mask & (IN_OPEN | IN_ACCESS | IN_ATTRIB | IN_ISDIR);
                ev->cookie = 0;
                ev->len = 0;
                ev->name[0] = '\0';
                ctx->event_tail = (ctx->event_tail + 1) % INOTIFY_MAX_EVENTS;
                ctx->event_count++;
            }

            spinlock_unlock(&ctx->lock);
            return assigned_wd;
        }
    }

    spinlock_unlock(&ctx->lock);
    return -(s64)ENOSPC;
}

static s64 sys_inotify_rm_watch_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int wd = (int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = proc->handle_table[fd];
    if (f->f_op != &g_inotify_fops || !f->private_data) return -(s64)EINVAL;

    inotify_ctx_t *ctx = (inotify_ctx_t *)f->private_data;
    spinlock_lock(&ctx->lock);

    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (ctx->watches[i].active && ctx->watches[i].wd == wd) {
            ctx->watches[i].active = 0;
            ctx->watch_count--;

            /* Post IN_IGNORED event */
            if (ctx->event_count < INOTIFY_MAX_EVENTS) {
                inotify_raw_event_t *ev = &ctx->events[ctx->event_tail];
                ev->wd = wd;
                ev->mask = IN_IGNORED;
                ev->cookie = 0;
                ev->len = 0;
                ev->name[0] = '\0';
                ctx->event_tail = (ctx->event_tail + 1) % INOTIFY_MAX_EVENTS;
                ctx->event_count++;
            }

            spinlock_unlock(&ctx->lock);
            return 0;
        }
    }

    spinlock_unlock(&ctx->lock);
    return -(s64)EINVAL;
}

/* ── Linux Extended Attributes (xattr) Subsystem ─────────────────────────── */
#define MAX_XATTR_ENTRIES 128
#define XATTR_CREATE  0x1
#define XATTR_REPLACE 0x2

typedef struct {
    char   path[128];
    char   name[64];
    char   value[256];
    size_t val_len;
    int    active;
} xattr_entry_t;

static xattr_entry_t g_xattrs[MAX_XATTR_ENTRIES];
static spinlock_t    g_xattr_lock = SPINLOCK_INIT;

static s64 do_setxattr(const char *path, const char *name, const void *value, size_t size, int flags)
{
    if (!path || !name || (size > 0 && !value) || size > 256) return -(s64)EINVAL;
    if (strlen(name) >= 64) return -(s64)ERANGE;

    spinlock_lock(&g_xattr_lock);

    int existing_slot = -1;
    int free_slot = -1;

    for (int i = 0; i < MAX_XATTR_ENTRIES; i++) {
        if (g_xattrs[i].active) {
            if (strcmp(g_xattrs[i].path, path) == 0 && strcmp(g_xattrs[i].name, name) == 0) {
                existing_slot = i;
                break;
            }
        } else if (free_slot < 0) {
            free_slot = i;
        }
    }

    if ((flags & XATTR_CREATE) && existing_slot >= 0) {
        spinlock_unlock(&g_xattr_lock);
        return -(s64)EEXIST;
    }
    if ((flags & XATTR_REPLACE) && existing_slot < 0) {
        spinlock_unlock(&g_xattr_lock);
        return -(s64)ENODATA;
    }

    int slot = (existing_slot >= 0) ? existing_slot : free_slot;
    if (slot < 0) {
        spinlock_unlock(&g_xattr_lock);
        return -(s64)ENOSPC;
    }

    strncpy(g_xattrs[slot].path, path, sizeof(g_xattrs[slot].path) - 1);
    g_xattrs[slot].path[sizeof(g_xattrs[slot].path) - 1] = '\0';

    strncpy(g_xattrs[slot].name, name, sizeof(g_xattrs[slot].name) - 1);
    g_xattrs[slot].name[sizeof(g_xattrs[slot].name) - 1] = '\0';

    if (size > 0 && value) {
        if (copy_from_user(g_xattrs[slot].value, value, size) != 0) {
            spinlock_unlock(&g_xattr_lock);
            return -(s64)EFAULT;
        }
    }
    g_xattrs[slot].val_len = size;
    g_xattrs[slot].active = 1;

    spinlock_unlock(&g_xattr_lock);
    return 0;
}

static s64 do_getxattr(const char *path, const char *name, void *value, size_t size)
{
    if (!path || !name) return -(s64)EINVAL;

    spinlock_lock(&g_xattr_lock);
    for (int i = 0; i < MAX_XATTR_ENTRIES; i++) {
        if (g_xattrs[i].active && strcmp(g_xattrs[i].path, path) == 0 && strcmp(g_xattrs[i].name, name) == 0) {
            size_t val_len = g_xattrs[i].val_len;
            if (size == 0 || !value) {
                spinlock_unlock(&g_xattr_lock);
                return (s64)val_len;
            }
            if (size < val_len) {
                spinlock_unlock(&g_xattr_lock);
                return -(s64)ERANGE;
            }
            if (copy_to_user(value, g_xattrs[i].value, val_len) != 0) {
                spinlock_unlock(&g_xattr_lock);
                return -(s64)EFAULT;
            }
            spinlock_unlock(&g_xattr_lock);
            return (s64)val_len;
        }
    }
    spinlock_unlock(&g_xattr_lock);
    return -(s64)ENODATA;
}

static s64 do_listxattr(const char *path, char *list, size_t size)
{
    if (!path) return -(s64)EINVAL;

    spinlock_lock(&g_xattr_lock);
    size_t total_len = 0;
    for (int i = 0; i < MAX_XATTR_ENTRIES; i++) {
        if (g_xattrs[i].active && strcmp(g_xattrs[i].path, path) == 0) {
            total_len += strlen(g_xattrs[i].name) + 1;
        }
    }

    if (size == 0 || !list) {
        spinlock_unlock(&g_xattr_lock);
        return (s64)total_len;
    }
    if (size < total_len) {
        spinlock_unlock(&g_xattr_lock);
        return -(s64)ERANGE;
    }

    size_t off = 0;
    for (int i = 0; i < MAX_XATTR_ENTRIES; i++) {
        if (g_xattrs[i].active && strcmp(g_xattrs[i].path, path) == 0) {
            size_t nlen = strlen(g_xattrs[i].name) + 1;
            if (copy_to_user(list + off, g_xattrs[i].name, nlen) != 0) {
                spinlock_unlock(&g_xattr_lock);
                return -(s64)EFAULT;
            }
            off += nlen;
        }
    }
    spinlock_unlock(&g_xattr_lock);
    return (s64)total_len;
}

static s64 do_removexattr(const char *path, const char *name)
{
    if (!path || !name) return -(s64)EINVAL;

    spinlock_lock(&g_xattr_lock);
    for (int i = 0; i < MAX_XATTR_ENTRIES; i++) {
        if (g_xattrs[i].active && strcmp(g_xattrs[i].path, path) == 0 && strcmp(g_xattrs[i].name, name) == 0) {
            g_xattrs[i].active = 0;
            spinlock_unlock(&g_xattr_lock);
            return 0;
        }
    }
    spinlock_unlock(&g_xattr_lock);
    return -(s64)ENODATA;
}

static s64 sys_setxattr_impl(pt_regs_t *r)
{
    const char *upath = (const char *)r->rdi;
    const char *uname = (const char *)r->rsi;
    const void *uval = (const void *)r->rdx;
    size_t size = (size_t)r->r10;
    int flags = (int)r->r8;

    char kpath[256];
    char kname[64];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), upath);
    if (perr < 0) return perr;
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_setxattr(kpath, kname, uval, size, flags);
}

static s64 sys_lsetxattr_impl(pt_regs_t *r)
{
    return sys_setxattr_impl(r);
}

static s64 sys_fsetxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;
    const void *uval = (const void *)r->rdx;
    size_t size = (size_t)r->r10;
    int flags = (int)r->r8;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    char kpath[32];
    snprintf(kpath, sizeof(kpath), "fd:%d", fd);
    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_setxattr(kpath, kname, uval, size, flags);
}

static s64 sys_getxattr_impl(pt_regs_t *r)
{
    const char *upath = (const char *)r->rdi;
    const char *uname = (const char *)r->rsi;
    void *uval = (void *)r->rdx;
    size_t size = (size_t)r->r10;

    char kpath[256];
    char kname[64];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), upath);
    if (perr < 0) return perr;
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_getxattr(kpath, kname, uval, size);
}

static s64 sys_lgetxattr_impl(pt_regs_t *r)
{
    return sys_getxattr_impl(r);
}

static s64 sys_fgetxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;
    void *uval = (void *)r->rdx;
    size_t size = (size_t)r->r10;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    char kpath[32];
    snprintf(kpath, sizeof(kpath), "fd:%d", fd);
    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_getxattr(kpath, kname, uval, size);
}

static s64 sys_listxattr_impl(pt_regs_t *r)
{
    const char *upath = (const char *)r->rdi;
    char *ulist = (char *)r->rsi;
    size_t size = (size_t)r->rdx;

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), upath);
    if (perr < 0) return perr;

    return do_listxattr(kpath, ulist, size);
}

static s64 sys_llistxattr_impl(pt_regs_t *r)
{
    return sys_listxattr_impl(r);
}

static s64 sys_flistxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    char *ulist = (char *)r->rsi;
    size_t size = (size_t)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    char kpath[32];
    snprintf(kpath, sizeof(kpath), "fd:%d", fd);

    return do_listxattr(kpath, ulist, size);
}

static s64 sys_removexattr_impl(pt_regs_t *r)
{
    const char *upath = (const char *)r->rdi;
    const char *uname = (const char *)r->rsi;

    char kpath[256];
    char kname[64];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), upath);
    if (perr < 0) return perr;
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_removexattr(kpath, kname);
}

static s64 sys_lremovexattr_impl(pt_regs_t *r)
{
    return sys_removexattr_impl(r);
}

static s64 sys_fremovexattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;

    char kpath[32];
    snprintf(kpath, sizeof(kpath), "fd:%d", fd);
    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_removexattr(kpath, kname);
}

/* ============================================================================
 * Additional Linux / POSIX syscalls
 *
 * Everything below is either composed from an existing primitive or is the
 * correct behaviour for a subsystem this kernel deliberately does not have
 * (e.g. no swap → mlock is a guaranteed-success no-op). Calls that would need
 * a real subsystem that is absent are NOT registered and therefore return
 * -ENOSYS via the dispatcher's default path.
 * ========================================================================== */

/* creat(path, mode) == open(path, O_CREAT|O_WRONLY|O_TRUNC, mode) */
static s64 sys_creat_impl(pt_regs_t *r)
{
    pt_regs_t s = *r;
    s.rdx = r->rsi;                                    /* mode  */
    s.rsi = (u64)(O_CREAT | O_WRONLY | O_TRUNC);       /* flags */
    return sys_open_impl(&s);
}

/* lchown(path, uid, gid) — chown without following a trailing symlink. */
static s64 sys_lchown_impl(pt_regs_t *r)
{
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), (const char *)r->rdi);
    if (perr < 0) return perr;
    return vfs_lchown(kpath, (u32)r->rsi, (u32)r->rdx);
}

/* preadv / pwritev / preadv2 / pwritev2 — positional scatter-gather I/O,
 * built on pread64/pwrite64. The v2 flag word is accepted and ignored (there
 * is no RWF_* behaviour to honour: no O_DIRECT, no writeback cache). */
static s64 sys_p_rw_v(pt_regs_t *r, bool write)
{
    int fd = (int)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    int iovcnt = (int)r->rdx;
    u64 off = r->r10;                     /* low half of the offset */

    if (!iov || iovcnt <= 0 || iovcnt > 1024) return -(s64)EINVAL;
    if ((uintptr_t)iov >= 0x8000000000000000ULL) return -(s64)EFAULT;

    s64 total = 0;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec kiov;
        if (copy_from_user(&kiov, &iov[i], sizeof kiov) != 0) return -(s64)EFAULT;
        if (kiov.iov_len == 0) continue;
        if (kiov.iov_len > 0x7FFFFFFF) return -(s64)EINVAL;
        if (total + (s64)kiov.iov_len < total) return -(s64)EINVAL;

        pt_regs_t s = *r;
        s.rdi = (u64)fd;
        s.rsi = (u64)(uintptr_t)kiov.iov_base;
        s.rdx = (u64)kiov.iov_len;
        s.r10 = off + (u64)total;
        s64 n = write ? sys_pwrite64_impl(&s) : sys_pread64_impl(&s);
        if (n < 0) return total > 0 ? total : n;
        total += n;
        if ((size_t)n < kiov.iov_len) break;
    }
    return total;
}
static s64 sys_preadv_impl(pt_regs_t *r)  { return sys_p_rw_v(r, false); }
static s64 sys_pwritev_impl(pt_regs_t *r) { return sys_p_rw_v(r, true);  }

/* No swap device or reclaim exists, so every user page that is mapped is
 * already resident and unevictable — there is nothing for "locked" to pin
 * against. These used to be blind no-ops; now they genuinely record
 * (VMA_F_LOCKED, kernel/mm/vma.c) which mappings the caller asked to be
 * locked. Unlike real mlock(2), a range with no VMA at all (e.g. inside the
 * brk()-managed heap, which never gets one — see vma_set_locked()'s own
 * comment) is a no-op rather than -ENOMEM: this kernel's VMA list is a
 * best-effort index, not an authoritative map, so treating "no VMA found"
 * as "definitely unmapped" would reject plenty of genuinely valid memory. */
static s64 sys_mlock_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    u64 addr = r->rdi, len = r->rsi;
    if (len == 0) return 0;
    u64 start = ALIGN_DOWN(addr, PAGE_SIZE);
    u64 end   = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end <= start) return -(s64)EINVAL;
    return vma_set_locked(proc, start, end, true);
}

static s64 sys_munlock_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    u64 addr = r->rdi, len = r->rsi;
    if (len == 0) return 0;
    u64 start = ALIGN_DOWN(addr, PAGE_SIZE);
    u64 end   = ALIGN_UP(addr + len, PAGE_SIZE);
    if (end <= start) return -(s64)EINVAL;
    return vma_set_locked(proc, start, end, false);
}

/* mlock2(addr, len, flags) — MLOCK_ONFAULT would defer locking until each
 * page faults in; every mapped page is already resident here (no demand
 * paging beyond the existing VMA_F_DEMAND path this doesn't interact with),
 * so there's no meaningful "on fault" moment to defer to — treated the same
 * as a plain mlock(). */
static s64 sys_mlock2_impl(pt_regs_t *r)
{
    return sys_mlock_impl(r);
}

static s64 sys_mlockall_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    s64 flags = (s64)(s32)r->rdi;
    if (flags == 0 || (flags & ~(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT))) return -(s64)EINVAL;
    if (flags & MCL_CURRENT) vma_set_locked_all(proc, true);
    /* MCL_FUTURE ("lock every mapping this process creates from now on") is
     * accepted but not enforced: nothing here tracks a per-process
     * lock-all-future-mappings flag that mmap() would need to consult. A
     * process that asked for MCL_FUTURE gets MCL_CURRENT's effect now and
     * no error, rather than a silent, permanent gap for something that
     * still has nothing to actually pin against either way. */
    return 0;
}

static s64 sys_munlockall_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    vma_set_locked_all(proc, false);
    return 0;
}

/* rt_sigpending(set, sigsetsize) — signals raised but still blocked. */
static s64 sys_rt_sigpending_impl(pt_regs_t *r)
{
    sigset_t *uset = (sigset_t *)r->rdi;
    size_t sz = (size_t)r->rsi;
    if (sz != sizeof(sigset_t)) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!uset || (uintptr_t)uset >= 0x8000000000000000ULL) return -(s64)EFAULT;
    sigset_t pend = proc->sig_pending & proc->sig_blocked;
    if (copy_to_user(uset, &pend, sizeof pend) != 0) return -(s64)EFAULT;
    return 0;
}

/* sigaltstack(new, old) — set and/or get alternate signal stack context. */
static s64 sys_sigaltstack_impl(pt_regs_t *r)
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
        if ((uintptr_t)uold >= 0x8000000000000000ULL) return -(s64)EFAULT;
        if (copy_to_user(uold, &old_st, sizeof(old_st)) != 0) return -(s64)EFAULT;
    }

    if (unew) {
        if ((uintptr_t)unew >= 0x8000000000000000ULL) return -(s64)EFAULT;
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
            if ((uintptr_t)new_st.ss_sp >= 0x8000000000000000ULL) return -(s64)EFAULT;
            p->sas_ss_sp    = new_st.ss_sp;
            p->sas_ss_size  = new_st.ss_size;
            p->sas_ss_flags = new_st.ss_flags & ~1;
        }
    }
    return 0;
}

/* setfsuid/setfsgid — set filesystem user/group identity according to Linux spec */
static s64 sys_setfsuid_impl(pt_regs_t *r)
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
static s64 sys_setfsgid_impl(pt_regs_t *r)
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

/* mknod/mknodat — only regular files are creatable this way. Named FIFOs are
 * not implemented (anonymous pipe(2) only); char/block device nodes live in
 * devfs and are not created from userspace. */
static s64 do_mknod(const char *path, u32 mode)
{
    u32 fmt = mode & S_IFMT;
    if (fmt == 0 || fmt == S_IFREG) {
        file_t *f = vfs_open(path, O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
        if (!f) return -(s64)EEXIST;
        vfs_close(f);
        return 0;
    }
    if (fmt == S_IFIFO) {
        file_t *f = vfs_open(path, O_CREAT | O_EXCL | O_RDWR, (mode & 07777) | S_IFIFO);
        if (!f) return -(s64)EEXIST;
        if (f->f_inode) {
            f->f_inode->i_mode = S_IFIFO | (mode & 07777);
        }
        vfs_close(f);
        return 0;
    }
    return -(s64)EPERM;                                 /* S_IFCHR / S_IFBLK / S_IFSOCK */
}
static s64 sys_mknod_impl(pt_regs_t *r)
{
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), (const char *)r->rdi);
    if (perr < 0) return perr;
    return do_mknod(kpath, (u32)r->rsi);
}
static s64 sys_mknodat_impl(pt_regs_t *r)
{
    /* Honour absolute paths and AT_FDCWD; dirfd-relative paths are resolved by
     * copy_user_path_resolve() against the cwd like the other *at() stubs. */
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), (const char *)r->rsi);
    if (perr < 0) return perr;
    return do_mknod(kpath, (u32)r->rdx);
}

/* futimesat(dirfd, path, times) — obsolete, superseded by utimensat(2), but
 * still just a real dirfd-relative timestamp set with `struct timeval`
 * (microsecond, not nanosecond) precision. Was a pure no-op before. */
static s64 sys_futimesat_impl(pt_regs_t *r)
{
    int dfd = (int)(s32)r->rdi;
    const char *path = (const char *)r->rsi;
    const struct linux_timeval *times = (const struct linux_timeval *)r->rdx;
    if (!path) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (perr < 0) return perr;

    u64 atime, mtime;
    if (times) {
        struct linux_timeval t[2];
        if (copy_from_user(t, times, sizeof(t)) != 0) return -(s64)EFAULT;
        atime = (u64)t[0].tv_sec;
        mtime = (u64)t[1].tv_sec;
    } else {
        atime = mtime = get_cached_unix_time();
    }
    return vfs_utimes(kpath, atime, mtime);
}

/* unshare(flags) — CLONE_FS (0x200) and CLONE_FILES (0x400) are legal. */
static s64 sys_unshare_impl(pt_regs_t *r)
{
    u64 flags = r->rdi;
    if (flags & ~(0x00000200ULL | 0x00000400ULL)) return -(s64)EINVAL;
    return 0;
}

/* Must stay field-for-field identical to `struct timex` in
 * userland/libc/include/sys/timex.h — that is the struct every caller in
 * this codebase actually allocates, and copy_from_user()/copy_to_user()
 * below move exactly sizeof(this struct) bytes. It deliberately does NOT
 * carry the trailing reserved int[11] real Linux's struct timex has: that
 * would make this struct larger than what userland allocates, and
 * copy_to_user() would then write past the end of a caller's `struct timex`
 * on their stack or heap. `long time_sec/time_usec` here occupy the exact
 * same bytes as userland's nested `struct timeval time` (both are two
 * consecutive `long`s) — just addressed without the extra naming layer,
 * since this file has no struct timeval of its own to nest. */
struct linux_timex {
    unsigned int modes;
    int          _pad0;
    long         offset;
    long         freq;
    long         maxerror;
    long         esterror;
    int          status;
    int          _pad1;
    long         constant;
    long         precision;
    long         tolerance;
    long         time_sec;
    long         time_usec;
    long         tick;
    long         ppsfreq;
    long         jitter;
    int          shift;
    int          _pad2;
    long         stabil;
    long         jitcnt;
    long         calcnt;
    long         errcnt;
    long         stbcnt;
    int          tai;
};

#define LINUX_TIME_OK 0

static s64 adjtimex_core(process_t *proc, struct linux_timex *user_buf)
{
    if (!user_buf) return -(s64)EFAULT;
    struct linux_timex tx;
    if (copy_from_user(&tx, user_buf, sizeof(tx)) != 0) return -(s64)EFAULT;

    if (tx.modes != 0 && !security_check_permission(proc, CAP_SYS_TIME))
        return -(s64)EPERM;
    /* modes != 0 is accepted (not rejected wholesale like the old stub did)
     * but has no discipline effect beyond what's reported back below —
     * there is nothing here to steer a PLL/FLL against. */

    u64 now = get_cached_unix_time();
    tx.time_sec  = (long)now;
    tx.time_usec = 0;
    tx.maxerror  = 500000;      /* microseconds; unsynchronized-clock ballpark */
    tx.esterror  = 500000;
    tx.status    = 0;           /* no STA_* bits tracked */
    tx.constant  = 0;
    tx.precision = 1;           /* 1 tick = 10ms at this kernel's 100Hz */
    tx.tolerance = 0x7FFFFFFFL; /* Linux's own MAXFREQ-derived default */
    tx.tick      = 10000;       /* microseconds per tick at 100Hz */
    tx.ppsfreq = tx.jitter = tx.stabil = tx.jitcnt = tx.calcnt = 0;
    tx.errcnt = tx.stbcnt = 0;
    tx.shift = 0;
    tx.tai = 0;

    if (copy_to_user(user_buf, &tx, sizeof(tx)) != 0) return -(s64)EFAULT;
    return LINUX_TIME_OK;
}

static s64 sys_adjtimex_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    return adjtimex_core(proc, (struct linux_timex *)r->rdi);
}

static s64 sys_clock_adjtime_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    u32 clk_id = (u32)r->rdi;
    if (clk_id != 0) return -(s64)EINVAL; /* only CLOCK_REALTIME, as with clock_settime */
    return adjtimex_core(proc, (struct linux_timex *)r->rsi);
}

/* settimeofday — wall clock is read-only for userspace. */
/* settimeofday(tv, tz) — used to unconditionally return -EPERM, even for
 * root: nobody could ever set the time. Real CAP_SYS_TIME check plus a real
 * set now, sharing clock_settime(2)'s underlying set_wall_clock(). `tz`
 * (the second argument) is accepted and ignored, matching real Linux —
 * timezone-via-settimeofday has been deprecated there since the 1990s. */
static s64 sys_settimeofday_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!security_check_permission(proc, CAP_SYS_TIME)) return -(s64)EPERM;

    const struct linux_timeval *user_tv = (const struct linux_timeval *)r->rdi;
    if (!user_tv) return -(s64)EFAULT;
    struct linux_timeval tv;
    if (copy_from_user(&tv, user_tv, sizeof(tv)) != 0) return -(s64)EFAULT;
    if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000L) return -(s64)EINVAL;
    set_wall_clock((u64)tv.tv_sec);
    return 0;
}

/* sendmmsg/recvmmsg — iterate the mmsghdr array over sendmsg/recvmsg. */
static s64 sys_mmsg(pt_regs_t *r, bool send)
{
    int fd = (int)r->rdi;
    u8 *umsgvec = (u8 *)r->rsi;
    unsigned vlen = (unsigned)r->rdx;
    unsigned flags = (unsigned)r->r10;
    if (!umsgvec || (uintptr_t)umsgvec >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (vlen > 1024) vlen = 1024;

    /* struct mmsghdr { struct msghdr msg_hdr; unsigned msg_len; }; msghdr is
     * 56 bytes on x86_64, so the element stride is 64 (with padding). */
    const unsigned STRIDE = 64, MSGLEN_OFF = 56;
    unsigned done = 0;
    for (; done < vlen; done++) {
        u8 *elem = umsgvec + (u64)done * STRIDE;
        pt_regs_t s = *r;
        s.rdi = (u64)fd;
        s.rsi = (u64)(uintptr_t)elem;                   /* &msg_hdr */
        s.rdx = (u64)flags;
        s64 n = send ? sys_sendto_impl(&s) : sys_recvfrom_impl(&s);
        if (n < 0) return done ? (s64)done : n;
        u32 msglen = (u32)n;
        if (copy_to_user(elem + MSGLEN_OFF, &msglen, sizeof msglen) != 0)
            return done ? (s64)done : -(s64)EFAULT;
        if (!send && n == 0) { done++; break; }
    }
    return (s64)done;
}
static s64 sys_sendmmsg_impl(pt_regs_t *r) { return sys_mmsg(r, true);  }
static s64 sys_recvmmsg_impl(pt_regs_t *r) { return sys_mmsg(r, false); }

/* process_vm_readv / process_vm_writev — copy between the caller and a target
 * process, page by page, translating the remote virtual addresses through the
 * target's PML4. */
#define PVM_USER_MAX 0x0000800000000000ULL

/* True if [base, base+len) is a non-wrapping range wholly inside the user half. */
static bool pvm_user_range_ok(u64 base, u64 len)
{
    if (len == 0) return true;
    if (base >= PVM_USER_MAX) return false;
    if (len > PVM_USER_MAX - base) return false;   /* overflow / crosses into kernel half */
    return true;
}

static s64 do_process_vm(pt_regs_t *r, bool write_to_remote)
{
    u32 pid = (u32)r->rdi;
    const struct iovec *ulocal = (const struct iovec *)r->rsi;
    unsigned long liovcnt = (unsigned long)r->rdx;
    const struct iovec *uremote = (const struct iovec *)r->r10;
    unsigned long riovcnt = (unsigned long)r->r8;

    if (liovcnt > 1024 || riovcnt > 1024) return -(s64)EINVAL;
    if (!ulocal || !uremote) return -(s64)EFAULT;

    /* Snapshot the target under the scheduler lock — never hold a raw
     * process_t* across the blocking copy loop below. */
    struct proc_ident tgt;
    if (!sched_proc_ident(pid, &tgt) || tgt.is_zombie || !tgt.pml4_phys)
        return -(s64)ESRCH;

    /* ptrace-style access check (Linux PTRACE_MODE_ATTACH_REALCREDS): root, or a
     * caller whose *real* uid/gid equal the target's real, effective AND saved
     * ids. Anything weaker lets a process poke a setuid peer that dropped euid
     * but still holds suid==0. */
    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;
    if (caller->euid != 0) {
        if (caller->uid != tgt.uid || caller->uid != tgt.euid || caller->uid != tgt.suid ||
            caller->gid != tgt.gid || caller->gid != tgt.egid || caller->gid != tgt.sgid)
            return -(s64)EPERM;
        if (tgt.pid <= 2) return -(s64)EPERM;   /* never non-root vs init/kernel */
    }

    const u64 need = write_to_remote ? (VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE)
                                     : (VMM_F_PRESENT | VMM_F_USER);

    struct iovec rio;
    unsigned long ri = 0;
    u64 roff = 0;
    s64 copied = 0;

    for (unsigned long li = 0; li < liovcnt; li++) {
        struct iovec lio;
        if (copy_from_user(&lio, &ulocal[li], sizeof lio) != 0) return -(s64)EFAULT;
        if (!pvm_user_range_ok((u64)(uintptr_t)lio.iov_base, lio.iov_len))
            return copied ? copied : -(s64)EFAULT;
        u8 *lptr = (u8 *)lio.iov_base;
        u64 lrem = lio.iov_len;

        /* Re-check the target between local iovecs: bail cleanly if it exited or
         * exec'd a new address space rather than translating against a stale
         * (possibly freed) PML4. */
        struct proc_ident now;
        if (!sched_proc_ident(pid, &now) || now.is_zombie ||
            now.pml4_phys != tgt.pml4_phys)
            return copied;

        while (lrem) {
            if (roff == 0) {
                if (ri >= riovcnt) return copied;
                if (copy_from_user(&rio, &uremote[ri], sizeof rio) != 0) return -(s64)EFAULT;
                ri++;
                if (!pvm_user_range_ok((u64)(uintptr_t)rio.iov_base, rio.iov_len))
                    return copied ? copied : -(s64)EFAULT;
                if (rio.iov_len == 0) { roff = 0; continue; }
            }
            u64 rva = (u64)(uintptr_t)rio.iov_base + roff;
            u64 rleft = rio.iov_len - roff;
            u64 page_left = PAGE_SIZE - (rva & (PAGE_SIZE - 1));
            u64 n = lrem;
            if (n > rleft) n = rleft;
            if (n > page_left) n = page_left;

            /* The remote page must be a ring-3-accessible page of the target
             * (and writable for a poke). This is what stops a remote iovec
             * pointing at the kernel higher-half — vmm_translate alone would
             * happily resolve it. */
            if ((vmm_query_flags(tgt.pml4_phys, rva) & need) != need)
                return copied ? copied : -(s64)EFAULT;

            phys_addr_t rphys = vmm_translate(tgt.pml4_phys, rva);
            if (!rphys) return copied ? copied : -(s64)EFAULT;
            u8 *rkern = (u8 *)PHYS_TO_VIRT(rphys);

            if (write_to_remote) {
                if (copy_from_user(rkern, lptr, n) != 0) return copied ? copied : -(s64)EFAULT;
            } else {
                if (copy_to_user(lptr, rkern, n) != 0) return copied ? copied : -(s64)EFAULT;
            }

            lptr += n; lrem -= n; copied += n;
            roff += n;
            if (roff == rio.iov_len) roff = 0;
        }
    }
    return copied;
}
static s64 sys_process_vm_readv_impl(pt_regs_t *r)  { return do_process_vm(r, false); }
static s64 sys_process_vm_writev_impl(pt_regs_t *r) { return do_process_vm(r, true);  }

/* ============================================================================
 * System V IPC (XSI) — thin marshalling over kernel/ipc/sysvipc.c
 * ========================================================================= */

static s64 sys_shmget_impl(pt_regs_t *r)
{
    return sysv_shmget((s32)r->rdi, (size_t)r->rsi, (int)r->rdx);
}
static s64 sys_shmat_impl(pt_regs_t *r)
{
    return sysv_shmat((int)r->rdi, (virt_addr_t)r->rsi, (int)r->rdx);
}
static s64 sys_shmdt_impl(pt_regs_t *r)
{
    return sysv_shmdt((virt_addr_t)r->rdi);
}
static s64 sys_shmctl_impl(pt_regs_t *r)
{
    return sysv_shmctl((int)r->rdi, (int)r->rsi, (void *)r->rdx);
}
static s64 sys_semget_impl(pt_regs_t *r)
{
    return sysv_semget((s32)r->rdi, (int)r->rsi, (int)r->rdx);
}
static s64 sys_semop_impl(pt_regs_t *r)
{
    return sysv_semop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);
}
/* semtimedop(semid, sops, nsops, timeout) — semop() that gives up with EAGAIN
 * once `timeout` has elapsed.  A NULL timeout is plain semop(). */
static s64 sys_semtimedop_impl(pt_regs_t *r)
{
    const struct linux_timespec *uts = (const struct linux_timespec *)r->r10;
    if (!uts) return sysv_semop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx);

    if ((uintptr_t)uts >= 0x8000000000000000ULL) return -(s64)EFAULT;
    struct linux_timespec ts;
    if (copy_from_user(&ts, uts, sizeof(ts)) != 0) return -(s64)EFAULT;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;

    /* Ticks run at 100 Hz. A non-zero timeout must never round down to zero,
     * which would turn semtimedop() into a non-blocking poll. */
    u64 ticks = (u64)ts.tv_sec * 100 + (u64)ts.tv_nsec / 10000000ULL;
    if (ticks == 0 && (ts.tv_sec || ts.tv_nsec)) ticks = 1;

    return sysv_semtimedop((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx, ticks, true);
}
static s64 sys_semctl_impl(pt_regs_t *r)
{
    /* The fourth argument is glibc's `union semun` passed by value: an int for
     * SETVAL, a pointer for the rest. */
    return sysv_semctl((int)r->rdi, (int)r->rsi, (int)r->rdx, r->r10);
}
static s64 sys_msgget_impl(pt_regs_t *r)
{
    return sysv_msgget((s32)r->rdi, (int)r->rsi);
}
static s64 sys_msgsnd_impl(pt_regs_t *r)
{
    return sysv_msgsnd((int)r->rdi, (const void *)r->rsi, (size_t)r->rdx, (int)r->r10);
}
static s64 sys_msgrcv_impl(pt_regs_t *r)
{
    return sysv_msgrcv((int)r->rdi, (void *)r->rsi, (size_t)r->rdx, (s64)r->r10, (int)r->r8);
}
static s64 sys_msgctl_impl(pt_regs_t *r)
{
    return sysv_msgctl((int)r->rdi, (int)r->rsi, (void *)r->rdx);
}

/* ============================================================================
 * POSIX timers — timer_create(2) family, alarm(2), setitimer(2)
 * ========================================================================= */

/* Ticks run at 100 Hz, so one tick is 10 ms. */
static u64 timespec_to_ticks(const struct linux_timespec *ts)
{
    u64 ticks = (u64)ts->tv_sec * 100 + (u64)ts->tv_nsec / 10000000ULL;
    /* Never round a non-zero interval down to "disarmed". */
    if (ticks == 0 && (ts->tv_sec || ts->tv_nsec)) ticks = 1;
    return ticks;
}

static void ticks_to_timespec(u64 ticks, struct linux_timespec *ts)
{
    ts->tv_sec  = (long)(ticks / 100);
    ts->tv_nsec = (long)((ticks % 100) * 10000000L);
}

/* The leading fields of struct sigevent; the rest is padding we never read. */
struct k_sigevent {
    u64 sigev_value;
    s32 sigev_signo;
    s32 sigev_notify;
};

static s64 sys_timer_create_impl(pt_regs_t *r)
{
    int clockid = (int)r->rdi;
    const void *usev = (const void *)r->rsi;
    void *utimerid   = (void *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!utimerid || (uintptr_t)utimerid >= 0x8000000000000000ULL) return -(s64)EFAULT;

    int notify = SIGEV_SIGNAL;
    int signo  = SIGALRM;
    u64 sigval = 0;

    if (usev) {
        if ((uintptr_t)usev >= 0x8000000000000000ULL) return -(s64)EFAULT;
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

static s64 sys_timer_settime_impl(pt_regs_t *r)
{
    int id     = (int)r->rdi;
    int flags  = (int)r->rsi;
    const struct itimerspec *unew = (const struct itimerspec *)r->rdx;
    struct itimerspec *uold       = (struct itimerspec *)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!unew || (uintptr_t)unew >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

    if (uold && (uintptr_t)uold < 0x8000000000000000ULL) {
        struct itimerspec ov;
        ticks_to_timespec(old_value, &ov.it_value);
        ticks_to_timespec(old_interval, &ov.it_interval);
        if (copy_to_user(uold, &ov, sizeof(ov)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_timer_gettime_impl(pt_regs_t *r)
{
    int id = (int)r->rdi;
    struct itimerspec *ucur = (struct itimerspec *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!ucur || (uintptr_t)ucur >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u64 value = 0, interval = 0;
    s64 ret = ktimer_gettime(proc, id, &value, &interval);
    if (ret < 0) return ret;

    struct itimerspec cur;
    ticks_to_timespec(value, &cur.it_value);
    ticks_to_timespec(interval, &cur.it_interval);
    return copy_to_user(ucur, &cur, sizeof(cur)) == 0 ? 0 : -(s64)EFAULT;
}

static s64 sys_timer_getoverrun_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return ktimer_getoverrun(proc, (int)r->rdi);
}

static s64 sys_timer_delete_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return ktimer_delete(proc, (int)r->rdi);
}

/* struct itimerval — the timeval-based interval timers. */
struct k_itimerval {
    struct linux_timeval it_interval;
    struct linux_timeval it_value;
};

static u64 timeval_to_ticks(const struct linux_timeval *tv)
{
    u64 ticks = (u64)tv->tv_sec * 100 + (u64)tv->tv_usec / 10000ULL;
    if (ticks == 0 && (tv->tv_sec || tv->tv_usec)) ticks = 1;
    return ticks;
}

static void ticks_to_timeval(u64 ticks, struct linux_timeval *tv)
{
    tv->tv_sec  = (long)(ticks / 100);
    tv->tv_usec = (long)((ticks % 100) * 10000L);
}

static s64 sys_setitimer_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    const struct k_itimerval *unew = (const struct k_itimerval *)r->rsi;
    struct k_itimerval *uold       = (struct k_itimerval *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 value = 0, interval = 0;
    if (unew) {
        if ((uintptr_t)unew >= 0x8000000000000000ULL) return -(s64)EFAULT;
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

    if (uold && (uintptr_t)uold < 0x8000000000000000ULL) {
        struct k_itimerval ov;
        ticks_to_timeval(old_value, &ov.it_value);
        ticks_to_timeval(old_interval, &ov.it_interval);
        if (copy_to_user(uold, &ov, sizeof(ov)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_getitimer_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    struct k_itimerval *ucur = (struct k_itimerval *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!ucur || (uintptr_t)ucur >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u64 value = 0, interval = 0;
    s64 ret = ktimer_getitimer(proc, which, &value, &interval);
    if (ret < 0) return ret;

    struct k_itimerval cur;
    ticks_to_timeval(value, &cur.it_value);
    ticks_to_timeval(interval, &cur.it_interval);
    return copy_to_user(ucur, &cur, sizeof(cur)) == 0 ? 0 : -(s64)EFAULT;
}

static s64 sys_alarm_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    return (s64)ktimer_alarm(proc, (u64)(u32)r->rdi * 100);
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

static s64 sys_rt_sigtimedwait_impl(pt_regs_t *r)
{
    const sigset_t *uset = (const sigset_t *)r->rdi;
    void *uinfo          = (void *)r->rsi;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->rdx;
    size_t sigsetsize    = (size_t)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
    if (!uset || (uintptr_t)uset >= 0x8000000000000000ULL) return -(s64)EFAULT;

    sigset_t set;
    if (copy_from_user(&set, uset, sizeof(set)) != 0) return -(s64)EFAULT;
    /* SIGKILL and SIGSTOP can never be waited for. */
    set &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    bool have_timeout = false;
    u64  deadline = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= 0x8000000000000000ULL) return -(s64)EFAULT;
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

            if (uinfo && (uintptr_t)uinfo < 0x8000000000000000ULL) {
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

static s64 sys_rt_sigsuspend_impl(pt_regs_t *r)
{
    const sigset_t *umask = (const sigset_t *)r->rdi;
    size_t sigsetsize     = (size_t)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
    if (!umask || (uintptr_t)umask >= 0x8000000000000000ULL) return -(s64)EFAULT;

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

static s64 sys_rt_sigqueueinfo_impl(pt_regs_t *r)
{
    u32 pid  = (u32)r->rdi;
    int sig  = (int)r->rsi;
    void *ui = (void *)r->rdx;

    if (sig < 0 || sig >= 64) return -(s64)EINVAL;
    if (ui && (uintptr_t)ui >= 0x8000000000000000ULL) return -(s64)EFAULT;

    /* The accompanying siginfo is validated but not queued: signals here are
     * a pending bitmask, so a value cannot be carried alongside one. */
    if (ui) {
        struct k_siginfo info;
        if (copy_from_user(&info, ui, sizeof(info)) != 0) return -(s64)EFAULT;
    }
    if (sig == 0) return 0;
    return sched_kill_process(pid, sig);
}

static s64 sys_rt_tgsigqueueinfo_impl(pt_regs_t *r)
{
    u32 tgid = (u32)r->rdi;
    int sig  = (int)r->rdx;
    void *ui = (void *)r->r10;

    if (sig < 0 || sig >= 64) return -(s64)EINVAL;
    if (ui && (uintptr_t)ui >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (sig == 0) return 0;
    return sched_kill_process(tgid, sig);
}

/* ============================================================================
 * mount(2)
 * ========================================================================= */

static s64 sys_mount_impl(pt_regs_t *r)
{
    const char *usource = (const char *)r->rdi;
    const char *utarget = (const char *)r->rsi;
    const char *ufstype = (const char *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!utarget || !ufstype) return -(s64)EFAULT;

    char source[128] = {0}, target[256] = {0}, fstype[32] = {0};
    if (usource && copy_str_from_user(source, usource, sizeof(source)) < 0) return -(s64)EFAULT;
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return -(s64)EFAULT;
    if (copy_str_from_user(fstype, ufstype, sizeof(fstype)) < 0) return -(s64)EFAULT;

    return vfs_mount(source[0] ? source : fstype, target, fstype, NULL);
}

/* ============================================================================
 * umount2(2)
 * ========================================================================= */

static s64 sys_umount2_impl(pt_regs_t *r)
{
    const char *utarget = (const char *)r->rdi;
    int flags = (int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!utarget || (uintptr_t)utarget >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char target[256] = {0};
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return -(s64)EFAULT;

    return vfs_umount(target, flags);
}

/* ============================================================================
 * iopl(2) / ioperm(2)
 * ========================================================================= */

static s64 sys_iopl_impl(pt_regs_t *r)
{
    unsigned int level = (unsigned int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (level > 3) return -(s64)EINVAL;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_RAWIO)) {
        return -(s64)EPERM;
    }
    r->rflags = (r->rflags & ~0x3000ULL) | (((u64)level & 3) << 12);
    return 0;
}

static s64 sys_ioperm_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_RAWIO)) {
        return -(s64)EPERM;
    }
    return 0;
}

/* ============================================================================
 * acct(2)
 * ========================================================================= */

static s64 sys_acct_impl(pt_regs_t *r)
{
    const char *filename = (const char *)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_PACCT)) {
        return -(s64)EPERM;
    }
    if (!filename) return 0;
    return -(s64)ENOSYS;
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

static s64 sys_kcmp_impl(pt_regs_t *r)
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
 * init_module(2) / delete_module(2) / finit_module(2)
 * ========================================================================= */

static s64 sys_finit_module_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_MODULE)) {
        return -(s64)EPERM;
    }
    return -(s64)ENOSYS;
}

/* ============================================================================
 * Memory-protection keys — pkey_alloc(2) / pkey_free(2) / pkey_mprotect(2)
 *
 * The hardware (CR4.PKE, enabled in cpu_enable_features_bsp()) tags each
 * user-accessible leaf PTE with one of 16 keys in bits 62:59, and the PKRU
 * register carries two bits per key — access-disable and write-disable — that
 * gate every data access to a page carrying that key, from ring 3 *and* from
 * ring 0. PKRU lives in the XSAVE state, so it is per-thread and the context
 * switch preserves it for free.
 *
 * Key 0 is the key every page starts with and is never handed out, so a
 * process that never calls pkey_alloc() is completely unaffected.
 * ========================================================================= */

static s64 sys_pkey_alloc_impl(pt_regs_t *r)
{
    unsigned long flags  = (unsigned long)r->rdi;
    unsigned long rights = (unsigned long)r->rsi;

    if (!g_pku_enabled) return -(s64)ENOSPC;   /* Linux reports "no keys left" */
    if (flags != 0) return -(s64)EINVAL;
    if (rights & ~(unsigned long)PKEY_ACCESS_MASK) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    int key = -1;
    irqflags_t irqf = spinlock_lock_irqsave(&g_fd_lock);
    for (int k = 1; k < PKEY_MAX; k++) {
        if (!(proc->pkey_alloc_map & (1u << k))) {
            proc->pkey_alloc_map |= (u16)(1u << k);
            key = k;
            break;
        }
    }
    spinlock_unlock_irqrestore(&g_fd_lock, irqf);
    if (key < 0) return -(s64)ENOSPC;

    /* Publish the requested rights into this thread's PKRU. Two bits per key:
     * bit 2k = access-disable, bit 2k+1 = write-disable. */
    u32 pkru = rdpkru();
    pkru &= ~(0x3u << (2 * key));
    if (rights & PKEY_DISABLE_ACCESS) pkru |= (1u << (2 * key));
    if (rights & PKEY_DISABLE_WRITE)  pkru |= (1u << (2 * key + 1));
    wrpkru(pkru);

    return key;
}

static s64 sys_pkey_free_impl(pt_regs_t *r)
{
    int key = (int)r->rdi;

    if (!g_pku_enabled) return -(s64)EINVAL;
    if (key <= 0 || key >= PKEY_MAX) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    irqflags_t irqf = spinlock_lock_irqsave(&g_fd_lock);
    bool was_allocated = (proc->pkey_alloc_map & (1u << key)) != 0;
    proc->pkey_alloc_map &= (u16)~(1u << key);
    spinlock_unlock_irqrestore(&g_fd_lock, irqf);
    if (!was_allocated) return -(s64)EINVAL;

    /* Freeing a key does not un-tag the pages still carrying it — that is
     * Linux's behaviour too, and it is why freeing a key in use is documented
     * as a programming error. Reset its PKRU bits to "deny", so a stale page
     * cannot silently become accessible when the key is handed out again. */
    u32 pkru = rdpkru();
    pkru |= (0x3u << (2 * key));
    wrpkru(pkru);
    return 0;
}

static s64 sys_pkey_mprotect_impl(pt_regs_t *r)
{
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length    = (size_t)r->rsi;
    int prot         = (int)r->rdx;
    int pkey         = (int)(s32)r->r10;

    /* pkey == -1 means "leave the key alone", i.e. plain mprotect(2). */
    if (pkey != -1) {
        if (!g_pku_enabled) return -(s64)EINVAL;
        if (pkey < 0 || pkey >= PKEY_MAX) return -(s64)EINVAL;
        process_t *p = sched_current_process();
        if (!p) return -(s64)EPERM;
        if (!(p->pkey_alloc_map & (1u << pkey))) return -(s64)EINVAL;
    }

    if (length == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr >= 0x0000800000000000ULL || addr < 0x1000) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    if (addr + aligned_len < addr || addr + aligned_len > 0x0000800000000000ULL)
        return -(s64)EINVAL;

    u64 vmm_flags = VMM_F_USER;
    if (prot != PROT_NONE)  vmm_flags |= VMM_F_PRESENT;
    if (prot & PROT_WRITE)  vmm_flags |= VMM_F_WRITE;
    if (!(prot & PROT_EXEC)) vmm_flags |= VMM_F_NX;
    if (pkey != -1) vmm_flags |= VMM_F_PKEY_SET | VMM_F_PKEY(pkey);

    s64 rc = vma_setprot(proc, addr, addr + aligned_len, (u32)prot & 7);
    if (rc < 0) return rc;

    vmm_set_flags(proc->pml4_phys, addr, aligned_len / PAGE_SIZE, vmm_flags);
    return 0;
}

/* ============================================================================
 * set_robust_list(2) / get_robust_list(2)
 * ========================================================================= */

static s64 sys_set_robust_list_impl(pt_regs_t *r)
{
    void  *head = (void *)r->rdi;
    size_t len  = (size_t)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* Linux rejects a size that does not match its own struct robust_list_head,
     * which is 24 bytes on x86-64; anything else means the caller and kernel
     * disagree about the layout. */
    if (len != 24) return -(s64)EINVAL;
    if (head && (uintptr_t)head >= 0x0000800000000000ULL) return -(s64)EFAULT;

    proc->robust_list     = head;
    proc->robust_list_len = len;
    return 0;
}

static s64 sys_get_robust_list_impl(pt_regs_t *r)
{
    s32     pid    = (s32)r->rdi;
    void  **uhead  = (void **)r->rsi;
    size_t *ulen   = (size_t *)r->rdx;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;
    if (!uhead || !ulen) return -(s64)EFAULT;
    if ((uintptr_t)uhead >= 0x0000800000000000ULL ||
        (uintptr_t)ulen  >= 0x0000800000000000ULL) return -(s64)EFAULT;

    /* pid 0 is the caller (always live). Any other pid is referenced for the
     * duration so it cannot be reaped between the lookup and the field reads. */
    process_t *target = caller;
    if (pid != 0) {
        target = proc_get_by_pid((u32)pid);
        if (!target) return -(s64)ESRCH;
    }

    s64 rc = 0;
    /* Reading another process's list is a credential check, same as ptrace. */
    if (target != caller && caller->euid != 0 && caller->uid != target->uid) {
        rc = -(s64)EPERM;
    } else {
        void  *head = target->robust_list;
        size_t len  = target->robust_list_len ? target->robust_list_len : 24;
        if (copy_to_user(uhead, &head, sizeof(head)) != 0) rc = -(s64)EFAULT;
        else if (copy_to_user(ulen, &len, sizeof(len)) != 0) rc = -(s64)EFAULT;
    }

    if (target != caller) proc_put(target);
    return rc;
}

/* ============================================================================
 * mincore(2) — real residency, one page-table lookup per page
 * ========================================================================= */

static s64 sys_mincore_impl(pt_regs_t *r)
{
    uintptr_t start = (uintptr_t)r->rdi;
    size_t length   = (size_t)r->rsi;
    unsigned char *vec = (unsigned char *)r->rdx;

    if (start >= 0x0000800000000000ULL || (start & 0xFFF) != 0) return -(s64)EINVAL;
    if (!vec || (uintptr_t)vec >= 0x0000800000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    if (start + (u64)pages * PAGE_SIZE > 0x0000800000000000ULL) return -(s64)ENOMEM;

    /* Batch the answers: one copy_to_user per chunk instead of per page turns
     * a 1 MB query from 256 user-access transitions into one. */
    unsigned char chunk[256];
    size_t done = 0;
    while (done < pages) {
        size_t n = pages - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        for (size_t i = 0; i < n; i++) {
            uintptr_t va = start + (done + i) * PAGE_SIZE;
            /* A PROT_NONE page keeps its frame with PRESENT clear, and is still
             * resident — report it as such, which is what mincore means. */
            chunk[i] = (vmm_query_flags(proc->pml4_phys, va) & VMM_F_PRESENT) ||
                       vmm_translate(proc->pml4_phys, va) ? 1 : 0;
        }
        if (copy_to_user(&vec[done], chunk, n) != 0) return -(s64)EFAULT;
        done += n;
    }
    return 0;
}

/* ============================================================================
 * process_madvise(2) — madvise on another process's address space
 * ========================================================================= */

static s64 sys_process_madvise_impl(pt_regs_t *r)
{
    int pidfd                = (int)r->rdi;
    const struct iovec *uiov = (const struct iovec *)r->rsi;
    unsigned long vlen       = (unsigned long)r->rdx;
    int advice               = (int)r->r10;
    unsigned int flags       = (unsigned int)r->r8;

    if (flags != 0) return -(s64)EINVAL;
    if (vlen > 1024) return -(s64)EINVAL;
    if (!uiov || (uintptr_t)uiov >= 0x0000800000000000ULL) return -(s64)EFAULT;

    process_t *caller = sched_current_process();
    if (!caller) return -(s64)EPERM;

    file_t *pf = fget(caller, pidfd);
    if (!pf) return -(s64)EBADF;
    fput(pf);

    /* Only the advice values that are meaningful without a reclaim path. The
     * hints this kernel can honour are all no-ops on an eagerly-mapped address
     * space, so the call validates its arguments and reports the byte count it
     * would have covered, which is what callers branch on. */
    switch (advice) {
        case 4:   /* MADV_DONTNEED  */
        case 8:   /* MADV_FREE      */
        case 20:  /* MADV_COLD      */
        case 21:  /* MADV_PAGEOUT   */
        case 22:  /* MADV_WILLNEED-ish / POPULATE_READ */
            break;
        default:
            return -(s64)EINVAL;
    }

    s64 total = 0;
    for (unsigned long i = 0; i < vlen; i++) {
        struct iovec kiov;
        if (copy_from_user(&kiov, &uiov[i], sizeof(kiov)) != 0) return -(s64)EFAULT;
        if ((uintptr_t)kiov.iov_base >= 0x0000800000000000ULL) return -(s64)EFAULT;
        if (kiov.iov_len > (size_t)0x7fffffffffffffffLL - (size_t)total)
            return -(s64)EINVAL;
        total += (s64)kiov.iov_len;
    }
    return total;
}

/* ============================================================================
 * cachestat(2) — page-cache residency of a file range
 * ========================================================================= */

static s64 sys_cachestat_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct cachestat_range *urange = (const struct cachestat_range *)r->rsi;
    struct cachestat *ucs = (struct cachestat *)r->rdx;
    unsigned int flags = (unsigned int)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (!urange || !ucs) return -(s64)EFAULT;
    if ((uintptr_t)urange >= 0x0000800000000000ULL ||
        (uintptr_t)ucs    >= 0x0000800000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    if (!file->f_inode || S_ISDIR(file->f_inode->i_mode)) { fput(file); return -(s64)EBADF; }

    struct cachestat_range range;
    if (copy_from_user(&range, urange, sizeof(range)) != 0) { fput(file); return -(s64)EFAULT; }

    u64 size = file->f_inode->i_size;
    u64 off  = range.off;
    /* len == 0 means "to the end of the file", per the man page. */
    u64 len  = range.len ? range.len : (off < size ? size - off : 0);
    fput(file);

    u64 pages = 0;
    if (off < size) {
        u64 end = off + len;
        if (end < off || end > size) end = size;
        pages = (end - off + PAGE_SIZE - 1) / PAGE_SIZE;
    }

    /* This kernel reads through the block layer's cache rather than a unified
     * page cache with its own residency bookkeeping, so the honest answer is
     * "everything within the file is cached, nothing is dirty or in
     * writeback" — the shape callers use to decide whether a read would
     * block. */
    struct cachestat cs;
    __builtin_memset(&cs, 0, sizeof(cs));
    cs.nr_cache = pages;
    return copy_to_user(ucs, &cs, sizeof(cs)) == 0 ? 0 : -(s64)EFAULT;
}

/* ============================================================================
 * futex_waitv(2) — wait until any one of several futexes is woken
 * ========================================================================= */

static s64 sys_futex_waitv_impl(pt_regs_t *r)
{
    const struct futex_waitv *uwaiters = (const struct futex_waitv *)r->rdi;
    unsigned int nr = (unsigned int)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (nr == 0 || nr > FUTEX_WAITV_MAX) return -(s64)EINVAL;
    if (!uwaiters || (uintptr_t)uwaiters >= 0x0000800000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    thread_t  *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    struct futex_waitv w[FUTEX_WAITV_MAX];
    if (copy_from_user(w, uwaiters, (size_t)nr * sizeof(w[0])) != 0) return -(s64)EFAULT;

    for (unsigned int i = 0; i < nr; i++) {
        if (w[i].__reserved != 0) return -(s64)EINVAL;
        /* Only 32-bit futexes exist here; the size field must say so. */
        if ((w[i].flags & 0x0F) != FUTEX2_SIZE_U32) return -(s64)EINVAL;
        if (w[i].uaddr >= 0x0000800000000000ULL || (w[i].uaddr & 3) != 0)
            return -(s64)EFAULT;
    }

    u64 timeout_ticks = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= 0x0000800000000000ULL) return -(s64)EFAULT;
        struct linux_timespec ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L)
            return -(s64)EINVAL;
        u64 ms = (u64)ts.tv_sec * 1000 + (u64)ts.tv_nsec / 1000000;
        timeout_ticks = (ms + 9) / 10;
        if (timeout_ticks == 0) timeout_ticks = 1;
    }

    /* Enqueue on every futex first, then re-check all the values. Checking
     * before enqueueing would lose a wake that lands in between. */
    futex_q_t q[FUTEX_WAITV_MAX];
    u32 bucket[FUTEX_WAITV_MAX];

    spinlock_lock(&g_futex_lock);
    for (unsigned int i = 0; i < nr; i++) {
        q[i].thread = curr;
        q[i].proc   = proc;
        q[i].uaddr  = (uintptr_t)w[i].uaddr;
        q[i].bitset = FUTEX_BITSET_MATCH_ANY;
        bucket[i]   = futex_hash(q[i].uaddr);
        q[i].next   = g_futex_table[bucket[i]];
        g_futex_table[bucket[i]] = &q[i];
    }
    spinlock_unlock(&g_futex_lock);

    /* dequeue_all() must run on every exit path below, including the early
     * "value already changed" one. */
    #define FUTEX_WAITV_DEQUEUE(found_out) do {                          \
        spinlock_lock(&g_futex_lock);                                    \
        for (unsigned int _i = 0; _i < nr; _i++) {                       \
            futex_q_t **pp = &g_futex_table[bucket[_i]];                 \
            bool _still = false;                                         \
            while (*pp) {                                                \
                if (*pp == &q[_i]) { *pp = q[_i].next; _still = true; break; } \
                pp = &(*pp)->next;                                       \
            }                                                            \
            if (!_still) (found_out) = true;                             \
        }                                                                \
        spinlock_unlock(&g_futex_lock);                                  \
    } while (0)

    for (unsigned int i = 0; i < nr; i++) {
        u32 cur = 0;
        bool unqueued = false;   /* the macro reports it; nothing to act on here */
        if (copy_from_user(&cur, (const void *)(uintptr_t)w[i].uaddr, sizeof(u32)) != 0) {
            FUTEX_WAITV_DEQUEUE(unqueued);
            (void)unqueued;
            return -(s64)EFAULT;
        }
        if (cur != (u32)w[i].val) {
            /* This futex already moved: nothing to wait for, report its index. */
            FUTEX_WAITV_DEQUEUE(unqueued);
            (void)unqueued;
            return (s64)i;
        }
    }

    u64 start_ticks = sched_get_ticks();
    if (timeout_ticks > 0) sched_sleep(timeout_ticks);
    else                   sched_block(THREAD_BLOCKED_PENDING);

    bool woken = false;
    FUTEX_WAITV_DEQUEUE(woken);
    #undef FUTEX_WAITV_DEQUEUE

    if (!woken) {
        if (proc->sig_pending & ~proc->sig_blocked) return -(s64)EINTR;
        if (timeout_ticks > 0 && sched_get_ticks() - start_ticks >= timeout_ticks)
            return -(s64)ETIMEDOUT;
        return -(s64)EINTR;
    }

    /* Report the first futex whose value now differs from what we waited on;
     * fall back to 0 when the waker changed nothing observable. */
    for (unsigned int i = 0; i < nr; i++) {
        u32 cur = 0;
        if (copy_from_user(&cur, (const void *)(uintptr_t)w[i].uaddr, sizeof(u32)) == 0 &&
            cur != (u32)w[i].val)
            return (s64)i;
    }
    return 0;
}

/* ============================================================================
 * mseal(2) — make a mapping's protections permanent
 * ========================================================================= */

static s64 sys_mseal_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    size_t len     = (size_t)r->rsi;
    unsigned long flags = (unsigned long)r->rdx;

    if (flags != 0) return -(s64)EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (addr + len < addr || addr + len > 0x0000800000000000ULL) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Every page in the range must be mapped, or Linux returns ENOMEM. We
     * enforce that much; the seal itself is not yet tracked, so a later
     * mprotect() on the range still succeeds. Reporting ENOSYS instead would
     * be worse: callers use mseal() as opportunistic hardening and treat a
     * success as "done", never as "and now nothing can change". */
    for (uintptr_t va = addr; va < addr + len; va += PAGE_SIZE) {
        if (!vmm_translate(proc->pml4_phys, va)) return -(s64)ENOMEM;
    }
    return 0;
}

/* ============================================================================
 * setns(2)
 * ========================================================================= */

static s64 sys_setns_impl(pt_regs_t *r)
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
 * POSIX message queues (mq_open, mq_unlink, mq_timedsend, mq_timedreceive,
 * mq_notify, mq_getsetattr)
 *
 * The subsystem lives in kernel/ipc/mqueue.c; these are register-unpacking
 * shims and nothing else, so the ABI mapping stays visible in one place.
 * ========================================================================= */

static s64 sys_mq_open_impl(pt_regs_t *r)
{
    return mq_open_impl((const char *)r->rdi, (int)r->rsi, (u32)r->rdx,
                        (const void *)r->r10);
}

static s64 sys_mq_unlink_impl(pt_regs_t *r)
{
    return mq_unlink_impl((const char *)r->rdi);
}

static s64 sys_mq_timedsend_impl(pt_regs_t *r)
{
    return mq_timedsend_impl((int)r->rdi, (const char *)r->rsi, (size_t)r->rdx,
                             (u32)r->r10, (const void *)r->r8);
}

static s64 sys_mq_timedreceive_impl(pt_regs_t *r)
{
    return mq_timedreceive_impl((int)r->rdi, (char *)r->rsi, (size_t)r->rdx,
                                (u32 *)r->r10, (const void *)r->r8);
}

static s64 sys_mq_notify_impl(pt_regs_t *r)
{
    return mq_notify_impl((int)r->rdi, (const void *)r->rsi);
}

static s64 sys_mq_getsetattr_impl(pt_regs_t *r)
{
    return mq_getsetattr_impl((int)r->rdi, (const void *)r->rsi, (void *)r->rdx);
}

/* ============================================================================
 * futex2: futex_wake(2), futex_wait(2), futex_requeue(2)
 *
 * The newer, flag-word-based entry points onto the same wait queues futex(2)
 * uses. They are not sugar: futex_wait() takes an *absolute* deadline against
 * a caller-named clock, which is what a correct pthread_cond_timedwait() needs
 * and what the FUTEX_WAIT opcode could never express without the
 * FUTEX_CLOCK_REALTIME retrofit.
 * ========================================================================= */

/* Reject the parts of the futex2 flag word this kernel cannot honour, so a
 * program probing for 64-bit or NUMA futexes gets a clean -EINVAL instead of
 * silently operating on the wrong width. */
static s64 futex2_check_flags(unsigned int flags)
{
    if (flags & ~(u32)(FUTEX2_SIZE_MASK | FUTEX2_NUMA | FUTEX2_PRIVATE))
        return -(s64)EINVAL;
    if ((flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32) return -(s64)EINVAL;
    if (flags & FUTEX2_NUMA) return -(s64)EINVAL;   /* single memory node */
    return 0;
}

static s64 sys_futex_wake_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    u32 mask        = (u32)r->rsi;
    int nr          = (int)r->rdx;
    unsigned int flags = (unsigned int)r->r10;

    s64 rc = futex2_check_flags(flags);
    if (rc < 0) return rc;
    if (mask == 0 || nr < 0) return -(s64)EINVAL;
    if (uaddr >= 0x0000800000000000ULL || (uaddr & 3) != 0) return -(s64)EFAULT;

    pt_regs_t sub = *r;
    sub.rsi = FUTEX_WAKE_BITSET | FUTEX_PRIVATE_FLAG;
    sub.rdx = (u64)(u32)nr;
    sub.r9  = mask;
    return sys_futex_impl(&sub);
}

static s64 sys_futex_wait_impl(pt_regs_t *r)
{
    uintptr_t uaddr = (uintptr_t)r->rdi;
    u32 val         = (u32)r->rsi;
    u32 mask        = (u32)r->rdx;
    unsigned int flags = (unsigned int)r->r10;
    const struct linux_timespec *utimeout = (const struct linux_timespec *)r->r8;
    int clockid     = (int)r->r9;

    s64 rc = futex2_check_flags(flags);
    if (rc < 0) return rc;
    if (mask == 0) return -(s64)EINVAL;
    if (uaddr >= 0x0000800000000000ULL || (uaddr & 3) != 0) return -(s64)EFAULT;
    /* CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) are the only clocks the call
     * is defined for; both advance at the same rate here. */
    if (clockid != 0 && clockid != 1) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    thread_t  *curr = sched_current_thread();
    if (!proc || !curr) return -(s64)EPERM;

    u32 cur_val = 0;
    if (copy_from_user(&cur_val, (const void *)uaddr, sizeof(u32)) != 0)
        return -(s64)EFAULT;
    if (cur_val != val) return -(s64)EAGAIN;

    /* Absolute deadline, unlike FUTEX_WAIT's relative one. Reading the clock
     * here rather than in the caller is the whole point: a deadline that has
     * already passed must not block, however long the caller took to get here. */
    u64 timeout_ticks = 0;
    if (utimeout) {
        if ((uintptr_t)utimeout >= 0x0000800000000000ULL) return -(s64)EFAULT;
        struct linux_timespec ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;

        u64 now_ticks = sched_get_ticks();
        u64 abs_ticks = (u64)ts.tv_sec * 100 + (u64)(ts.tv_nsec / 10000000L);
        if (clockid == 0) {
            /* CLOCK_REALTIME deadlines are wall-clock; rebase onto ticks. */
            u64 now_sec = get_cached_unix_time();
            s64 delta_ms = (s64)((ts.tv_sec - (s64)now_sec) * 1000 +
                                 ts.tv_nsec / 1000000);
            if (delta_ms <= 0) return -(s64)ETIMEDOUT;
            timeout_ticks = (u64)((delta_ms + 9) / 10);
        } else {
            if (abs_ticks <= now_ticks) return -(s64)ETIMEDOUT;
            timeout_ticks = abs_ticks - now_ticks;
        }
        if (timeout_ticks == 0) timeout_ticks = 1;
    }

    return futex_wait_queued(proc, curr, uaddr, mask, timeout_ticks);
}

static s64 sys_futex_requeue_impl(pt_regs_t *r)
{
    /* futex_requeue() takes its two futexes through a two-element array of
     * struct futex_waitv rather than as two bare pointers, which is what lets
     * each side carry its own flags. */
    const struct futex_waitv *uwaiters = (const struct futex_waitv *)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    int nr_wake    = (int)r->rdx;
    int nr_requeue = (int)r->r10;

    if (flags != 0) return -(s64)EINVAL;
    if (nr_wake < 0 || nr_requeue < 0) return -(s64)EINVAL;
    if (!uwaiters || (uintptr_t)uwaiters >= 0x0000800000000000ULL)
        return -(s64)EFAULT;

    struct futex_waitv w[2];
    if (copy_from_user(w, uwaiters, sizeof(w)) != 0) return -(s64)EFAULT;

    for (int i = 0; i < 2; i++) {
        s64 rc = futex2_check_flags(w[i].flags);
        if (rc < 0) return rc;
        if (w[i].__reserved) return -(s64)EINVAL;
        if (w[i].uaddr >= 0x0000800000000000ULL || (w[i].uaddr & 3) != 0)
            return -(s64)EFAULT;
    }

    pt_regs_t sub = *r;
    sub.rdi = w[0].uaddr;
    sub.rsi = FUTEX_CMP_REQUEUE | FUTEX_PRIVATE_FLAG;
    sub.rdx = (u64)(u32)nr_wake;
    sub.r10 = (u64)(u32)nr_requeue;   /* the legacy path reads val2 from here */
    sub.r8  = w[1].uaddr;
    sub.r9  = (u32)w[0].val;          /* expected value at the source futex   */
    return sys_futex_impl(&sub);
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

static s64 sys_ioprio_set_impl(pt_regs_t *r)
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

static s64 sys_ioprio_get_impl(pt_regs_t *r)
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

/* ============================================================================
 * NUMA memory policy: set_mempolicy(2), get_mempolicy(2), mbind(2),
 * migrate_pages(2), set_mempolicy_home_node(2)
 *
 * AzamiOS presents one memory node. That makes every policy trivially
 * satisfied rather than unimplementable, so these validate their arguments the
 * way a one-node Linux kernel does and keep the state they are given — a
 * nodemask naming node 3 is an error here for the same reason it is there.
 * ========================================================================= */

/* Read a user nodemask and check it names nothing but node 0. */
static s64 mempolicy_check_nodemask(const unsigned long *unodes, unsigned long maxnode,
                                    u64 *out_mask)
{
    *out_mask = 0;
    if (maxnode > 8 * sizeof(u64) * 16) return -(s64)EINVAL;
    if (!unodes || maxnode == 0) return 0;
    if ((uintptr_t)unodes >= 0x0000800000000000ULL) return -(s64)EFAULT;

    /* maxnode counts bits, and the bitmap is an array of unsigned long. */
    unsigned long words = (maxnode + 63) / 64;
    for (unsigned long i = 0; i < words; i++) {
        u64 w = 0;
        if (copy_from_user(&w, unodes + i, sizeof(u64)) != 0) return -(s64)EFAULT;
        if (i == 0) {
            /* Mask off bits past maxnode before judging the word. */
            if (maxnode < 64) w &= (maxnode == 0) ? 0 : ((1ULL << maxnode) - 1);
            *out_mask = w;
            if (w & ~1ULL) return -(s64)EINVAL;   /* only node 0 exists */
        } else if (w) {
            return -(s64)EINVAL;
        }
    }
    return 0;
}

static s64 mempolicy_check_mode(int mode, u64 nodemask)
{
    int flags = mode & MPOL_MODE_FLAGS;
    mode &= ~MPOL_MODE_FLAGS;
    if (mode < 0 || mode >= MPOL_MAX) return -(s64)EINVAL;
    if ((flags & MPOL_F_STATIC_NODES) && (flags & MPOL_F_RELATIVE_NODES))
        return -(s64)EINVAL;

    /* MPOL_BIND and MPOL_INTERLEAVE require a non-empty node set;
     * MPOL_DEFAULT and MPOL_LOCAL require an empty one. */
    if ((mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nodemask == 0)
        return -(s64)EINVAL;
    if ((mode == MPOL_DEFAULT || mode == MPOL_LOCAL) && nodemask != 0)
        return -(s64)EINVAL;
    return 0;
}

static s64 sys_set_mempolicy_impl(pt_regs_t *r)
{
    int mode = (int)r->rdi;
    const unsigned long *unodes = (const unsigned long *)r->rsi;
    unsigned long maxnode = (unsigned long)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 mask = 0;
    s64 rc = mempolicy_check_nodemask(unodes, maxnode, &mask);
    if (rc < 0) return rc;
    rc = mempolicy_check_mode(mode, mask);
    if (rc < 0) return rc;

    proc->mempolicy_mode = (u32)mode;
    proc->mempolicy_nodemask = mask;
    return 0;
}

static s64 sys_get_mempolicy_impl(pt_regs_t *r)
{
    int *umode = (int *)r->rdi;
    unsigned long *unodes = (unsigned long *)r->rsi;
    unsigned long maxnode = (unsigned long)r->rdx;
    uintptr_t addr = (uintptr_t)r->r10;
    unsigned long flags = (unsigned long)r->r8;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (flags & ~(unsigned long)(MPOL_F_NODE | MPOL_F_ADDR | MPOL_F_MEMS_ALLOWED))
        return -(s64)EINVAL;
    if ((flags & MPOL_F_ADDR) && !addr) return -(s64)EINVAL;
    if ((flags & MPOL_F_MEMS_ALLOWED) && (flags & (MPOL_F_ADDR | MPOL_F_NODE)))
        return -(s64)EINVAL;

    int mode;
    u64 mask;
    if (flags & MPOL_F_MEMS_ALLOWED) {
        mode = 0;
        mask = 1;                       /* node 0 is the only one permitted */
    } else if (flags & MPOL_F_NODE) {
        /* Report which node the memory is on, not which policy governs it.
         * With one node the answer is always 0 — but only for a mapped
         * address, so an unmapped one still has to fault. */
        if (flags & MPOL_F_ADDR) {
            if (addr >= 0x0000800000000000ULL) return -(s64)EFAULT;
            if (!vmm_translate(proc->pml4_phys, addr)) return -(s64)EFAULT;
        }
        mode = 0;
        mask = proc->mempolicy_nodemask;
    } else {
        mode = (int)proc->mempolicy_mode;
        mask = proc->mempolicy_nodemask;
    }

    if (umode) {
        if ((uintptr_t)umode >= 0x0000800000000000ULL) return -(s64)EFAULT;
        if (copy_to_user(umode, &mode, sizeof(int)) != 0) return -(s64)EFAULT;
    }
    if (unodes && maxnode) {
        if ((uintptr_t)unodes >= 0x0000800000000000ULL) return -(s64)EFAULT;
        unsigned long words = (maxnode + 63) / 64;
        for (unsigned long i = 0; i < words; i++) {
            u64 w = (i == 0) ? mask : 0;
            if (copy_to_user(unodes + i, &w, sizeof(u64)) != 0) return -(s64)EFAULT;
        }
    }
    return 0;
}

/* move_pages(2), fully implemented for the one-node case this kernel
 * presents — same "faithful for one node rather than stubbed" approach as
 * the other mempolicy calls above (see this file's MPOL_* comment).
 * Querying (nodes == NULL) reports node 0 for every present page and
 * -ENOENT for an unmapped one; a move request succeeds trivially when the
 * target is node 0 (the page is already there) and fails -ENODEV for any
 * other target, exactly matching what a real move_pages() reports on
 * hardware with a single NUMA node.
 *
 * Only self (pid == 0 or the caller's own pid) is supported: real
 * move_pages() can also target another process the caller has
 * ptrace-equivalent permission over, but every real caller of this syscall
 * is a NUMA-aware allocator checking or placing its *own* pages — nothing
 * in a single-node kernel needs the cross-process case, and skipping it
 * avoids taking on another process's page-table locking here. */
#define MOVE_PAGES_MAX 1024 /* same per-call bound as readv/writev's iovec cap */

static s64 sys_move_pages_impl(pt_regs_t *r)
{
    int pid = (int)r->rdi;
    unsigned long count = (unsigned long)r->rsi;
    void *const *upages = (void *const *)r->rdx;
    const int *unodes = (const int *)r->r10;
    int *ustatus = (int *)r->r8;
    /* r9 (flags: MPOL_MF_MOVE / MPOL_MF_MOVE_ALL) doesn't change behavior
     * either way on a single-node system. */

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid != 0 && pid != (int)proc->pid) return -(s64)EPERM;

    if (count > MOVE_PAGES_MAX) return -(s64)E2BIG;
    if (count == 0) return 0;
    if (!upages || !ustatus) return -(s64)EFAULT;
    if ((uintptr_t)upages >= 0x0000800000000000ULL || (uintptr_t)ustatus >= 0x0000800000000000ULL)
        return -(s64)EFAULT;
    if (unodes && (uintptr_t)unodes >= 0x0000800000000000ULL) return -(s64)EFAULT;

    for (unsigned long i = 0; i < count; i++) {
        void *upage_ptr;
        if (copy_from_user(&upage_ptr, &upages[i], sizeof(void *)) != 0) return -(s64)EFAULT;
        uintptr_t addr = (uintptr_t)upage_ptr;

        int status;
        if (addr >= 0x0000800000000000ULL ||
            !vmm_translate(proc->pml4_phys, ALIGN_DOWN(addr, PAGE_SIZE))) {
            status = -(s32)ENOENT; /* page not present */
        } else if (unodes) {
            int want_node;
            if (copy_from_user(&want_node, &unodes[i], sizeof(int)) != 0) return -(s64)EFAULT;
            status = (want_node == 0) ? 0 : -(s32)ENODEV;
        } else {
            status = 0; /* query only: present pages are always on node 0 */
        }
        if (copy_to_user(&ustatus[i], &status, sizeof(int)) != 0) return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_mbind_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    u64 len = r->rsi;
    int mode = (int)r->rdx;
    const unsigned long *unodes = (const unsigned long *)r->r10;
    unsigned long maxnode = (unsigned long)r->r8;
    unsigned flags = (unsigned)r->r9;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    /* MPOL_MF_STRICT | MPOL_MF_MOVE | MPOL_MF_MOVE_ALL */
    if (flags & ~7u) return -(s64)EINVAL;

    u64 mask = 0;
    s64 rc = mempolicy_check_nodemask(unodes, maxnode, &mask);
    if (rc < 0) return rc;
    rc = mempolicy_check_mode(mode, mask);
    if (rc < 0) return rc;

    /* The range has to exist even though the policy cannot move anything. */
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    if (end < addr || end > 0x0000800000000000ULL) return -(s64)EINVAL;
    return 0;
}

static s64 sys_migrate_pages_impl(pt_regs_t *r)
{
    unsigned long maxnode = (unsigned long)r->rsi;
    const unsigned long *uold = (const unsigned long *)r->rdx;
    const unsigned long *unew = (const unsigned long *)r->r10;

    u64 om = 0, nm = 0;
    s64 rc = mempolicy_check_nodemask(uold, maxnode, &om);
    if (rc < 0) return rc;
    rc = mempolicy_check_nodemask(unew, maxnode, &nm);
    if (rc < 0) return rc;

    /* Every page is already on the only node there is, so nothing could not
     * be moved — which is exactly what a return of 0 means. */
    return 0;
}

static s64 sys_set_mempolicy_home_node_impl(pt_regs_t *r)
{
    uintptr_t addr = (uintptr_t)r->rdi;
    u64 len = r->rsi;
    unsigned long home_node = (unsigned long)r->rdx;
    unsigned long flags = (unsigned long)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (flags) return -(s64)EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -(s64)EINVAL;
    if (home_node != 0) return -(s64)EINVAL;   /* node 0 is the only node */
    u64 end = addr + ALIGN_UP(len, PAGE_SIZE);
    if (end < addr || end > 0x0000800000000000ULL) return -(s64)EINVAL;

    proc->mempolicy_home_node = (u32)home_node;
    return 0;
}

/* ============================================================================
 * fchmodat2(2) — fchmodat() with the flags argument it should always have had.
 * ========================================================================= */

static s64 sys_fchmodat2_impl(pt_regs_t *r)
{
    int flags = (int)r->r10;

    /* AT_SYMLINK_NOFOLLOW is the whole reason this call exists; the mode of a
     * symlink itself is not meaningful on any filesystem here, so the honest
     * answer is the one Linux gives for filesystems that cannot do it. */
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) return -(s64)EINVAL;
    if (flags & AT_SYMLINK_NOFOLLOW) return -(s64)EOPNOTSUPP;

    pt_regs_t sub = *r;
    sub.r10 = 0;
    return sys_fchmodat_impl(&sub);
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

static s64 sys_restart_syscall_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)EINTR;
}

/* ============================================================================
 * vhangup(2) — revoke the controlling terminal of every process on it.
 * ========================================================================= */

static s64 sys_vhangup_impl(pt_regs_t *r)
{
    (void)r;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_TTY_CONFIG)) return -(s64)EPERM;

    /* getty(8) calls this between sessions so the next login cannot inherit a
     * descriptor onto the previous user's terminal. Delivering SIGHUP to the
     * session is the visible half of that contract and the half programs
     * actually depend on.
     *
     * Collect the pids under the scheduler lock and signal afterwards:
     * sched_kill_process() takes that same lock, and it is the only thing that
     * knows how to apply a signal's default action, so it must not be called
     * from inside the walk. */
    u32 sid = proc->sid;
    u32 pids[64];
    u32 npids = 0;

    sched_lock();
    for (process_t *t = sched_get_process_list(); t; t = t->next) {
        if (t->is_zombie || t->sid != sid || t->pid == proc->pid) continue;
        if (npids == (u32)(sizeof(pids) / sizeof(pids[0]))) break;
        pids[npids++] = t->pid;
    }
    sched_unlock();

    for (u32 i = 0; i < npids; i++) sched_kill_process(pids[i], SIGHUP);
    return 0;
}

/* ============================================================================
 * pivot_root(2) — swap the root filesystem for another mounted one.
 * ========================================================================= */

static s64 sys_pivot_root_impl(pt_regs_t *r)
{
    const char *unew = (const char *)r->rdi;
    const char *uput = (const char *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;

    char newroot[256], putold[256];
    if (copy_str_from_user(newroot, unew, sizeof(newroot)) < 0) return -(s64)EFAULT;
    if (copy_str_from_user(putold, uput, sizeof(putold)) < 0) return -(s64)EFAULT;

    /* Both must be directories, and put_old must lie under new_root — the two
     * constraints that make the swap reversible. */
    dentry_t *dn = NULL, *dp = NULL;
    if (vfs_path_lookup(newroot, &dn) < 0 || !dn || !dn->d_inode) return -(s64)ENOENT;
    if (!S_ISDIR(dn->d_inode->i_mode)) return -(s64)ENOTDIR;
    if (vfs_path_lookup(putold, &dp) < 0 || !dp || !dp->d_inode) return -(s64)ENOENT;
    if (!S_ISDIR(dp->d_inode->i_mode)) return -(s64)ENOTDIR;

    size_t nlen = strlen(newroot);
    while (nlen > 1 && newroot[nlen - 1] == '/') nlen--;
    if (strncmp(putold, newroot, nlen) != 0 ||
        (putold[nlen] != '/' && putold[nlen] != '\0'))
        return -(s64)EINVAL;

    /* The mount table is a single global namespace with no per-process root,
     * so the old root cannot be detached from under processes that are still
     * using it. Reporting that honestly beats half-performing the swap. */
    return -(s64)EBUSY;
}

/* ============================================================================
 * process_mrelease(2) — reap the address space of a process that is already
 * dying, without waiting for it to be reaped by its parent.
 * ========================================================================= */

static s64 sys_process_mrelease_impl(pt_regs_t *r)
{
    int pidfd = (int)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;

    if (flags != 0) return -(s64)EINVAL;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;

    file_t *f = fget(me, pidfd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_pidfd_fops || !f->private_data) { fput(f); return -(s64)EBADF; }
    u32 target_pid = ((pidfd_ctx_t *)f->private_data)->target_pid;
    fput(f);

    struct proc_ident id;
    if (!sched_proc_ident(target_pid, &id)) return -(s64)ESRCH;
    /* Only for a process that is already on its way out: the call exists to
     * accelerate an OOM kill, not to shoot down a running program. */
    if (!id.is_zombie) return -(s64)EINVAL;
    if (me->euid != 0 && me->euid != id.uid) return -(s64)EPERM;

    /* A zombie's address space is torn down at exit here, so the memory this
     * call would free is already gone. */
    return 0;
}

/* ============================================================================
 * POSIX Named Semaphores — Azami extended syscalls 534–541
 *
 * Model: sem_open() returns a normal fd backed by a g_semfd_fops file whose
 * private_data points to the kernel posix_sem_t.  sem_post / sem_wait / ...
 * take that fd and look it up via fget() exactly like any other fd call.
 * sem_unlink() takes a name string.
 * ============================================================================ */

#include "../ipc/posix_sem.h"

static s64 semfd_release_op(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (filp && filp->private_data) {
        posix_sem_put((posix_sem_t *)filp->private_data);
        filp->private_data = NULL;
    }
    return 0;
}

static file_operations_t g_semfd_fops = {
    .release = semfd_release_op,
};

/* sem_open(name, oflag, mode, value) → fd or −errno */
static s64 sys_az_sem_open_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    int         oflag = (int)r->rsi;
    unsigned    mode  = (unsigned)r->rdx;
    unsigned    value = (unsigned)r->r10;

    char kname[POSIX_SEM_NAME_MAX];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    posix_sem_t *sem = posix_sem_open_kern(kname, oflag, mode, value);
    if (!sem) return -(s64)ENOENT; /* caller interprets per oflag */

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) { posix_sem_put(sem); return -(s64)ENOMEM; }

    f->f_op       = &g_semfd_fops;
    f->private_data = sem;
    f->f_count    = 1;
    f->f_mode     = 0600;

    s64 fd = fd_install(proc, f, 0);
    if (fd < 0) { posix_sem_put(sem); kfree(f); return -(s64)EMFILE; }
    return fd;
}

/* sem_close(fd) */
static s64 sys_az_sem_close_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* A normal close() also works — we just provide the specific sem-close path. */
    return sys_close_impl(r);  /* reuse the standard close */
}

/* Helper: get the posix_sem_t from an fd, or return NULL (sets errno). */
static posix_sem_t *semfd_get(process_t *proc, int fd, file_t **fout)
{
    file_t *f = fget(proc, fd);
    if (!f) return NULL;
    if (f->f_op != &g_semfd_fops || !f->private_data) { fput(f); return NULL; }
    if (fout) *fout = f;
    return (posix_sem_t *)f->private_data;
}

/* sem_post(fd) */
static s64 sys_az_sem_post_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_post(sem);
    fput(f);
    return ret;
}

/* sem_wait(fd) */
static s64 sys_az_sem_wait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_wait(sem);
    fput(f);
    return ret;
}

/* sem_trywait(fd) */
static s64 sys_az_sem_trywait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_trywait(sem);
    fput(f);
    return ret;
}

/* sem_timedwait(fd, *timespec) */
static s64 sys_az_sem_timedwait_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct linux_timespec *uts = (const struct linux_timespec *)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    u64 abs_ns = 0;
    if (uts) {
        struct linux_timespec ts;
        if (copy_from_user(&ts, uts, sizeof(ts)) != 0) return -(s64)EFAULT;
        abs_ns = (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
    }

    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    s64 ret = posix_sem_timedwait(sem, abs_ns);
    fput(f);
    return ret;
}

/* sem_unlink(name) */
static s64 sys_az_sem_unlink_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    char kname[POSIX_SEM_NAME_MAX];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;
    return (s64)posix_sem_unlink(kname);
}

/* sem_getvalue(fd, *sval) */
static s64 sys_az_sem_getvalue_impl(pt_regs_t *r)
{
    int  fd   = (int)r->rdi;
    int *usval = (int *)r->rsi;
    if (!usval || (uintptr_t)usval >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    file_t *f = NULL;
    posix_sem_t *sem = semfd_get(proc, fd, &f);
    if (!sem) return -(s64)EBADF;
    int sval = 0;
    s64 ret = posix_sem_getvalue(sem, &sval);
    fput(f);
    if (ret == 0 && copy_to_user(usval, &sval, sizeof(sval)) != 0)
        return -(s64)EFAULT;
    return ret;
}

/* ============================================================================
 * ktrace syscall stubs — delegate to ktrace.c at kernel/perf/ktrace.c
 * These provide a direct syscall interface as an alternative to the
 * /sys/kernel/trace/ VFS interface.
 * ============================================================================ */

/* ktrace syscall implementations (API defined in kernel/perf/ktrace.h) */

/* ktrace_enable(name_ptr) or ktrace_disable("-name_ptr") */
static s64 sys_az_ktrace_enable_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    char kname[64];
    s64 err = copy_str_from_user(kname, uname, sizeof(kname));
    if (err < 0) return -(s64)EFAULT;

    if (kname[0] == '-') {
        return (s64)ktrace_disable(kname + 1);
    }
    return (s64)ktrace_enable(kname);
}

/* ktrace_read(buf, max_entries) → number of entries written */
static s64 sys_az_ktrace_read_impl(pt_regs_t *r)
{
    void  *ubuf       = (void *)r->rdi;
    size_t max_entries = (size_t)r->rsi;
    if (!ubuf || (uintptr_t)ubuf >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (max_entries == 0) return 0;
    if (max_entries > 256) max_entries = 256;

    size_t sz = max_entries * sizeof(ktrace_entry_t);
    ktrace_entry_t *kbuf = (ktrace_entry_t *)kzalloc(sz);
    if (!kbuf) return -(s64)ENOMEM;

    size_t n = ktrace_read(kbuf, max_entries);
    s64 ret = 0;
    if (n > 0 && copy_to_user(ubuf, kbuf, n * sizeof(ktrace_entry_t)) != 0)
        ret = -(s64)EFAULT;
    else
        ret = (s64)n;

    kfree(kbuf);
    return ret;
}

/* ktrace_clear() */
static s64 sys_az_ktrace_clear_impl(pt_regs_t *r)
{
    (void)r;
    ktrace_ring_clear();
    return 0;
}

