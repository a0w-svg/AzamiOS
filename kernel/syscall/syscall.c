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

typedef s64 (*syscall_fn_t)(pt_regs_t *r);

#define SYSCALL_TABLE_SIZE  530
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
static s64 sys_fork_impl(pt_regs_t *r);
static s64 sys_execve_impl(pt_regs_t *r);
s64 sys_exit_impl(pt_regs_t *r);
static s64 sys_wait4_impl(pt_regs_t *r);
static s64 sys_kill_impl(pt_regs_t *r);
static s64 sys_uname_impl(pt_regs_t *r);
static s64 sys_fcntl_impl(pt_regs_t *r);
static s64 sys_truncate_impl(pt_regs_t *r);
static s64 sys_ftruncate_impl(pt_regs_t *r);
static s64 sys_getdents_impl(pt_regs_t *r);
static s64 sys_getcwd_impl(pt_regs_t *r);
static s64 sys_chdir_impl(pt_regs_t *r);
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
static s64 sys_az_spawn(pt_regs_t *r);
static s64 sys_az_yield(pt_regs_t *r);
static s64 sys_az_thread_create_impl(pt_regs_t *r);
static s64 sys_az_sysstat_impl(pt_regs_t *r);
static s64 sys_az_set_timer_impl(pt_regs_t *r);

/* ── Main dispatcher ─────────────────────────────────────────────────────── */

void syscall_dispatch(pt_regs_t *regs)
{
    u64 nr = regs->rax;

    if (unlikely(nr >= SYSCALL_TABLE_SIZE) || !g_syscall_table[nr]) {
        pr_debug("[SYSCALL] Unknown syscall %llu from RIP=0x%016llx\n",
                (unsigned long long)nr,
                (unsigned long long)regs->rip);
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
    reg(SYS_munmap,        sys_munmap_impl);
    reg(SYS_brk,           sys_brk_impl);
    reg(SYS_rt_sigaction,  sys_rt_sigaction_impl);
    reg(SYS_rt_sigprocmask, sys_rt_sigprocmask_impl);
    reg(SYS_rt_sigreturn,  sys_rt_sigreturn_impl);
    reg(SYS_ioctl,         sys_ioctl_impl);
    reg(SYS_readv,         sys_readv_impl);
    reg(SYS_writev,        sys_writev_impl);
    reg(SYS_access,        sys_access_impl);
    reg(SYS_pipe,          sys_pipe_impl);
    reg(SYS_select,        sys_select_impl);
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
    reg(SYS_setsockopt,    sys_setsockopt_impl);
    reg(SYS_getsockopt,    sys_getsockopt_impl);
    reg(SYS_fork,          sys_fork_impl);
    reg(SYS_execve,        sys_execve_impl);
    reg(SYS_exit,          sys_exit_impl);
    reg(SYS_wait4,         sys_wait4_impl);
    reg(SYS_waitpid,       sys_wait4_impl);
    reg(SYS_kill,          sys_kill_impl);
    reg(SYS_uname,         sys_uname_impl);
    reg(SYS_fcntl,         sys_fcntl_impl);
    reg(SYS_truncate,      sys_truncate_impl);
    reg(SYS_ftruncate,     sys_ftruncate_impl);
    reg(SYS_getdents,      sys_getdents_impl);
    reg(SYS_getcwd,        sys_getcwd_impl);
    reg(SYS_chdir,         sys_chdir_impl);
    reg(SYS_rename,        sys_rename_impl);
    reg(SYS_mkdir,         sys_mkdir_impl);
    reg(SYS_rmdir,         sys_rmdir_impl);
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
    reg(SYS_utime,         sys_utime_impl);
    reg(SYS_statfs,        sys_statfs_impl);
    reg(SYS_fstatfs,       sys_fstatfs_impl);
    reg(SYS_reboot,        sys_reboot_impl);
    reg(SYS_time,          sys_time_impl);
    reg(SYS_getdents64,    sys_getdents64_impl);
    reg(SYS_clock_gettime, sys_clock_gettime_impl);
    reg(SYS_exit_group,    sys_exit_group_impl);
    reg(SYS_utimes,        sys_utimes_impl);
    reg(SYS_pselect6,      sys_pselect6_impl);
    reg(SYS_ppoll,         sys_ppoll_impl);
    reg(SYS_utimensat,     sys_utimensat_impl);
    reg(SYS_dup3,          sys_dup3_impl);
    reg(SYS_pipe2,         sys_pipe2_impl);

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
    reg(SYS_AZ_SPAWN,          sys_az_spawn);
    reg(SYS_AZ_YIELD,          sys_az_yield);
    reg(SYS_AZ_THREAD_CREATE,  sys_az_thread_create_impl);
    reg(SYS_AZ_SYSSTAT,        sys_az_sysstat_impl);
    reg(SYS_AZ_SET_TIMER,      sys_az_set_timer_impl);

    pr_debug("[SYSCALL] Dispatch table ready (%d entries)\n", SYSCALL_TABLE_SIZE);
}

/* ── Path Resolution Helper ──────────────────────────────────────────────── */

static s64 copy_user_path_resolve(char *kpath, size_t max_len, const char *user_path)
{
    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char raw[256];
    __builtin_memset(raw, 0, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw) - 1; i++) {
        if (copy_from_user(&raw[i], user_path + i, 1) != 0) return -(s64)EFAULT;
        if (raw[i] == '\0') break;
    }

    process_t *proc = sched_current_process();
    const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
    return vfs_resolve_path(cwd, raw, kpath, max_len);
}

