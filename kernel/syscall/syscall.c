/* ============================================================================
 * AzamiOS — System Call Dispatcher Implementation
 * File: kernel/syscall/syscall.c
 *
 * Full POSIX x86_64 ABI System Call Implementation with argument parsing,
 * network sockets, polling, System V stack construction, and telemetry.
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "syscall.h"
#include "../../drivers/char/console.h"
#include "../../drivers/char/uart.h"
#include "../../drivers/input/input.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/sched/elf.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/ipc/ipc.h"
#include "../../kernel/object/object.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../../drivers/misc/bga.h"
#include "../../include/azami/defs.h"
#include "../../kernel/uaccess.h"
#include "../../fs/pipe.h"
#include "../../drivers/acpi/power.h"
#include "../../drivers/misc/rtc.h"
#include "../../include/azami/net.h"
#include "../../include/azami/socket.h"
#include "../security/acl.h"
#include "../security/security.h"
#include "../../arch/x86_64/cpu/msr.h"
#include "../../arch/x86_64/cpu/smp.h"

typedef s64 (*syscall_fn_t)(pt_regs_t *r);


#define SYSCALL_TABLE_SIZE  540
static syscall_fn_t g_syscall_table[SYSCALL_TABLE_SIZE];

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
static s64 sys_rt_sigreturn_impl(pt_regs_t *r);
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
static s64 sys_set_robust_list_impl(pt_regs_t *r);
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
static s64 sys_mincore_impl(pt_regs_t *r);
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


void syscall_dispatch(pt_regs_t *regs)
{
    u64 nr = regs->rax;

    if (unlikely(nr >= SYSCALL_TABLE_SIZE) || !g_syscall_table[nr]) {
        regs->rax = (u64)(-(s64)ENOSYS);
        return;
    }

    regs->rax = (u64)g_syscall_table[nr](regs);
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
    reg(SYS_sendmsg,       sys_sendto_impl);
    reg(SYS_recvmsg,       sys_recvfrom_impl);
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
    reg(SYS_getfacl,       sys_getfacl_impl);
    reg(SYS_setfacl,       sys_setfacl_impl);
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

    pr_debug("[SYSCALL] Dispatch table ready (%d entries)\n", SYSCALL_TABLE_SIZE);
}

/* ── Path Resolution Helper ──────────────────────────────────────────────── */

static s64 copy_str_from_user(char *dst, const char *user_src, size_t max_len)
{
    if (!dst || !user_src || max_len == 0) return -(s64)EINVAL;
    if ((uintptr_t)user_src >= 0x0000800000000000ULL) return -(s64)EFAULT;

    size_t copied = 0;
    while (copied < max_len - 1) {
        char c = '\0';
        if (copy_from_user(&c, user_src + copied, 1) != 0) {
            return -(s64)EFAULT;
        }
        dst[copied] = c;
        if (c == '\0') {
            return (s64)copied;
        }
        copied++;
    }
    dst[max_len - 1] = '\0';
    return (s64)(max_len - 1);
}

static s64 copy_user_path_resolve_at(int dirfd, char *kpath, size_t max_len, const char *user_path)
{
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char raw[512];
    __builtin_memset(raw, 0, sizeof(raw));
    s64 slen = copy_str_from_user(raw, user_path, sizeof(raw));
    if (slen < 0) return slen;

    if (raw[0] == '/') {
        return vfs_resolve_path("/", raw, kpath, max_len);
    }

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (dirfd == AT_FDCWD) {
        const char *cwd = (proc->cwd[0]) ? proc->cwd : "/";
        return vfs_resolve_path(cwd, raw, kpath, max_len);
    }

    if (dirfd < 0 || dirfd >= 64 || !proc->handle_table[dirfd]) return -(s64)EBADF;
    file_t *df = (file_t *)proc->handle_table[dirfd];
    if (!df || !df->f_dentry || !df->f_inode) return -(s64)EBADF;
    if (!S_ISDIR(df->f_inode->i_mode)) return -(s64)ENOTDIR;

    char dir_path[512];
    __builtin_memset(dir_path, 0, sizeof(dir_path));
    dentry_build_path(df->f_dentry, dir_path, sizeof(dir_path));

    return vfs_resolve_path(dir_path, raw, kpath, max_len);
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
    file_t *file = NULL;
    if (proc && fd >= 0 && fd < 64 && proc->handle_table[fd]) {
        uintptr_t addr = (uintptr_t)proc->handle_table[fd];
        if (addr >= 0xFFFF800000000000ULL) {
            file = (file_t *)proc->handle_table[fd];
        }
    }

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
            if (total_read == 0) return ret;
            break;
        }
        if (ret == 0) break;
        if (copy_to_user(buf + total_read, kbuf, (size_t)ret) != 0) {
            if (total_read == 0) return -(s64)EFAULT;
            break;
        }
        total_read += ret;
        count -= ret;
        if (ret < (s64)chunk) break;
    }
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
    file_t *file = NULL;
    if (proc && fd >= 0 && fd < 64 && proc->handle_table[fd]) {
        uintptr_t addr = (uintptr_t)proc->handle_table[fd];
        if (addr >= 0xFFFF800000000000ULL) {
            file = (file_t *)proc->handle_table[fd];
        }
    }

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
            if (total_written == 0) return -(s64)EFAULT;
            break;
        }
        s64 ret = (s64)vfs_write(file, kbuf, chunk);
        if (ret < 0) {
            if (ret == -(s64)EPIPE && proc) {
                sched_kill_process(proc->pid, 13 /* SIGPIPE */);
            }
            if (total_written == 0) return ret;
            break;
        }
        if (ret == 0) break;
        total_written += ret;
        count -= ret;
        if (ret < (s64)chunk) break;
    }
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
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = file;
            proc->fd_flags[i] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            return i;
        }
    }
    vfs_close(file);
    return -(s64)EMFILE;
}