/* ── Standard I/O Syscalls ───────────────────────────────────────────────── */

static s64 sys_read_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    char *buf = (char *)r->rsi;
    s64 count = (s64)r->rdx;
    if (count <= 0 || !buf) return 0;
    if ((uintptr_t)buf >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    file_t *file = NULL;
    if (proc && fd >= 0 && fd < 64 && proc->handle_table[fd]) {
        file = (file_t *)proc->handle_table[fd];
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
        if (ret <= 0) break;
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
        file = (file_t *)proc->handle_table[fd];
    }

    if ((fd == 1 || fd == 2) && !file) {
        /* If a process has no stdout/stderr redirected, do not write directly to console */
        return count;
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
        if (ret <= 0) break;
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

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    file_t *file = vfs_open(kpath, (u32)flags, mode);
    if (!file) return -(s64)ENOENT;
    process_t *proc = sched_current_process();
    if (!proc) { vfs_close(file); return -(s64)EPERM; }
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            proc->handle_table[i] = file;
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
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    vfs_close((file_t *)proc->handle_table[fd]);
    proc->handle_table[fd] = NULL;
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

    if (new_brk < proc->heap_start || new_brk > 0x0000700000000000ULL) {
        return (s64)proc->heap_end;
    }

    virt_addr_t cur_page = ALIGN_UP(proc->heap_end, PAGE_SIZE);
    virt_addr_t target_page = ALIGN_UP(new_brk, PAGE_SIZE);

    if (target_page > cur_page) {
        for (virt_addr_t va = cur_page; va < target_page; va += PAGE_SIZE) {
            phys_addr_t phys = pmm_alloc_page();
            if (!phys) return (s64)proc->heap_end;
            __builtin_memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
            vmm_map(proc->pml4_phys, va, phys, VMM_USER_RW);
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
    virt_addr_t addr = (virt_addr_t)r->rdi;
    size_t length = (size_t)r->rsi;
    (void)r->rdx; /* prot */
    (void)r->r10; /* flags */

    if (length == 0) return -(s64)EINVAL;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    size_t aligned_len = ALIGN_UP(length, PAGE_SIZE);
    virt_addr_t target_addr = addr;

    if (!target_addr || target_addr < 0x10000000 || target_addr >= 0x0000700000000000ULL) {
        if (!proc->mmap_current) proc->mmap_current = 0x0000600000000000ULL;
        target_addr = proc->mmap_current;
        proc->mmap_current += aligned_len;
    }

    u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE;
    for (size_t offset = 0; offset < aligned_len; offset += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) return -(s64)ENOMEM;
        __builtin_memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
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

/* ── File Operations & Metadata ─────────────────────────────────────────── */

static s64 sys_ioctl_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    u32 cmd = (u32)r->rsi;
    u64 arg = r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= 64 || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = (file_t *)proc->handle_table[fd];
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

    char kpath[256];
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

    char kpath[256];
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

    char kpath[256];
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
    char kpath[256];
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
    char kpath[256];
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
    u32 mask = (u32)r->rdi;
    (void)mask;
    return 022; /* Default umask */
}

static s64 sys_symlink_impl(pt_regs_t *r)
{
    const char *user_target = (const char *)r->rdi;
    const char *user_link = (const char *)r->rsi;
    if (!user_target || !user_link) return -(s64)EINVAL;

    char ktarget[256], klink[256];
    if (copy_from_user(ktarget, user_target, sizeof(ktarget) - 1) != 0) return -(s64)EFAULT;
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

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

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
            short rev = 0;

            /* Check standard streams or files */
            if (req & POLLIN)  rev |= POLLIN;
            if (req & POLLOUT) rev |= POLLOUT;

            if (rev & req) {
                kfds[i].revents = rev & req;
                ready_count++;
            }
        }

        if (ready_count > 0 || timeout_ms == 0) break;
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) break;

        sched_yield();
    }

    copy_to_user(user_fds, kfds, sizeof(struct pollfd) * nfds);
    kfree(kfds);
    return ready_count;
}

static s64 sys_ppoll_impl(pt_regs_t *r)
{
    return sys_poll_impl(r);
}

static s64 sys_select_impl(pt_regs_t *r)
{
    int nfds = (int)r->rdi;
    (void)nfds; (void)r;
    /* Return ready count 1 by default */
    return 1;
}

static s64 sys_pselect6_impl(pt_regs_t *r)
{
    return sys_select_impl(r);
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

    for (int i = 0; i < 64; i++) {
        if (parent->handle_table[i]) {
            file_t *pf = (file_t *)parent->handle_table[i];
            __atomic_add_fetch(&pf->f_count, 1, __ATOMIC_SEQ_CST);
            child->handle_table[i] = pf;
        }
        if (parent->obj_handle_table[i]) {
            az_object_t *obj = parent->obj_handle_table[i];
            az_object_reference(obj);
            child->obj_handle_table[i] = obj;
        }
    }

    thread_t *t = thread_create(child, r->rip, r->rsp, false);
    if (!t) {
        proc_destroy(child);
        return -(s64)ENOMEM;
    }

    if (t->user_regs) {
        *t->user_regs = *r;
        t->user_regs->rax = 0;      /* Child returns 0 */
        t->user_regs->rflags = 0x202; /* IF=1 */
    }

    return (s64)child->pid;
}