static s64 sys_close_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= 64 || !proc) return -(s64)EBADF;
    file_t *f = (file_t *)__atomic_exchange_n(&proc->handle_table[fd], NULL, __ATOMIC_SEQ_CST);
    if (!f) return -(s64)EBADF;
    proc->fd_flags[fd] = 0;
    vfs_close(f);
    return 0;
}

static s64 sys_close_range_impl(pt_regs_t *r)
{
    unsigned int first = (unsigned int)r->rdi;
    unsigned int last  = (unsigned int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (last >= 64) last = 63;
    for (unsigned int i = first; i <= last && i < 64; i++) {
        file_t *f = (file_t *)__atomic_exchange_n(&proc->handle_table[i], NULL, __ATOMIC_SEQ_CST);
        if (f) {
            proc->fd_flags[i] = 0;
            vfs_close(f);
        }
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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
                __builtin_memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
                vmm_map(proc->pml4_phys, va, phys, VMM_USER_RW);
            }
        }
    } else if (target_page < cur_page) {
        for (virt_addr_t va = target_page; va < cur_page; va += PAGE_SIZE) {
            phys_addr_t phys = vmm_translate(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap(proc->pml4_phys, va);
                pmm_free_page(phys & VMM_PHYS_MASK);
            }
        }
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

    if (length == 0) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
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
            if (!proc->mmap_current || proc->mmap_current < 0x0000600000000000ULL || proc->mmap_current >= 0x00007ffff0000000ULL) {
                proc->mmap_current = 0x0000700000000000ULL;
            }
            target_addr = proc->mmap_current;
            proc->mmap_current += aligned_len;
        }
    } else {
        if (!target_addr || target_addr < 0x1000 || target_addr + aligned_len >= 0x0000800000000000ULL) {
            return -(s64)EINVAL;
        }
        for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
            virt_addr_t va = target_addr + offset;
            phys_addr_t phys = vmm_translate(proc->pml4_phys, va);
            if (phys) {
                vmm_unmap(proc->pml4_phys, va);
                pmm_free_page(phys & VMM_PHYS_MASK);
            }
        }
    }

    file_t *file = NULL;
    if (!(flags & 0x20) && fd >= 0 && fd < 64 && proc->handle_table[fd]) {
        file = (file_t *)proc->handle_table[fd];
        if (file && file->f_op && file->f_op->mmap) {
            s64 ret = file->f_op->mmap(file, target_addr, aligned_len, (u32)prot, (u32)flags, file_offset);
            if (ret == 0) {
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
        if (!phys) return -(s64)ENOMEM;
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
    for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
        virt_addr_t va = addr + offset;
        phys_addr_t phys = vmm_translate(proc->pml4_phys, va);
        if (phys) {
            vmm_unmap(proc->pml4_phys, va);
            pmm_free_page(phys & VMM_PHYS_MASK);
        }
    }
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
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];

    /* TTY ioctl commands: only valid for character devices / TTYs */
    if (cmd == 0x5401 /* TCGETS */ || cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ ||
        cmd == 0x5404 /* TCSETSF */ || cmd == 0x5413 /* TIOCGWINSZ */ || cmd == 0x5414 /* TIOCSWINSZ */ ||
        cmd == 0x540F /* TIOCGPGRP */ || cmd == 0x5410 /* TIOCSPGRP */ || cmd == 0x540B /* TIOCSCTTY */) {
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
                if (copy_to_user((void *)arg, termios_buf, 44) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ || cmd == 0x5404 /* TCSETSF */) return 0;
        if (cmd == 0x540F /* TIOCGPGRP */) {
            if (arg && (uintptr_t)arg < 0x8000000000000000ULL) {
                int pgid = (int)proc->pid;
                if (copy_to_user((void *)arg, &pgid, sizeof(int)) == 0) return 0;
                return -(s64)EFAULT;
            }
            return -(s64)EINVAL;
        }
        if (cmd == 0x5410 /* TIOCSPGRP */ || cmd == 0x540B /* TIOCSCTTY */) return 0;
    }

    /* Network configuration ioctl privilege checks */
    if (cmd == 0x8916 /* SIOCSIFADDR */ || cmd == 0x891C /* SIOCSIFNETMASK */ ||
        cmd == 0x892A /* SIOCSIFGW */   || cmd == 0x892B /* SIOCSIFDNS */ ||
        cmd == 0x8914 /* SIOCSIFFLAGS */ || cmd == 0x8990 /* SIOCSIFDHCP */) {
        if (!security_check_permission(proc, CAP_NET_ADMIN)) {
            return -(s64)EPERM;
        }
    }

    return vfs_ioctl(file, cmd, arg);

}