static s64 sys_execve_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    const char *const *user_argv = (const char *const *)r->rsi;
    const char *const *user_envp = (const char *const *)r->rdx;

    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    /* Copy argv array from user space */
    char *kargv[64];
    __builtin_memset(kargv, 0, sizeof(kargv));
    int argc = 0;
    if (user_argv && (uintptr_t)user_argv < 0x8000000000000000ULL) {
        for (int i = 0; i < 63; i++) {
            const char *arg_ptr = NULL;
            if (copy_from_user(&arg_ptr, &user_argv[i], sizeof(char *)) != 0) break;
            if (!arg_ptr) break;
            if ((uintptr_t)arg_ptr >= 0x8000000000000000ULL) break;

            char *buf = (char *)kmalloc(256);
            if (!buf) break;
            if (copy_from_user(buf, arg_ptr, 255) != 0) {
                kfree(buf);
                break;
            }
            buf[255] = '\0';
            kargv[argc++] = buf;
        }
    }
    if (argc == 0) {
        kargv[0] = strdup(kpath);
        argc = 1;
    }
    kargv[argc] = NULL;

    /* Copy envp array from user space */
    char *kenvp[64];
    __builtin_memset(kenvp, 0, sizeof(kenvp));
    int envc = 0;
    if (user_envp && (uintptr_t)user_envp < 0x8000000000000000ULL) {
        for (int i = 0; i < 63; i++) {
            const char *env_ptr = NULL;
            if (copy_from_user(&env_ptr, &user_envp[i], sizeof(char *)) != 0) break;
            if (!env_ptr) break;
            if ((uintptr_t)env_ptr >= 0x8000000000000000ULL) break;

            char *buf = (char *)kmalloc(256);
            if (!buf) break;
            if (copy_from_user(buf, env_ptr, 255) != 0) {
                kfree(buf);
                break;
            }
            buf[255] = '\0';
            kenvp[envc++] = buf;
        }
    }
    kenvp[envc] = NULL;

    process_t *proc = sched_current_process();
    if (!proc) {
        for (int i = 0; i < argc; i++) kfree(kargv[i]);
        for (int i = 0; i < envc; i++) kfree(kenvp[i]);
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

    if (err < 0) return (s64)err;

    ipc_shmem_unmap_all(proc);

    proc->pml4_phys = new_space;
    vmm_switch(new_space);

    if (old_space && old_space != vmm_kernel_space()) {
        vmm_destroy_space(old_space);
    }

    r->rip = (u64)new_entry;
    r->rsp = (u64)new_rsp;
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

static s64 sys_kill_impl(pt_regs_t *r)
{
    s32 pid = (s32)r->rdi;
    int sig = (int)r->rsi;
    if (pid <= 0) return -(s64)EINVAL;

    process_t *curr = sched_current_process();
    if (!curr) return -(s64)EPERM;

    if (pid == (s32)curr->pid) {
        if (sig != 0) {
            sys_exit_impl(r);
        }
        return 0;
    }

    return sched_kill_process((u32)pid, sig);
}

/* ── Signals ─────────────────────────────────────────────────────────────── */

static s64 sys_rt_sigaction_impl(pt_regs_t *r)
{
    (void)r;
    return 0;
}

static s64 sys_rt_sigprocmask_impl(pt_regs_t *r)
{
    (void)r;
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
    sched_yield();
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
    (void)domain; (void)type; (void)protocol;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Create dummy socket device */
    for (int i = 0; i < 64; i++) {
        if (!proc->handle_table[i]) {
            file_t *f = vfs_open("/dev/net0", O_RDWR, 0);
            if (!f) f = vfs_open("/dev/null", O_RDWR, 0);
            proc->handle_table[i] = f;
            return i;
        }
    }
    return -(s64)EMFILE;
}

static s64 sys_bind_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_connect_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_listen_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_accept_impl(pt_regs_t *r) { return sys_socket_impl(r); }
static s64 sys_shutdown_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_getsockname_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_getpeername_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_setsockopt_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_getsockopt_impl(pt_regs_t *r) { (void)r; return 0; }

static s64 sys_sendto_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const void *buf = (const void *)r->rsi;
    size_t len = (size_t)r->rdx;
    (void)fd;
    if (!buf || len == 0) return 0;
    return (s64)len;
}

static s64 sys_recvfrom_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    void *buf = (void *)r->rsi;
    size_t len = (size_t)r->rdx;
    (void)fd; (void)buf; (void)len;
    return -(s64)EAGAIN;
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

    int fds[2] = { fd0, fd1 };
    if (copy_to_user(user_fds, fds, sizeof(fds)) != 0) {
        proc->handle_table[fd0] = NULL;
        proc->handle_table[fd1] = NULL;
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EFAULT;
    }
    return 0;
}

static s64 sys_pipe2_impl(pt_regs_t *r)
{
    return sys_pipe_impl(r);
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
    return newfd;
}