static s64 sys_lseek_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    s64 offset = (s64)r->rsi;
    int whence = (int)r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
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
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
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
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
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
            if (fd >= 64 || !proc->handle_table[fd]) {
                kfds[i].revents = POLLNVAL;
                ready_count++;
                continue;
            }

            short req = kfds[i].events;
            short rev = check_file_readiness((file_t *)proc->handle_table[fd], req);

            if (rev & (req | POLLHUP | POLLERR | POLLNVAL)) {
                kfds[i].revents = rev;
                ready_count++;
            }
        }

        if (ready_count > 0 || timeout_ms == 0) break;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) break;

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
        timeout_ms = (int)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    }

    u64 end_ticks = (timeout_ms > 0) ? (sched_get_ticks() + (timeout_ms + 9) / 10) : 0;
    int check_nfds = nfds > 64 ? 64 : nfds;
    kernel_fd_set_t out_rfds, out_wfds, out_efds;
    int ready_count = 0;

    for (;;) {
        ready_count = 0;
        __builtin_memset(&out_rfds, 0, sizeof(out_rfds));
        __builtin_memset(&out_wfds, 0, sizeof(out_wfds));
        __builtin_memset(&out_efds, 0, sizeof(out_efds));

        for (int fd = 0; fd < check_nfds; fd++) {
            file_t *f = (file_t *)proc->handle_table[fd];
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
        }

        if (ready_count > 0 || timeout_ms == 0) break;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) break;

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
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);

    /* A-02: inherit umask and user heap boundaries */
    child->umask        = parent->umask;
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->euid         = parent->euid;
    child->egid         = parent->egid;
    child->heap_start   = parent->heap_start;
    child->heap_end     = parent->heap_end;
    child->mmap_current = parent->mmap_current;
    child->fs_base      = parent->fs_base;
    child->gs_base      = parent->gs_base;

    for (int i = 0; i < 64; i++) {
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

    thread_t *t = thread_create_ex(child, r->rip, r->rsp, false, false);
    if (!t) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    if (t->user_regs) {
        *t->user_regs = *r;
        t->user_regs->rax = 0;      /* Child returns 0 */
        t->user_regs->rflags = 0x202; /* IF=1 */
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

    process_t *parent = sched_current_process();
    if (!parent) return -(s64)EPERM;

    vmm_space_t child_space = vmm_clone_space(parent->pml4_phys);
    if (!child_space) return -(s64)ENOMEM;

    process_t *child = proc_create(parent->name, child_space);
    if (!child) {
        vmm_destroy_space(child_space);
        return -(s64)ENOMEM;
    }

    child->parent       = parent;
    strncpy(child->cwd, parent->cwd, sizeof(child->cwd) - 1);
    child->umask        = parent->umask;
    child->uid          = parent->uid;
    child->gid          = parent->gid;
    child->euid         = parent->euid;
    child->egid         = parent->egid;
    child->heap_start   = parent->heap_start;
    child->heap_end     = parent->heap_end;
    child->mmap_current = parent->mmap_current;
    child->fs_base      = (flags & 0x00080000ULL /* CLONE_SETTLS */) ? newtls : parent->fs_base;
    child->gs_base      = parent->gs_base;

    for (int i = 0; i < 64; i++) {
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

    virt_addr_t user_rsp = child_stack ? child_stack : r->rsp;
    thread_t *t = thread_create_ex(child, r->rip, user_rsp, false, false);
    if (!t) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    if (t->user_regs) {
        *t->user_regs = *r;
        t->user_regs->rax = 0;        /* Child returns 0 */
        t->user_regs->rsp = user_rsp;
        t->user_regs->rflags = 0x202; /* IF=1 */
    }

    if ((flags & 0x00000100ULL /* CLONE_PARENT_SETTID */) && parent_tidptr && (uintptr_t)parent_tidptr < 0x8000000000000000ULL) {
        int tid = (int)child->pid;
        copy_to_user(parent_tidptr, &tid, sizeof(int));
    }

    if ((flags & 0x01000000ULL /* CLONE_CHILD_SETTID */) && child_tidptr && (uintptr_t)child_tidptr < 0x8000000000000000ULL) {
        int tid = (int)child->pid;
        copy_to_user(child_tidptr, &tid, sizeof(int));
    }

    sched_enqueue_thread(t);
    return (s64)child->pid;
}

static s64 sys_vfork_impl(pt_regs_t *r)
{
    return sys_fork_impl(r);
}

static s64 sys_execve_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    const char *const *user_argv = (const char *const *)r->rsi;
    const char *const *user_envp = (const char *const *)r->rdx;

    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

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

    /* A-01: Close file descriptors marked with FD_CLOEXEC */
    for (int i = 0; i < 64; i++) {
        if (proc->handle_table[i] && (proc->fd_flags[i] & FD_CLOEXEC)) {
            vfs_close((file_t *)proc->handle_table[i]);
            proc->handle_table[i] = NULL;
            proc->fd_flags[i] = 0;
        }
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

    ipc_shmem_unmap_all(proc);

    proc->pml4_phys = new_space;
    vmm_switch(new_space);

    if (old_space && old_space != vmm_kernel_space()) {
        vmm_destroy_space(old_space);
    }

    proc->fs_base = 0;
    wrmsr(MSR_FS_BASE, 0);
    proc->gs_base = 0;
    wrmsr(MSR_KERNEL_GS_BASE, 0);

    /* Reset custom signal handlers to default (SIG_DFL) per POSIX execve spec */
    for (int i = 0; i < _NSIG; i++) {
        if (proc->sigactions[i].sa_handler != SIG_IGN) {
            proc->sigactions[i].sa_handler = SIG_DFL;
            proc->sigactions[i].sa_flags = 0;
            proc->sigactions[i].sa_mask = 0;
        }
    }
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
    return 0;
}


s64 sys_exit_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (proc) {
        if (r) proc->exit_code = (int)r->rdi;
        pr_debug("[SYSCALL] Process exiting (PID %u, code %d)\n", proc->pid, proc->exit_code);

        /* Close all open file descriptors for this process so pipes, sockets, and files release immediately */
        for (int i = 0; i < 64; i++) {
            if (proc->handle_table[i]) {
                vfs_close((file_t *)proc->handle_table[i]);
                proc->handle_table[i] = NULL;
            }
            if (proc->obj_handle_table[i]) {
                az_object_t *obj = proc->obj_handle_table[i];
                proc->obj_handle_table[i] = NULL;
                az_object_dereference(obj);
            }
        }
    }
    sched_exit_thread();
    __builtin_unreachable();
}

static s64 sys_exit_group_impl(pt_regs_t *r)
{
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

    /* pid == 0: signal the current process group */
    if (pid == 0) pid = (s32)curr->pid;

    if (pid > 0) {
        /* Positive pid: signal that specific process */
        if (pid == (s32)curr->pid) {
            if (sig != 0) {
                sys_exit_impl(r);
            }
            return 0;
        }
        return sched_kill_process((u32)pid, sig);
    }

    /* POSIX-03: negative pid means signal process group |pid|.
     * Iterate all processes with matching pgid (approximated as pid == |target|).
     * For now we treat pid == -1 as "all processes" and negative pid as
     * "all processes whose pid matches |pid|" (single-process groups). */
    s32 target_pgid = -pid;
    irqflags_t irqf = 0; (void)irqf;
    s64 ret = -(s64)ESRCH;
    process_t *p = sched_get_process_list();
    while (p) {
        process_t *next = p->next;
        if (pid == -1 || (s32)p->pid == target_pgid) {
            if (p != curr) {
                s64 r2 = sched_kill_process(p->pid, sig);
                if (r2 == 0) ret = 0;
            }
        }
        p = next;
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

static s64 sys_rt_sigreturn_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_pause_impl(pt_regs_t *r)
{
    (void)r;
    /* B-14: block until awakened by signal */
    sched_block(THREAD_BLOCKED);
    return -(s64)EINTR;
}

static s64 sys_alarm_impl(pt_regs_t *r)
{
    u32 sec = (u32)r->rdi;
    (void)sec;
    return 0;
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

    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            if (type & 02000000) { /* SOCK_CLOEXEC */
                proc->fd_flags[i] = FD_CLOEXEC;
            }
            return i;
        }
    }

    sock_free(sock);
    kfree(f);
    return -(s64)EMFILE;
}

static s64 sys_bind_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->rsi;
    socklen_t addrlen = (socklen_t)r->rdx;

    if (!uaddr || addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

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

    if (!uaddr || addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    struct sockaddr_in sin;
    if (copy_from_user(&sin, uaddr, sizeof(struct sockaddr_in)) != 0) {
        return -(s64)EFAULT;
    }

    u16 port = ntohs(sin.sin_port);
    const u8 *ip = (const u8 *)&sin.sin_addr.s_addr;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

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

    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_listen(sock->tcp, backlog);
    }

    return -(s64)EOPNOTSUPP;
}

static s64 sys_accept_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    socket_t *listener = NULL;
    int ret = sock_get_from_fd(fd, &listener);
    if (ret < 0) return -(s64)ret;

    if (listener->type != SOCK_STREAM || !listener->tcp) {
        return -(s64)EOPNOTSUPP;
    }

    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

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

    int new_fd = -1;
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = child_file;
            new_fd = i;
            break;
        }
    }

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