static s64 sys_dup3_impl(pt_regs_t *r)
{
    return sys_dup2_impl(r);
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
    case 0: { /* F_DUPFD */
        int minfd = (int)arg;
        if (minfd < 0 || minfd >= 64) return -(s64)EINVAL;
        for (int i = minfd; i < 64; i++) {
            if (!proc->handle_table[i]) {
                __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
                proc->handle_table[i] = f;
                return i;
            }
        }
        return -(s64)EMFILE;
    }
    case 1: /* F_GETFD */
        return 0;
    case 2: /* F_SETFD */
        return 0;
    case 3: /* F_GETFL */
        return f->f_flags;
    case 4: /* F_SETFL */
        f->f_flags = (u32)arg;
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
    char kpath[256];
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

static s64 sys_unlink_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[256];
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
    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_mkdir(kpath, mode);
}

static s64 sys_rmdir_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_rmdir(kpath);
}

static s64 sys_truncate_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    s64 length = (s64)r->rsi;
    if (length < 0) return -(s64)EINVAL;

    char kpath[256];
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
    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    struct stat st;
    return vfs_stat(kpath, &st);
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

struct linux_timespec {
    long tv_sec;
    long tv_nsec;
};

static s64 sys_nanosleep_impl(pt_regs_t *r)
{
    const struct linux_timespec *req = (const struct linux_timespec *)r->rdi;
    if (!req) return -(s64)EINVAL;
    if ((uintptr_t)req >= 0x8000000000000000ULL) return -(s64)EFAULT;
    
    struct linux_timespec t;
    if (copy_from_user(&t, req, sizeof(t)) != 0) return -(s64)EFAULT;
    
    u64 ticks = (t.tv_sec * 100) + (t.tv_nsec / 10000000);
    if (ticks == 0 && (t.tv_sec > 0 || t.tv_nsec > 0)) ticks = 1;
    
    sched_sleep(ticks);
    return 0;
}

static s64 sys_clock_gettime_impl(pt_regs_t *r)
{
    void *tp = (void *)r->rsi;
    if (!tp) return -(s64)EINVAL;
    if ((uintptr_t)tp >= 0x8000000000000000ULL) return -(s64)EFAULT;

    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_sec = rtc_to_unix_time(&t);

    u64 ts[2] = { unix_sec, 0 };
    if (copy_to_user(tp, ts, sizeof(ts)) != 0) return -(s64)EFAULT;

    return 0;
}

struct linux_timeval {
    long tv_sec;
    long tv_usec;
};