static s64 sys_shutdown_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int how = (int)r->rsi;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

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

static s64 sys_sendto_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const void *ubuf = (const void *)r->rsi;
    size_t len = (size_t)r->rdx;
    int flags = (int)r->r10;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->r8;
    socklen_t uaddrlen = (socklen_t)r->r9;
    (void)uaddrlen;

    if (!ubuf || len == 0) return 0;
    if ((uintptr_t)ubuf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    void *kbuf = kmalloc(len);
    if (!kbuf) return -(s64)ENOMEM;
    if (copy_from_user(kbuf, ubuf, len) != 0) {
        kfree(kbuf);
        return -(s64)EFAULT;
    }

    s64 res = -(s64)EOPNOTSUPP;

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
                sched_block(THREAD_BLOCKED);
            }
            spinlock_unlock(&sock->raw->lock);
            sched_yield();
        }
    }

    kfree(kbuf);
    return -(s64)EOPNOTSUPP;
}


/* ── Pipes & File Descriptors ────────────────────────────────────────────── */

static s64 sys_pipe_impl(pt_regs_t *r)
{
    int *user_fds = (int *)r->rdi;
    if (!user_fds || (uintptr_t)user_fds >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    int fd0 = -1, fd1 = -1;
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            if (fd0 == -1) fd0 = i;
            else if (fd1 == -1) { fd1 = i; break; }
        }
    }
    if (fd0 == -1 || fd1 == -1) return -(s64)EMFILE;

    file_t *rf = NULL, *wf = NULL;
    int err = pipe_create(&rf, &wf);
    if (err < 0) return (s64)err;

    proc->handle_table[fd0] = rf;
    proc->handle_table[fd1] = wf;
    proc->fd_flags[fd0] = 0;
    proc->fd_flags[fd1] = 0;

    int fds[2] = { fd0, fd1 };
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        proc->handle_table[fd0] = NULL;
        proc->handle_table[fd1] = NULL;
        proc->fd_flags[fd0] = 0;
        proc->fd_flags[fd1] = 0;
        vfs_close(rf);
        vfs_close(wf);
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

    int fd0 = -1, fd1 = -1;
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            if (fd0 == -1) fd0 = i;
            else if (fd1 == -1) { fd1 = i; break; }
        }
    }
    if (fd0 == -1 || fd1 == -1) return -(s64)EMFILE;

    file_t *rf = NULL, *wf = NULL;
    int err = pipe_create(&rf, &wf);
    if (err < 0) return (s64)err;

    if (flags & O_NONBLOCK) {
        rf->f_flags |= O_NONBLOCK;
        wf->f_flags |= O_NONBLOCK;
    }

    proc->handle_table[fd0] = rf;
    proc->handle_table[fd1] = wf;
    proc->fd_flags[fd0] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
    proc->fd_flags[fd1] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;

    int fds[2] = { fd0, fd1 };
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        proc->handle_table[fd0] = NULL;
        proc->handle_table[fd1] = NULL;
        proc->fd_flags[fd0] = 0;
        proc->fd_flags[fd1] = 0;
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_dup_impl(pt_regs_t *r)
{
    int oldfd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc || oldfd < 0 || oldfd >= 64 || !proc->handle_table[oldfd])
        return -(s64)EBADF;

    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            file_t *f = (file_t *)proc->handle_table[oldfd];
            __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
            proc->handle_table[i] = f;
            proc->fd_flags[i] = 0; /* dup clears FD_CLOEXEC */
            return i;
        }
    }
    return -(s64)EMFILE;
}