static s64 sys_gettimeofday_impl(pt_regs_t *r)
{
    struct linux_timeval *user_tv = (struct linux_timeval *)r->rdi;
    if (!user_tv) return 0;
    if ((uintptr_t)user_tv >= 0x8000000000000000ULL) return -(s64)EFAULT;

    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_sec = rtc_to_unix_time(&t);
    struct linux_timeval tv = { (long)unix_sec, 0 };
    if (copy_to_user(user_tv, &tv, sizeof(tv)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_time_impl(pt_regs_t *r)
{
    long *user_tloc = (long *)r->rdi;
    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_sec = rtc_to_unix_time(&t);

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

static s64 sys_uname_impl(pt_regs_t *r)
{
    struct utsname *u = (struct utsname *)r->rdi;
    if (!u || (uintptr_t)u >= 0x8000000000000000ULL) return -(s64)EFAULT;

    struct utsname info;
    memset(&info, 0, sizeof(info));
    strncpy(info.sysname, "AzamiOS", sizeof(info.sysname) - 1);
    strncpy(info.nodename, "azami", sizeof(info.nodename) - 1);
    strncpy(info.release, "7.0.0-posix", sizeof(info.release) - 1);
    strncpy(info.version, "AzamiOS Modular Microkernel v7.0 x86_64 SMP", sizeof(info.version) - 1);
    strncpy(info.machine, "x86_64", sizeof(info.machine) - 1);
    strncpy(info.domainname, "local", sizeof(info.domainname) - 1);

    if (copy_to_user(u, &info, sizeof(info)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sys_reboot_impl(pt_regs_t *r)
{
    u32 cmd = (u32)r->rdx;
    if (cmd == 0x01234567 /* LINUX_REBOOT_CMD_RESTART */) {
        power_reboot();
    } else {
        power_shutdown();
    }
    __builtin_unreachable();
}

static s64 sys_getuid_impl(pt_regs_t *r)  { (void)r; return 0; }
static s64 sys_geteuid_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_getgid_impl(pt_regs_t *r)  { (void)r; return 0; }
static s64 sys_getegid_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_setuid_impl(pt_regs_t *r)  { (void)r; return 0; }
static s64 sys_setgid_impl(pt_regs_t *r)  { (void)r; return 0; }
static s64 sys_getpgrp_impl(pt_regs_t *r) {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 0;
}
static s64 sys_setpgid_impl(pt_regs_t *r) { (void)r; return 0; }
static s64 sys_setsid_impl(pt_regs_t *r)  {
    (void)r;
    process_t *proc = sched_current_process();
    return proc ? (s64)proc->pid : 0;
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
    if (virt >= 0x8000000000000000ULL) return -(s64)EINVAL;

    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;

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
    ipc_shmem_put(shmem);
    ipc_shmem_destroy(shmem);
    return 0;
}

static s64 sys_az_shmem_unmap(pt_regs_t *r)
{
    u32 shmem_id = (u32)r->rdi;
    virt_addr_t virt = (virt_addr_t)r->rsi;
    
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EINVAL;
    
    ipc_shmem_t *shmem = ipc_shmem_find(shmem_id);
    if (!shmem) return -(s64)EINVAL;

    s64 ret = ipc_shmem_unmap(shmem, proc, virt);
    ipc_shmem_put(shmem);
    return ret;
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
    size_t      fb_size = bga_get_fb_size();

    if (!fb_phys) {
        struct limine_framebuffer *fb = az_boot_framebuffer();
        if (!fb) return -(s64)ENODEV;
        fb_phys = (phys_addr_t)((u64)(uintptr_t)fb->address - HHDM_BASE);
        fb_size = (size_t)(fb->pitch * fb->height);
    }

    size_t page_count = (fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 flags = VMM_F_PRESENT | VMM_F_WRITE | VMM_F_USER | VMM_F_PCD | VMM_F_PWT | VMM_F_NX | VMM_F_SHARED;
    for (size_t i = 0; i < page_count; i++) {
        vmm_map(proc->pml4_phys, virt + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE, flags);
    }

    console_disable_fb();
    return 0;
}

static s64 sys_az_spawn(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    if (!user_path || (uintptr_t)user_path >= 0x8000000000000000ULL) return -(s64)EFAULT;

    char kpath[256];
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

    if (!entry || !stack) return -(s64)EINVAL;
    if (entry >= 0x8000000000000000ULL || stack >= 0x8000000000000000ULL) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    thread_t *t = thread_create(proc, entry, stack, false);
    if (!t) return -(s64)ENOMEM;

    sched_unblock(t);
    return (s64)t->tid;
}

static s64 sys_az_object_create(pt_regs_t *r) { (void)r; return -(s64)ENOSYS; }
static s64 sys_az_object_open(pt_regs_t *r)   { (void)r; return -(s64)ENOSYS; }
static s64 sys_az_object_close(pt_regs_t *r)  { (void)r; return -(s64)ENOSYS; }

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
    kmsg.sender_pid = 51;
    kmsg.msg_type   = 0;
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

    process_t *kproc = sched_current_process();
    thread_t *t = thread_create(kproc, (uintptr_t)az_timer_thread, (uintptr_t)ctx, true);
    if (!t) {
        kfree(ctx);
        return -(s64)ENOMEM;
    }

    return 0;
}