static s64 sys_dup2_impl(pt_regs_t *r)
{
    int oldfd = (int)(s32)r->rdi;
    int newfd = (int)(s32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (oldfd < 0 || oldfd >= 64 || !proc->handle_table[oldfd]) return -(s64)EBADF;
    if (newfd < 0 || newfd >= 64) return -(s64)EBADF;
    if (oldfd == newfd) return newfd;

    if (proc->handle_table[newfd])
        vfs_close((file_t *)proc->handle_table[newfd]);

    file_t *f = (file_t *)proc->handle_table[oldfd];
    __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
    proc->handle_table[newfd] = f;
    proc->fd_flags[newfd] = 0; /* dup2 clears FD_CLOEXEC */
    return newfd;
}

static s64 sys_dup3_impl(pt_regs_t *r)
{
    /* BUG-04: POSIX requires dup3(old, new, flags) to return EINVAL when oldfd == newfd */
    int oldfd = (int)(s32)r->rdi;
    int newfd = (int)(s32)r->rsi;
    int flags = (int)r->rdx;
    if (oldfd == newfd) return -(s64)EINVAL;
    if (flags & ~O_CLOEXEC) return -(s64)EINVAL;

    s64 ret = sys_dup2_impl(r);
    if (ret >= 0 && (flags & O_CLOEXEC)) {
        process_t *proc = sched_current_process();
        if (proc && newfd >= 0 && newfd < 64) {
            proc->fd_flags[newfd] = FD_CLOEXEC;
        }
    }
    return ret;
}

static s64 sys_fcntl_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int cmd = (int)r->rsi;
    u64 arg = r->rdx;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];

    switch (cmd) {
    case 0:      /* F_DUPFD */
    case 1030: { /* F_DUPFD_CLOEXEC */
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= 64) return -(s64)EINVAL;
        for (int i = minfd; i < 64; i++) {
            if (!proc->handle_table[i]) {
                __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
                proc->handle_table[i] = f;
                proc->fd_flags[i] = (cmd == 1030) ? FD_CLOEXEC : 0;
                return i;
            }
        }
        return -(s64)EMFILE;
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
        if (arg && arg < 0x8000000000000000ULL) {
            struct {
                short l_type;
                short l_whence;
                s64   l_start;
                s64   l_len;
                s32   l_pid;
            } fl;
            if (copy_from_user(&fl, (const void *)arg, sizeof(fl)) == 0) {
                fl.l_type = 2; /* F_UNLCK */
                copy_to_user((void *)arg, &fl, sizeof(fl));
            }
        }
        return 0;
    }
    case 6:   /* F_SETLK */
    case 7:   /* F_SETLKW */
        return 0;
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

    process_t *proc = sched_current_process();
    if (proc) {
        strncpy(proc->cwd, kpath, sizeof(proc->cwd) - 1);
    }
    return 0;
}

static s64 sys_fchdir_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *f = (file_t *)proc->handle_table[fd];
    if (!f || !f->f_dentry || !f->f_dentry->d_inode) return -(s64)EBADF;
    if (!S_ISDIR(f->f_dentry->d_inode->i_mode)) return -(s64)ENOTDIR;

    if (f->f_dentry->d_name[0] == '/') {
        strncpy(proc->cwd, f->f_dentry->d_name, sizeof(proc->cwd) - 1);
    }
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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
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
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
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

static u64 get_cached_unix_time(void)
{
    u64 ticks = sched_get_ticks();
    if (s_rtc_unix_sec == 0 || (ticks - s_rtc_base_ticks) >= 100) {
        rtc_time_t t;
        rtc_read_time(&t);
        s_rtc_unix_sec   = rtc_to_unix_time(&t);
        s_rtc_base_ticks = ticks;
    }
    return s_rtc_unix_sec + (ticks - s_rtc_base_ticks) / 100;
}

static s64 sys_clock_gettime_impl(pt_regs_t *r)
{
    u32 clk_id = (u32)r->rdi;
    void *tp   = (void *)r->rsi;
    if (!tp) return -(s64)EINVAL;
    if ((uintptr_t)tp >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u64 ticks = sched_get_ticks();
    u64 unix_sec;
    if (clk_id == 1 /* CLOCK_MONOTONIC */) {
        unix_sec = ticks / 100;
    } else {
        unix_sec = get_cached_unix_time();
    }
    u64 sub_sec_ns = (ticks % 100) * 10000000ULL;

    u64 ts[2] = { unix_sec, sub_sec_ns };
    if (copy_to_user(tp, ts, sizeof(ts)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_gettimeofday_impl(pt_regs_t *r)
{
    struct linux_timeval *user_tv = (struct linux_timeval *)r->rdi;
    if (!user_tv) return 0;
    if ((uintptr_t)user_tv >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u64 ticks = sched_get_ticks();
    u64 unix_sec = get_cached_unix_time();
    u64 sub_sec_us = (ticks % 100) * 10000ULL;

    struct linux_timeval tv = { (long)unix_sec, (long)sub_sec_us };
    if (copy_to_user(user_tv, &tv, sizeof(tv)) != 0) return -(s64)EFAULT;
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

static s64 sys_utime_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_utimes_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_utimensat_impl(pt_regs_t *r) { (void)r; return 0; }

struct tms {
    u64 tms_utime;
    u64 tms_stime;
    u64 tms_cutime;
    u64 tms_cstime;
};

static s64 sys_times_impl(pt_regs_t *r)
{
    struct tms *buf = (struct tms *)r->rdi;
    if (buf && (uintptr_t)buf < 0x8000000000000000ULL) {
        struct tms ktms = {
            .tms_utime = sched_get_ticks() / 2,
            .tms_stime = sched_get_ticks() / 2,
            .tms_cutime = 0,
            .tms_cstime = 0,
        };
        copy_to_user(buf, &ktms, sizeof(struct tms));
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
static s64 sys_getpgrp_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 0;
}
static s64 sys_setpgid_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_setsid_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 0;
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
#define RLIM_INFINITY     (~0ULL)

static s64 sys_getrlimit_impl(pt_regs_t *r)
{
    int resource = (int)r->rdi;
    struct rlimit *rlim = (struct rlimit *)r->rsi;
    if (!rlim || (uintptr_t)rlim >= 0x8000000000000000ULL) return -(s64)EFAULT;

    struct rlimit krlim;
    switch (resource) {
    case RLIMIT_NOFILE:
        krlim.rlim_cur = 64;
        krlim.rlim_max = 64;
        break;
    case RLIMIT_STACK:
        krlim.rlim_cur = 8 * 1024 * 1024; /* 8 MB stack */
        krlim.rlim_max = 8 * 1024 * 1024;
        break;
    default:
        krlim.rlim_cur = RLIM_INFINITY;
        krlim.rlim_max = RLIM_INFINITY;
        break;
    }

    if (copy_to_user(rlim, &krlim, sizeof(struct rlimit)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_setrlimit_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
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
    u64 ticks = sched_get_ticks();
    struct rusage ru;
    __builtin_memset(&ru, 0, sizeof(ru));
    ru.ru_utime.tv_sec = (long)(ticks / 100);
    ru.ru_utime.tv_usec = (long)((ticks % 100) * 10000L);
    ru.ru_stime.tv_sec = (long)(ticks / 200);
    ru.ru_stime.tv_usec = (long)((ticks % 200) * 5000L);
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
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = file;
            proc->fd_flags[i] = (flags & O_CLOEXEC) ? FD_CLOEXEC : 0;
            return i;
        }
    }
    vfs_close(file);
    return -(s64)EMFILE;
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
        if (dirfd >= 0 && dirfd < 64) {
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
        if (dirfd >= 0 && dirfd < 64) {
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
        if (dirfd >= 0 && dirfd < 64) {
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
        proc->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    } else if (code == ARCH_GET_FS) {
        if (!addr || addr >= 0x8000000000000000ULL) return -(s64)EFAULT;
        return copy_to_user((void *)addr, &proc->fs_base, sizeof(u64)) == 0 ? 0 : -(s64)EFAULT;
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
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 1;
}

static s64 sys_set_robust_list_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
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

    (void)pid;
    (void)new_rlim;

    if (resource < 0 || resource > 15) return -(s64)EINVAL;

    if (old_rlim && (uintptr_t)old_rlim < 0x8000000000000000ULL) {
        struct kernel_rlimit64 cur;
        cur.rlim_cur = RLIM64_INFINITY;
        cur.rlim_max = RLIM64_INFINITY;

        switch (resource) {
        case RLIMIT_STACK:
            cur.rlim_cur = 8 * 1024 * 1024ULL;  /* 8 MB */
            cur.rlim_max = 64 * 1024 * 1024ULL; /* 64 MB */
            break;
        case RLIMIT_NOFILE:
            cur.rlim_cur = 1024;
            cur.rlim_max = 65536;
            break;
        case RLIMIT_NPROC:
            cur.rlim_cur = 1024;
            cur.rlim_max = 4096;
            break;
        case RLIMIT_CORE:
            cur.rlim_cur = 0;
            cur.rlim_max = RLIM64_INFINITY;
            break;
        case RLIMIT_MEMLOCK:
            cur.rlim_cur = 64 * 1024ULL;
            cur.rlim_max = 64 * 1024ULL;
            break;
        default:
            cur.rlim_cur = RLIM64_INFINITY;
            cur.rlim_max = RLIM64_INFINITY;
            break;
        }

        if (copy_to_user(old_rlim, &cur, sizeof(struct kernel_rlimit64)) != 0) {
            return -(s64)EFAULT;
        }
    }
    return 0;
}

static s64 sys_clone3_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
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
    (void)r;
    return -(s64)ENOSYS;
}

static s64 sys_sched_setattr_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_sched_getattr_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_pidfd_open_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}

static s64 sys_pidfd_send_signal_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}

static s64 sys_pidfd_getfd_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}

static s64 sys_memfd_create_impl(pt_regs_t *r)
{
    const char *uname = (const char *)r->rdi;
    unsigned int flags = (unsigned int)r->rsi;
    (void)uname;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *rf = NULL;
    file_t *wf = NULL;
    if (pipe_create(&rf, &wf) < 0) return -(s64)ENOMEM;

    int fd = -1;
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EMFILE;
    }
    proc->handle_table[fd] = wf;
    proc->fd_flags[fd] = (flags & 0x0001 /* MFD_CLOEXEC */) ? 1 : 0;
    vfs_close(rf);
    return (s64)fd;

}

static s64 sys_rseq_impl(pt_regs_t *r)
{
    (void)r;
    return -(s64)ENOSYS;
}



static u64 get_entropy64(void)
{
    static u64 s_rand_state = 0x853c49e6748fea9bULL;
    u32 lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    u64 tsc = ((u64)hi << 32) | lo;
    extern u64 g_system_ticks;
    s_rand_state ^= tsc ^ (g_system_ticks << 17) ^ ((u64)(uintptr_t)sched_current_thread() << 3);
    s_rand_state ^= s_rand_state >> 12;
    s_rand_state ^= s_rand_state << 25;
    s_rand_state ^= s_rand_state >> 27;
    return s_rand_state * 0x2545F4914F6CDD1DULL;
}

static s64 sys_getrandom_impl(pt_regs_t *r)
{
    void *buf = (void *)r->rdi;
    size_t buflen = (size_t)r->rsi;
    if (!buf || buflen == 0) return 0;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    u8 kbuf[256];
    size_t written = 0;
    while (written < buflen) {
        size_t chunk = buflen - written;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        for (size_t i = 0; i < chunk; i += 8) {
            u64 val = get_entropy64();
            size_t copy_sub = (chunk - i > 8) ? 8 : (chunk - i);
            __builtin_memcpy(&kbuf[i], &val, copy_sub);
        }
        if (copy_to_user((char *)buf + written, kbuf, chunk) != 0) return -(s64)EFAULT;
        written += chunk;
    }
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
    if (out_fd < 0 || out_fd >= 64 || in_fd < 0 || in_fd >= 64) return -(s64)EBADF;

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
    if (fd < 0 || fd >= 64) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = (file_t *)proc->handle_table[fd];
    if (!file || !file->f_inode) return -(s64)EBADF;

    s64 req_size = offset + len;
    if (!(mode & FALLOC_FL_KEEP_SIZE)) {
        if ((s64)file->f_inode->i_size < req_size) {
            file->f_inode->i_size = (u64)req_size;
        }
    }
    return 0;
}

static s64 sys_sync_file_range_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    if (fd < 0 || fd >= 64) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;
    return 0;
}

static s64 sys_readahead_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    if (fd < 0 || fd >= 64) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;
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

    if (fd < 0 || fd >= 64) return -(s64)EBADF;
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
        if (dirfd < 0 || dirfd >= 64) return -(s64)EBADF;
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
    (void)r;
    return 0;
}

static s64 sys_madvise_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_fadvise64_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
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

    int fd0 = -1, fd1 = -1;
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            if (fd0 == -1) fd0 = i;
            else if (fd1 == -1) { fd1 = i; break; }
        }
    }
    if (fd0 == -1 || fd1 == -1) return -(s64)EMFILE;

    file_t *rf = NULL, *wf = NULL;
    int err = sockpair_create(&rf, &wf);
    if (err < 0) return (s64)err;

    proc->handle_table[fd0] = rf;
    proc->handle_table[fd1] = wf;
    proc->fd_flags[fd0] = (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0;
    proc->fd_flags[fd1] = (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0;

    int sv[2] = { fd0, fd1 };
    if (copy_to_user(user_sv, sv, sizeof(sv)) != 0) {
        proc->handle_table[fd0] = NULL;
        proc->handle_table[fd1] = NULL;
        vfs_close(rf);
        vfs_close(wf);
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
    if (option == 38 /* PR_SET_NO_NEW_PRIVS */) return 0;
    if (option == 39 /* PR_GET_NO_NEW_PRIVS */) return 0;
    return 0;
}

static s64 sys_sched_getaffinity_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    size_t cpusetsize = (size_t)r->rsi;
    void *mask = (void *)r->rdx;
    (void)pid;
    if (!mask || (uintptr_t)mask >= 0x8000000000000000ULL) return -(s64)EFAULT;
    if (cpusetsize < sizeof(u64)) return -(s64)EINVAL;
    u64 affinity = 0x0F; /* 4 CPUs */
    if (copy_to_user(mask, &affinity, sizeof(u64)) != 0) return -(s64)EFAULT;
    return (s64)sizeof(u64);
}

static s64 sys_sched_setaffinity_impl(pt_regs_t *r)
{
    (void)r;
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
    (void)tgid;
    if (tid <= 0) return -(s64)EINVAL;
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
    return vfs_symlink(kold, knew);
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
    return vfs_symlink(kold, knew);
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

    char kpath[512];
    s64 err = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (err < 0) return err;
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
    (void)r;
    return 0;
}

static s64 sys_fsync_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
    return 0;
}

static s64 sys_fdatasync_impl(pt_regs_t *r)
{
    return sys_fsync_impl(r);
}

static s64 sys_sync_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_syncfs_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_getpgid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->pid;
    return (s64)pid;
}

static s64 sys_getsid_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (pid == 0) return (s64)proc->pid;
    return (s64)pid;
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
    struct linux_timespec *res = (struct linux_timespec *)r->rsi;
    if (res && (uintptr_t)res < 0x8000000000000000ULL) {
        struct linux_timespec ts = { .tv_sec = 0, .tv_nsec = 10000000L /* 10ms (100 Hz timer) */ };
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
    (void)r;
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
            memset((void *)(HHDM_BASE + p), 0, 4096);
            vmm_map(proc->pml4_phys, old_addr + i * 4096, p, VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_NX);
        }
        return (s64)old_addr;
    } else {
        for (size_t i = new_pages; i < old_pages; i++) {
            vmm_unmap(proc->pml4_phys, old_addr + i * 4096);
        }
        return (s64)old_addr;
    }
}

static s64 sys_mincore_impl(pt_regs_t *r)
{
    uintptr_t start = (uintptr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    unsigned char *vec = (unsigned char *)r->rdx;

    if (start >= 0x0000800000000000ULL || (start & 0xFFF) != 0) return -(s64)EINVAL;
    if (!vec || (uintptr_t)vec >= 0x0000800000000000ULL) return -(s64)EFAULT;

    size_t pages = (length + 4095) / 4096;
    for (size_t i = 0; i < pages; i++) {
        unsigned char status = 1;
        if (copy_to_user(&vec[i], &status, 1) != 0) return -(s64)EFAULT;
    }
    return 0;
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

        if (timeout_ticks > 0) {
            sched_sleep(timeout_ticks);
        } else {
            sched_block(THREAD_BLOCKED);
        }

        spinlock_lock(&g_futex_lock);
        futex_q_t **curr_q = &g_futex_table[b];
        while (*curr_q) {
            if (*curr_q == &q) {
                *curr_q = q.next;
                break;
            }
            curr_q = &(*curr_q)->next;
        }
        spinlock_unlock(&g_futex_lock);

        return 0;
    }
    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        u32 bitset = (cmd == FUTEX_WAKE_BITSET) ? val3 : FUTEX_BITSET_MATCH_ANY;
        if (bitset == 0) return -(s64)EINVAL;

        u32 b = futex_hash(uaddr);
        int woken = 0;

        spinlock_lock(&g_futex_lock);
        futex_q_t *curr_q = g_futex_table[b];
        while (curr_q && (u32)woken < val) {
            if (curr_q->proc == proc && curr_q->uaddr == uaddr && (curr_q->bitset & bitset)) {
                sched_unblock(curr_q->thread);
                woken++;
            }
            curr_q = curr_q->next;
        }
        spinlock_unlock(&g_futex_lock);

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
        int woken = 0;
        int requeued = 0;
        u32 val2_max = timeout ? (u32)(uintptr_t)timeout : 0;

        spinlock_lock(&g_futex_lock);
        futex_q_t *curr_q = g_futex_table[b1];
        while (curr_q) {
            if (curr_q->proc == proc && curr_q->uaddr == uaddr) {
                if ((u32)woken < val) {
                    sched_unblock(curr_q->thread);
                    woken++;
                } else if ((u32)requeued < val2_max) {
                    curr_q->uaddr = uaddr2;
                    requeued++;
                }
            }
            curr_q = curr_q->next;
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

    for (int i = 3; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            proc->fd_flags[i] = f->f_fd_flags;
            return i;
        }
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

    for (int i = 3; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            proc->fd_flags[i] = f->f_fd_flags;
            return i;
        }
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
    if (!proc || epfd < 0 || epfd >= 64 || !proc->handle_table[epfd]) return -(s64)EBADF;
    if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
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
    if (!proc || epfd < 0 || epfd >= 64 || !proc->handle_table[epfd]) return -(s64)EBADF;

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
            if (tfd >= 0 && tfd < 64 && proc->handle_table[tfd]) {
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
    if (!buf || (uintptr_t)buf >= 0x0000800000000000ULL) return -(s64)EFAULT;
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
            if (copy_to_user(buf, &exp, sizeof(u64)) != 0) return -(s64)EFAULT;
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

    for (int i = 3; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            proc->fd_flags[i] = f->f_fd_flags;
            return i;
        }
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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
        if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;
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

    for (int i = 3; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            proc->fd_flags[i] = f->f_fd_flags;
            return i;
        }
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
static s64 sys_sched_setscheduler_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_sched_getscheduler_impl(pt_regs_t *r)
{
    (void)r;
    return 0; /* SCHED_OTHER */
}

static s64 sys_sched_setparam_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_sched_getparam_impl(pt_regs_t *r)
{
    int *param = (int *)r->rsi;
    if (param && (uintptr_t)param < 0x0000800000000000ULL) {
        int priority = 0;
        copy_to_user(param, &priority, sizeof(int));
    }
    return 0;
}

static s64 sys_sched_get_priority_max_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 || policy == 2) return 99;
    return 0;
}

static s64 sys_sched_get_priority_min_impl(pt_regs_t *r)
{
    int policy = (int)r->rdi;
    if (policy == 1 || policy == 2) return 1;
    return 0;
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
    (void)r;
    return 0; /* PER_LINUX */
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

static s64 sys_capget_impl(pt_regs_t *r)
{
    void *datap = (void *)r->rsi;
    process_t *proc = sched_current_process();
    if (datap && (uintptr_t)datap < 0x0000800000000000ULL) {
        u32 caps[6] = { 0 };
        if (proc && proc->euid == 0) {
            caps[0] = 0xFFFFFFFF;
            caps[1] = 0xFFFFFFFF;
            caps[2] = 0xFFFFFFFF;
            caps[3] = 0xFFFFFFFF;
            caps[4] = 0xFFFFFFFF;
            caps[5] = 0xFFFFFFFF;
        }
        copy_to_user(datap, caps, sizeof(caps));
    }
    return 0;
}

static s64 sys_capset_impl(pt_regs_t *r)
{
    (void)r;
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

static s64 sys_getpriority_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who = (int)r->rsi;
    (void)which; (void)who;
    return 0;
}

static s64 sys_setpriority_impl(pt_regs_t *r)
{
    int which = (int)r->rdi;
    int who = (int)r->rsi;
    int prio = (int)r->rdx;
    (void)which; (void)who; (void)prio;
    return 0;
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
        strncpy(proc->cwd, kpath, sizeof(proc->cwd) - 1);
        proc->cwd[sizeof(proc->cwd) - 1] = '\0';
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

    for (int i = 3; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = f;
            proc->fd_flags[i] = f->f_fd_flags;
            return i;
        }
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
    if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

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
    if (!proc || fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

    char kpath[32];
    snprintf(kpath, sizeof(kpath), "fd:%d", fd);
    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) return -(s64)EFAULT;
    kname[sizeof(kname) - 1] = '\0';

    return do_removexattr(kpath, kname);
}
