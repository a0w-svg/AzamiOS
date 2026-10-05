/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Filesystem and I/O Syscalls
 * File: kernel/syscall/sys_fs.c
 * ============================================================================ */
#include "syscall_internal.h"

/* ── Standard I/O Syscalls ───────────────────────────────────────────────── */

s64 sys_read_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    char *buf = (char *)r->rsi;
    s64 count = (s64)r->rdx;
    /* BUG-01: negative count is EINVAL; zero count returns 0 immediately */
    if (count < 0) return -(s64)EINVAL;
    if (count == 0 || !buf) return 0;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    file_t *file = fget(proc, fd);

    if (fd == 0 && !file) {
        int c = console_getc();
        if (c != -1) {
            char kchar = (char)c;
            if (copy_to_user(buf, &kchar, 1) != 0) return -(s64)EFAULT;
            return 1;
        }
        return -(s64)EAGAIN;
    }

    if (!file) return -(s64)EBADF;

    s64 total_read = 0;
    char stack_buf[4096];
    char *kbuf = stack_buf;
    size_t max_chunk = sizeof(stack_buf);
    bool allocated = false;

    if (count > (s64)sizeof(stack_buf)) {
        size_t alloc_sz = (count > 65536) ? 65536 : (size_t)count;
        char *dyn = (char *)kmalloc(alloc_sz);
        if (dyn) {
            kbuf = dyn;
            max_chunk = alloc_sz;
            allocated = true;
        }
    }

    while (count > 0) {
        size_t chunk = count > (s64)max_chunk ? max_chunk : (size_t)count;
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
    if (allocated) kfree(kbuf);
    fput(file);
    return total_read;
}

s64 sys_write_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    const char *buf = (const char *)r->rsi;
    s64 count = (s64)r->rdx;
    if (count <= 0 || !buf) return 0;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    file_t *file = fget(proc, fd);

    if ((fd == 1 || fd == 2) && !file) {
        char kbuf[1024];
        s64 total_written = 0;
        while (count > 0) {
            size_t chunk = (size_t)count > sizeof(kbuf) ? sizeof(kbuf) : (size_t)count;
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
    char stack_buf[4096];
    char *kbuf = stack_buf;
    size_t max_chunk = sizeof(stack_buf);
    bool allocated = false;

    if (count > (s64)sizeof(stack_buf)) {
        size_t alloc_sz = (count > 65536) ? 65536 : (size_t)count;
        char *dyn = (char *)kmalloc(alloc_sz);
        if (dyn) {
            kbuf = dyn;
            max_chunk = alloc_sz;
            allocated = true;
        }
    }

    while (count > 0) {
        size_t chunk = count > (s64)max_chunk ? max_chunk : (size_t)count;
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
    if (allocated) kfree(kbuf);
    fput(file);
    return total_written;
}

s64 sys_open_impl(pt_regs_t *r)
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

s64 sys_close_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc) return -(s64)EBADF;
    file_t *f = fd_detach(proc, fd);
    if (!f) return -(s64)EBADF;
    vfs_close(f);
    return 0;
}

s64 sys_close_range_impl(pt_regs_t *r)
{
    unsigned int first = (unsigned int)r->rdi;
    unsigned int last  = (unsigned int)r->rsi;
    unsigned int flags = (unsigned int)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    if (first > last) return -(s64)EINVAL;
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC))
        return -(s64)EINVAL;

    /* Each AzamiOS process owns its descriptor table: clone()/fork() copies
     * descriptors and this kernel has no CLONE_FILES sharing. Consequently
     * CLOSE_RANGE_UNSHARE is already satisfied and needs no copy-on-write
     * work. Keep accepting the Linux flag so portable process launchers do
     * not need a kernel-specific fallback. */
    if (last >= PROC_MAX_FDS) last = PROC_MAX_FDS - 1;

    /* CLOSE_RANGE_CLOEXEC changes descriptor flags without closing the
     * underlying files. Hold the table lock across the range so execve() and
     * fcntl(F_SETFD) cannot observe a partially updated slot. */
    if (flags & CLOSE_RANGE_CLOEXEC) {
        if (first >= PROC_MAX_FDS) return 0;
        irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
        for (unsigned int i = first; i <= last; i++) {
            if (proc->handle_table[i]) proc->fd_flags[i] |= FD_CLOEXEC;
        }
        spinlock_unlock_irqrestore(&proc->fd_lock, fl);
        return 0;
    }

    if (first >= PROC_MAX_FDS) return 0;
    for (unsigned int i = first; i <= last && i < PROC_MAX_FDS; i++) {
        file_t *f = fd_detach(proc, (int)i);
        if (f) vfs_close(f);
    }
    return 0;
}

/* ── Scatter-Gather I/O ──────────────────────────────────────────────────── */


s64 sys_readv_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    int iovcnt = (int)r->rdx;

    if (!iov || iovcnt <= 0 || iovcnt > 1024) return -(s64)EINVAL;
    if ((uintptr_t)iov >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_writev_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct iovec *iov = (const struct iovec *)r->rsi;
    int iovcnt = (int)r->rdx;

    if (!iov || iovcnt <= 0 || iovcnt > 1024) return -(s64)EINVAL;
    if ((uintptr_t)iov >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_pread64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    void *user_buf = (void *)r->rsi;
    size_t count = (size_t)r->rdx;
    u64 pos = (u64)r->r10;

    if (!user_buf || count == 0) return 0;
    if ((uintptr_t)user_buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char *kbuf = kmalloc(count > 65536 ? 65536 : count);
    if (!kbuf) {
        fput(file);
        return -(s64)ENOMEM;
    }

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
            fput(file);
            return n;
        }

        if (copy_to_user((char *)user_buf + total_read, kbuf, (size_t)n) != 0) {
            kfree(kbuf);
            fput(file);
            return -(s64)EFAULT;
        }

        total_read += (size_t)n;
        if ((size_t)n < chunk) break;
    }

    kfree(kbuf);
    fput(file);
    return (s64)total_read;
}

s64 sys_pwrite64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    const void *user_buf = (const void *)r->rsi;
    size_t count = (size_t)r->rdx;
    u64 pos = (u64)r->r10;

    if (!user_buf || count == 0) return 0;
    if ((uintptr_t)user_buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char *kbuf = kmalloc(count > 65536 ? 65536 : count);
    if (!kbuf) {
        fput(file);
        return -(s64)ENOMEM;
    }

    size_t total_written = 0;
    while (total_written < count) {
        size_t chunk = count - total_written;
        if (chunk > 65536) chunk = 65536;

        if (copy_from_user(kbuf, (const char *)user_buf + total_written, chunk) != 0) {
            kfree(kbuf);
            fput(file);
            return -(s64)EFAULT;
        }

        u64 saved_pos = file->f_pos;
        file->f_pos = pos + total_written;
        s64 n = vfs_write(file, kbuf, chunk);
        file->f_pos = saved_pos;

        if (n <= 0) {
            if (total_written > 0) break;
            kfree(kbuf);
            fput(file);
            return n;
        }

        total_written += (size_t)n;
        if ((size_t)n < chunk) break;
    }

    kfree(kbuf);
    fput(file);
    return (s64)total_written;
}

/* ── File Operations & Metadata ─────────────────────────────────────────── */

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

s64 sys_ioctl_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    u32 cmd = (u32)r->rsi;
    u64 arg = r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc) return -(s64)EBADF;
    
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    s64 ret = 0;

    /* Generic file descriptor ioctls */
    if (cmd == 0x5451 /* FIOCLEX */) {
        proc->fd_flags[fd] |= FD_CLOEXEC;
        ret = 0;
        goto out;
    }
    if (cmd == 0x5450 /* FIONCLEX */) {
        proc->fd_flags[fd] &= ~FD_CLOEXEC;
        ret = 0;
        goto out;
    }
    if (cmd == 0x5421 /* FIONBIO */) {
        if (!arg || (uintptr_t)arg >= TASK_SIZE_MAX) { ret = -(s64)EFAULT; goto out; }
        int on = 0;
        if (copy_from_user(&on, (void *)arg, sizeof(int)) != 0) { ret = -(s64)EFAULT; goto out; }
        if (on) file->f_flags |= O_NONBLOCK;
        else    file->f_flags &= ~O_NONBLOCK;
        ret = 0;
        goto out;
    }
    if (cmd == 0x5452 /* FIOASYNC */) {
        if (!arg || (uintptr_t)arg >= TASK_SIZE_MAX) { ret = -(s64)EFAULT; goto out; }
        int on = 0;
        if (copy_from_user(&on, (void *)arg, sizeof(int)) != 0) { ret = -(s64)EFAULT; goto out; }
        if (on) file->f_flags |= 0x2000 /* O_ASYNC */;
        else    file->f_flags &= ~0x2000 /* O_ASYNC */;
        ret = 0;
        goto out;
    }

    /* Network configuration ioctl privilege checks */
    if (cmd == 0x8916 /* SIOCSIFADDR */ || cmd == 0x891c /* SIOCSIFNETMASK */ ||
        cmd == 0x891e /* SIOCSIFGW */   || cmd == 0x8921 /* SIOCSIFDNS */ ||
        cmd == 0x892a || cmd == 0x892b ||
        cmd == 0x8914 /* SIOCSIFFLAGS */ || cmd == 0x8990 /* SIOCSIFDHCP */) {
        if (!security_check_permission(proc, CAP_NET_ADMIN)) {
            ret = -(s64)EPERM;
            goto out;
        }
    }

    /* Try file operations driver ioctl first if implemented */
    if (file->f_op && file->f_op->ioctl) {
        s64 r_drv = file->f_op->ioctl(file, cmd, arg);
        if (r_drv != -(s64)ENOTTY && r_drv != -(s64)ENOSYS) {
            ret = r_drv;
            goto out;
        }
    }

    /* TTY ioctl commands fallback: only valid for character devices / TTYs */
    if (cmd == 0x5401 /* TCGETS */ || cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ ||
        cmd == 0x5404 /* TCSETSF */ || cmd == 0x5413 /* TIOCGWINSZ */ || cmd == 0x5414 /* TIOCSWINSZ */ ||
        cmd == 0x540F /* TIOCGPGRP */ || cmd == 0x5410 /* TIOCSPGRP */ || cmd == 0x540E /* TIOCSCTTY */ ||
        cmd == 0x5409 /* TCSBRK */ || cmd == 0x540A /* TCXONC */ || cmd == 0x540B /* TCFLSH */ ||
        cmd == 0x5429 /* TIOCGSID */) {
        if (file->f_inode && !S_ISCHR(file->f_inode->i_mode)) {
            ret = -(s64)ENOTTY;
            goto out;
        }
        if (cmd == 0x5413 /* TIOCGWINSZ */) {
            if (arg && (uintptr_t)arg < TASK_SIZE_MAX) {
                struct winsize ws;
                ws.ws_row = 24;
                ws.ws_col = 80;
                ws.ws_xpixel = 640;
                ws.ws_ypixel = 480;
                if (copy_to_user((void *)arg, &ws, sizeof(ws)) == 0) { ret = 0; goto out; }
                ret = -(s64)EFAULT;
                goto out;
            }
            ret = -(s64)EINVAL;
            goto out;
        }
        if (cmd == 0x5414 /* TIOCSWINSZ */) { ret = 0; goto out; }
        if (cmd == 0x5401 /* TCGETS */) {
            if (arg && (uintptr_t)arg < TASK_SIZE_MAX) {
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
                if (copy_to_user((void *)arg, termios_buf, 60) == 0) { ret = 0; goto out; }
                ret = -(s64)EFAULT;
                goto out;
            }
            ret = -(s64)EINVAL;
            goto out;
        }
        if (cmd == 0x5402 /* TCSETS */ || cmd == 0x5403 /* TCSETSW */ || cmd == 0x5404 /* TCSETSF */) {
            if (!arg || (uintptr_t)arg >= TASK_SIZE_MAX) { ret = -(s64)EINVAL; goto out; }
            char dummy[60];
            if (copy_from_user(dummy, (const void *)arg, 60) != 0) { ret = -(s64)EFAULT; goto out; }
            ret = 0;
            goto out;
        }
        if (cmd == 0x5409 /* TCSBRK */ || cmd == 0x540A /* TCXONC */ || cmd == 0x540B /* TCFLSH */) { ret = 0; goto out; }
        if (cmd == 0x540F /* TIOCGPGRP */) {
            if (arg && (uintptr_t)arg < TASK_SIZE_MAX) {
                int pgid = (int)proc->pgid;
                if (copy_to_user((void *)arg, &pgid, sizeof(int)) == 0) { ret = 0; goto out; }
                ret = -(s64)EFAULT;
                goto out;
            }
            ret = -(s64)EINVAL;
            goto out;
        }
        if (cmd == 0x5410 /* TIOCSPGRP */) {
            if (arg && (uintptr_t)arg < TASK_SIZE_MAX) {
                int pgid = 0;
                if (copy_from_user(&pgid, (const void *)arg, sizeof(int)) != 0) { ret = -(s64)EFAULT; goto out; }
                proc->pgid = (u32)pgid;
                ret = 0;
                goto out;
            }
            ret = -(s64)EINVAL;
            goto out;
        }
        if (cmd == 0x5429 /* TIOCGSID */) {
            if (arg && (uintptr_t)arg < TASK_SIZE_MAX) {
                int sid = (int)proc->sid;
                if (copy_to_user((void *)arg, &sid, sizeof(int)) == 0) { ret = 0; goto out; }
                ret = -(s64)EFAULT;
                goto out;
            }
            ret = -(s64)EINVAL;
            goto out;
        }
        if (cmd == 0x540E /* TIOCSCTTY */) {
            proc->sid = proc->pid;
            ret = 0;
            goto out;
        }
        if (cmd == 0x5422 /* TIOCNOTTY */) { ret = 0; goto out; }
    }

    ret = vfs_ioctl(file, cmd, arg);

out:
    fput(file);
    return ret;
}

s64 sys_lseek_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    s64 offset = (s64)r->rsi;
    int whence = (int)r->rdx;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 _ret = vfs_lseek(file, offset, whence);
    fput(file);
    return _ret;
}

s64 sys_stat_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!user_path || !statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_lstat_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!user_path || !statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_fstat_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    struct stat *statbuf = (struct stat *)r->rsi;
    if (!statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= TASK_SIZE_MAX) return -(s64)EFAULT;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    struct stat kstat;
    s64 ret = vfs_fstat(file, &kstat);
    if (ret == 0) {
        if (copy_to_user(statbuf, &kstat, sizeof(struct stat)) != 0) {
            return -(s64)EFAULT;
        }
    }
    s64 _ret = ret;
    fput(file);
    return _ret;
}

s64 sys_statfs_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    struct statfs *buf = (struct statfs *)r->rsi;
    if (!user_path || !buf) return -(s64)EINVAL;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_fstatfs_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    struct statfs *buf = (struct statfs *)r->rsi;
    if (!buf) return -(s64)EINVAL;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    struct statfs kbuf;
    s64 ret = vfs_fstatfs(file, &kbuf);
    if (ret == 0) {
        if (copy_to_user(buf, &kbuf, sizeof(struct statfs)) != 0) return -(s64)EFAULT;
    }
    s64 _ret = ret;
    fput(file);
    return _ret;
}

s64 sys_chmod_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    u32 mode = (u32)r->rsi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_chmod(kpath, mode);
}

s64 sys_fchmod_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    u32 mode = (u32)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 ret = vfs_fchmod(file, mode);
    fput(file);
    return ret;
}

s64 sys_chown_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    u32 uid = (u32)r->rsi;
    u32 gid = (u32)r->rdx;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_chown(kpath, uid, gid);
}

s64 sys_fchown_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    u32 uid = (u32)r->rsi;
    u32 gid = (u32)r->rdx;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 ret = vfs_fchown(file, uid, gid);
    fput(file);
    return ret;
}

s64 sys_umask_impl(pt_regs_t *r)
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

s64 sys_symlink_impl(pt_regs_t *r)
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

s64 sys_readlink_impl(pt_regs_t *r)
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

s64 sys_poll_impl(pt_regs_t *r)
{
    struct pollfd *user_fds = (struct pollfd *)r->rdi;
    u64 nfds = r->rsi;
    int timeout_ms = (int)r->rdx;

    if (nfds > 1024) return -(s64)EINVAL;
    if (nfds == 0) {
        if (timeout_ms > 0) sched_sleep(((u64)timeout_ms + 9) / 10);
        return 0;
    }
    if (!user_fds || (uintptr_t)user_fds >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    struct pollfd *kfds = (struct pollfd *)kmalloc(sizeof(struct pollfd) * nfds);
    if (!kfds) return -(s64)ENOMEM;

    if (copy_from_user(kfds, user_fds, sizeof(struct pollfd) * nfds) != 0) {
        kfree(kfds);
        return -(s64)EFAULT;
    }

    u64 end_ticks = (timeout_ms > 0) ? (sched_get_ticks() + ((u64)timeout_ms + 9) / 10) : 0;
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

    size_t not_copied = copy_to_user(user_fds, kfds, sizeof(struct pollfd) * nfds);
    kfree(kfds);
    if (not_copied != 0) return -(s64)EFAULT;
    return ready_count;
}

s64 sys_ppoll_impl(pt_regs_t *r)
{
    struct pollfd *user_fds = (struct pollfd *)r->rdi;
    u64 nfds = r->rsi;
    const struct linux_timespec *tmo_p = (const struct linux_timespec *)r->rdx;
    int timeout_ms = -1;
    if (tmo_p && (uintptr_t)tmo_p < TASK_SIZE_MAX) {
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

s64 sys_select_impl(pt_regs_t *r)
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

    if (u_rfds && (uintptr_t)u_rfds < TASK_SIZE_MAX) {
        if (copy_from_user(&in_rfds, u_rfds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }
    if (u_wfds && (uintptr_t)u_wfds < TASK_SIZE_MAX) {
        if (copy_from_user(&in_wfds, u_wfds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }
    if (u_efds && (uintptr_t)u_efds < TASK_SIZE_MAX) {
        if (copy_from_user(&in_efds, u_efds, sizeof(kernel_fd_set_t)) != 0) return -(s64)EFAULT;
    }

    int timeout_ms = -1;
    if (u_tv && (uintptr_t)u_tv < TASK_SIZE_MAX) {
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

    u64 end_ticks = (timeout_ms > 0) ? (sched_get_ticks() + ((u64)timeout_ms + 9) / 10) : 0;
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

    if (u_rfds && (uintptr_t)u_rfds < TASK_SIZE_MAX) copy_to_user(u_rfds, &out_rfds, sizeof(kernel_fd_set_t));
    if (u_wfds && (uintptr_t)u_wfds < TASK_SIZE_MAX) copy_to_user(u_wfds, &out_wfds, sizeof(kernel_fd_set_t));
    if (u_efds && (uintptr_t)u_efds < TASK_SIZE_MAX) copy_to_user(u_efds, &out_efds, sizeof(kernel_fd_set_t));

    return ready_count;
}

s64 sys_pselect6_impl(pt_regs_t *r)
{
    int nfds = (int)r->rdi;
    kernel_fd_set_t *u_rfds = (kernel_fd_set_t *)r->rsi;
    kernel_fd_set_t *u_wfds = (kernel_fd_set_t *)r->rdx;
    kernel_fd_set_t *u_efds = (kernel_fd_set_t *)r->r10;
    const struct linux_timespec *u_ts = (const struct linux_timespec *)r->r8;

    struct linux_timeval tv;
    struct linux_timeval *tv_ptr = NULL;
    if (u_ts && (uintptr_t)u_ts < TASK_SIZE_MAX) {
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

/* ── Pipes & File Descriptors ────────────────────────────────────────────── */

s64 sys_pipe_impl(pt_regs_t *r)
{
    int *user_fds = (int *)r->rdi;
    if (!user_fds || (uintptr_t)user_fds >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_pipe2_impl(pt_regs_t *r)
{
    int *user_fds = (int *)r->rdi;
    int flags = (int)r->rsi;
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) return -(s64)EINVAL;
    if (!user_fds || (uintptr_t)user_fds >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_dup_impl(pt_regs_t *r)
{
    int oldfd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc || oldfd < 0 || oldfd >= PROC_MAX_FDS) return -(s64)EBADF;

    irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
    file_t *f = (file_t *)proc->handle_table[oldfd];
    if (!f || (uintptr_t)f < 0xFFFF800000000000ULL) {
        spinlock_unlock_irqrestore(&proc->fd_lock, fl);
        return -(s64)EBADF;
    }
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        if (!proc->handle_table[i]) {
            __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
            proc->handle_table[i] = f;
            proc->fd_flags[i] = 0; /* dup clears FD_CLOEXEC */
            spinlock_unlock_irqrestore(&proc->fd_lock, fl);
            return i;
        }
    }
    spinlock_unlock_irqrestore(&proc->fd_lock, fl);
    return -(s64)EMFILE;
}

static s64 do_dup2(int oldfd, int newfd, u8 fd_flags)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (oldfd < 0 || oldfd >= PROC_MAX_FDS) return -(s64)EBADF;
    if (newfd < 0 || newfd >= PROC_MAX_FDS) return -(s64)EBADF;

    irqflags_t fl = spinlock_lock_irqsave(&proc->fd_lock);
    file_t *f = (file_t *)proc->handle_table[oldfd];
    if (!f || (uintptr_t)f < 0xFFFF800000000000ULL) {
        spinlock_unlock_irqrestore(&proc->fd_lock, fl);
        return -(s64)EBADF;
    }
    if (oldfd == newfd) {                    /* POSIX: no-op, keep FD_CLOEXEC */
        spinlock_unlock_irqrestore(&proc->fd_lock, fl);
        return newfd;
    }
    file_t *victim = (file_t *)proc->handle_table[newfd];
    if (victim && (uintptr_t)victim < 0xFFFF800000000000ULL) victim = NULL;
    __atomic_add_fetch(&f->f_count, 1, __ATOMIC_SEQ_CST);
    proc->handle_table[newfd] = f;
    proc->fd_flags[newfd] = fd_flags; /* BUG-AL fix: set fd_flags atomically under the per-process fd_lock */
    spinlock_unlock_irqrestore(&proc->fd_lock, fl);

    if (victim) vfs_close(victim);           /* drop the replaced fd outside the lock */
    return newfd;
}

s64 sys_dup2_impl(pt_regs_t *r)
{
    return do_dup2((int)(s32)r->rdi, (int)(s32)r->rsi, 0);
}

s64 sys_dup3_impl(pt_regs_t *r)
{
    /* BUG-04: POSIX requires dup3(old, new, flags) to return EINVAL when oldfd == newfd */
    int oldfd = (int)(s32)r->rdi;
    int newfd = (int)(s32)r->rsi;
    int flags = (int)r->rdx;
    if (oldfd == newfd) return -(s64)EINVAL;
    if (flags & ~O_CLOEXEC) return -(s64)EINVAL;

    return do_dup2(oldfd, newfd, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
}

s64 sys_fcntl_impl(pt_regs_t *r)
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
        if (!arg || arg >= TASK_SIZE_MAX) return -(s64)EFAULT;
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
        if (!arg || arg >= TASK_SIZE_MAX) return -(s64)EFAULT;
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

s64 sys_getcwd_impl(pt_regs_t *r)
{
    char *user_buf = (char *)r->rdi;
    size_t size    = (size_t)r->rsi;
    if (!user_buf || size == 0) return -(s64)EINVAL;
    if ((uintptr_t)user_buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
    size_t len = strlen(cwd) + 1;

    if (size < len) return -(s64)ERANGE;
    if (copy_to_user(user_buf, cwd, len) != 0) return -(s64)EFAULT;
    return (s64)(uintptr_t)user_buf;
}

s64 sys_chdir_impl(pt_regs_t *r)
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

s64 sys_fchdir_impl(pt_regs_t *r)
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

s64 sys_unlink_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_unlink(kpath);
}

s64 sys_rename_impl(pt_regs_t *r)
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

s64 sys_mkdir_impl(pt_regs_t *r)
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

s64 sys_rmdir_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;
    return vfs_rmdir(kpath);
}

s64 sys_truncate_impl(pt_regs_t *r)
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

s64 sys_ftruncate_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    s64 length = (s64)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    if (length < 0) return -(s64)EINVAL;
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 ret = vfs_truncate(file, (u64)length);
    fput(file);
    return ret;
}

s64 sys_access_impl(pt_regs_t *r)
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

s64 sys_getdents_impl(pt_regs_t *r)
{
    return sys_getdents64_impl(r);
}

s64 sys_getdents64_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    void *dirp = (void *)r->rsi;
    size_t count = (size_t)r->rdx;
    
    if (!dirp || count == 0) return -(s64)EINVAL;
    if ((uintptr_t)dirp >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (count > 65536) count = 65536;
    
    process_t *proc = sched_current_process();
    if (fd < 0 || fd >= PROC_MAX_FDS || !proc || !proc->handle_table[fd]) return -(s64)EBADF;
    
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
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
    s64 _ret = ret;
    fput(file);
    return _ret;
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

s64 sys_utime_impl(pt_regs_t *r)
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

s64 sys_utimes_impl(pt_regs_t *r)
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

s64 sys_utimensat_impl(pt_regs_t *r)
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

/* ── POSIX *at Syscall Family ────────────────────────────────────────────── */

s64 sys_openat_impl(pt_regs_t *r)
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

s64 sys_mkdirat_impl(pt_regs_t *r)
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

s64 sys_fstatat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    struct stat *statbuf = (struct stat *)r->rdx;
    int flags = (int)r->r10;

    if (!statbuf) return -(s64)EINVAL;
    if ((uintptr_t)statbuf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_faccessat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int mode = (int)r->rdx;
    int flags = (int)r->r10;

    if (!user_path) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_unlinkat_impl(pt_regs_t *r)
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

s64 sys_readlinkat_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    char *buf = (char *)r->rdx;
    size_t bufsiz = (size_t)r->r10;

    if (!buf || bufsiz == 0) return -(s64)EINVAL;
    if ((uintptr_t)buf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_getfacl_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    acl_entry_t *user_entries = (acl_entry_t *)r->rsi;
    int max_entries = (int)(s32)r->rdx;

    if (!user_path || !user_entries || max_entries <= 0) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if ((uintptr_t)user_entries >= TASK_SIZE_MAX) return -(s64)EFAULT;

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

s64 sys_setfacl_impl(pt_regs_t *r)
{
    const char *user_path = (const char *)r->rdi;
    const acl_entry_t *user_entries = (const acl_entry_t *)r->rsi;
    int count = (int)(s32)r->rdx;

    if (!user_path || count < 0 || count > ACL_MAX_ENTRIES) return -(s64)EINVAL;
    if ((uintptr_t)user_path >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (count > 0 && (!user_entries || (uintptr_t)user_entries >= TASK_SIZE_MAX)) return -(s64)EFAULT;

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

#define RESOLVE_NO_XDEV       0x01
#define RESOLVE_NO_MAGICLINKS 0x02
#define RESOLVE_NO_SYMLINKS   0x04
#define RESOLVE_BENEATH       0x08
#define RESOLVE_IN_ROOT       0x10
#define RESOLVE_CACHED        0x20

struct kernel_open_how {
    u64 flags;
    u64 mode;
    u64 resolve;
};

static bool path_contains_symlink(const char *path)
{
    if (!path || !path[0]) return false;
    char comp_buf[512];
    size_t plen = strlen(path);
    if (plen >= sizeof(comp_buf)) return false;

    for (size_t i = 1; i <= plen; i++) {
        if (path[i] == '/' || path[i] == '\0') {
            memcpy(comp_buf, path, i);
            comp_buf[i] = '\0';
            dentry_t *d = NULL;
            if (vfs_path_lookup_nofollow(comp_buf, &d) == 0 && d && d->d_inode) {
                if (S_ISLNK(d->d_inode->i_mode)) return true;
            }
        }
    }
    return false;
}

s64 sys_openat2_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    const struct kernel_open_how *user_how = (const struct kernel_open_how *)r->rdx;
    size_t size = (size_t)r->r10;

    if (!user_path || !user_how || size < sizeof(struct kernel_open_how)) return -(s64)EINVAL;
    if (size > sizeof(struct kernel_open_how)) return -(s64)E2BIG;

    struct kernel_open_how how;
    if (copy_from_user(&how, user_how, sizeof(how)) != 0) return -(s64)EFAULT;

    if ((how.flags >> 32) != 0) return -(s64)EINVAL;
    if (how.resolve & ~(RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    char raw[512];
    if (copy_str_from_user(raw, user_path, sizeof(raw)) < 0) return -(s64)EFAULT;

    if (how.resolve & RESOLVE_BENEATH) {
        if (raw[0] == '/') return -(s64)EXDEV;

        int rel_depth = 0;
        const char *p = raw;
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            const char *start = p;
            while (*p && *p != '/') p++;
            size_t len = (size_t)(p - start);
            if (len == 1 && start[0] == '.') continue;
            if (len == 2 && start[0] == '.' && start[1] == '.') {
                if (rel_depth <= 0) return -(s64)EXDEV;
                rel_depth--;
            } else {
                rel_depth++;
            }
        }
    }

    char kpath[512];
    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), user_path);
    if (perr < 0) return perr;

    if (how.resolve & RESOLVE_BENEATH) {
        char base_path[512];
        if (dirfd == AT_FDCWD) {
            const char *cwd = (proc->cwd[0]) ? proc->cwd : "/";
            if (proc_is_confined(proc)) {
                vpath_to_real(proc, cwd, base_path, sizeof(base_path));
            } else {
                strncpy(base_path, cwd, sizeof(base_path) - 1);
                base_path[sizeof(base_path) - 1] = '\0';
            }
        } else {
            if (dirfd < 0 || dirfd >= PROC_MAX_FDS || !proc->handle_table[dirfd])
                return -(s64)EBADF;
            file_t *df = (file_t *)proc->handle_table[dirfd];
            if (!df || !df->f_dentry || !df->f_inode) return -(s64)EBADF;
            if (!S_ISDIR(df->f_inode->i_mode)) return -(s64)ENOTDIR;
            dentry_build_path(df->f_dentry, base_path, sizeof(base_path));
        }
        size_t blen = strlen(base_path);
        while (blen > 1 && base_path[blen - 1] == '/') blen--;
        if (strncmp(kpath, base_path, blen) != 0 ||
            (kpath[blen] != '/' && kpath[blen] != '\0')) {
            return -(s64)EXDEV;
        }
    }

    if (how.resolve & RESOLVE_NO_SYMLINKS) {
        if (path_contains_symlink(kpath)) {
            return -(s64)ELOOP;
        }
    }

    u32 mode = (u32)how.mode;
    if (how.flags & O_CREAT) {
        mode &= ~proc->umask;
    }

    s64 open_err = 0;
    file_t *file = vfs_open_err(kpath, (u32)how.flags, mode, &open_err);
    if (!file) return open_err ? open_err : -(s64)ENOENT;

    s64 fd = fd_install(proc, file, (how.flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) vfs_close(file);
    return fd;
}

s64 sys_faccessat2_impl(pt_regs_t *r)
{
    return sys_faccessat_impl(r);
}

s64 sys_epoll_pwait2_impl(pt_regs_t *r)
{
    int epfd = (int)(s32)r->rdi;
    void *events = (void *)r->rsi;
    int maxevents = (int)(s32)r->rdx;
    const struct linux_timespec *ts = (const struct linux_timespec *)r->r10;
    const sigset_t *sigmask = (const sigset_t *)r->r8;
    size_t sigsetsize = (size_t)r->r9;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    int timeout = -1;
    if (ts) {
        if ((uintptr_t)ts >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct linux_timespec kts;
        if (copy_from_user(&kts, ts, sizeof(kts)) != 0) return -(s64)EFAULT;
        if (kts.tv_sec < 0 || kts.tv_nsec < 0 || kts.tv_nsec >= 1000000000L)
            return -(s64)EINVAL;
        timeout = (int)(kts.tv_sec * 1000 + kts.tv_nsec / 1000000);
    }

    sigset_t old_mask = proc->sig_blocked;
    if (sigmask) {
        if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
        if ((uintptr_t)sigmask >= TASK_SIZE_MAX) return -(s64)EFAULT;
        sigset_t kmask;
        if (copy_from_user(&kmask, sigmask, sizeof(sigset_t)) != 0) return -(s64)EFAULT;
        proc->sig_blocked = kmask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }

    pt_regs_t fake_r;
    fake_r.rdi = (u64)epfd;
    fake_r.rsi = (u64)events;
    fake_r.rdx = (u64)maxevents;
    fake_r.r10 = (u64)timeout;
    s64 ret = sys_epoll_wait_impl(&fake_r);

    if (sigmask) {
        proc->sig_blocked = old_mask;
    }
    return ret;
}


/* ── Linux sendfile, copy_file_range, fallocate, statx, splice ──────────── */

s64 sys_sendfile_impl(pt_regs_t *r)
{
    int out_fd = (int)(s32)r->rdi;
    int in_fd = (int)(s32)r->rsi;
    s64 *user_offset = (s64 *)r->rdx;
    size_t count = (size_t)r->r10;

    if (count == 0) return 0;
    if (out_fd < 0 || out_fd >= PROC_MAX_FDS || in_fd < 0 || in_fd >= PROC_MAX_FDS) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EBADF;

    file_t *out_file = fget(proc, out_fd);
    if (!out_file) return -(s64)EBADF;

    file_t *in_file = fget(proc, in_fd);
    if (!in_file) {
        fput(out_file);
        return -(s64)EBADF;
    }

    if ((in_file->f_flags & 3) == O_WRONLY || (out_file->f_flags & 3) == O_RDONLY) {
        fput(in_file);
        fput(out_file);
        return -(s64)EBADF;
    }

    if (out_file->f_flags & O_APPEND) {
        fput(in_file);
        fput(out_file);
        return -(s64)EINVAL;
    }

    s64 current_off = 0;
    bool use_off = false;
    if (user_offset) {
        if ((uintptr_t)user_offset >= TASK_SIZE_MAX) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (copy_from_user(&current_off, user_offset, sizeof(s64)) != 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (current_off < 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EINVAL;
        }
        use_off = true;
    }

    size_t total_transferred = 0;
    char kbuf[4096];
    s64 err = 0;

    while (total_transferred < count) {
        if (proc->sig_pending & ~proc->sig_blocked) {
            if (total_transferred == 0) err = -(s64)EINTR;
            break;
        }

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

        if (nread <= 0) {
            if (nread < 0 && total_transferred == 0) err = nread;
            break;
        }

        s64 nwritten = (s64)vfs_write(out_file, kbuf, (size_t)nread);
        if (nwritten <= 0) {
            if (nwritten == -(s64)EPIPE) {
                sched_kill_process(proc->pid, 13 /* SIGPIPE */);
            }
            if (total_transferred == 0) err = (nwritten < 0) ? nwritten : -(s64)EIO;
            break;
        }

        total_transferred += (size_t)nwritten;
        if (nwritten < (s64)to_read) break;
    }

    if (use_off && user_offset) {
        if (copy_to_user(user_offset, &current_off, sizeof(s64)) != 0 && total_transferred == 0) {
            err = -(s64)EFAULT;
        }
    }

    fput(in_file);
    fput(out_file);
    return (total_transferred > 0) ? (s64)total_transferred : err;
}

s64 sys_copy_file_range_impl(pt_regs_t *r)
{
    int fd_in = (int)(s32)r->rdi;
    s64 *off_in = (s64 *)r->rsi;
    int fd_out = (int)(s32)r->rdx;
    s64 *off_out = (s64 *)r->r10;
    size_t len = (size_t)r->r8;
    unsigned int flags = (unsigned int)r->r9;

    if (flags != 0) return -(s64)EINVAL;
    if (len == 0) return 0;
    if (fd_in < 0 || fd_in >= PROC_MAX_FDS || fd_out < 0 || fd_out >= PROC_MAX_FDS) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EBADF;

    file_t *in_file = fget(proc, fd_in);
    if (!in_file) return -(s64)EBADF;

    file_t *out_file = fget(proc, fd_out);
    if (!out_file) {
        fput(in_file);
        return -(s64)EBADF;
    }

    if ((in_file->f_flags & 3) == O_WRONLY || (out_file->f_flags & 3) == O_RDONLY) {
        fput(in_file);
        fput(out_file);
        return -(s64)EBADF;
    }

    if (out_file->f_flags & O_APPEND) {
        fput(in_file);
        fput(out_file);
        return -(s64)EBADF;
    }

    s64 cur_in = 0, cur_out = 0;
    bool has_in = false, has_out = false;
    if (off_in) {
        if ((uintptr_t)off_in >= TASK_SIZE_MAX) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (copy_from_user(&cur_in, off_in, sizeof(s64)) != 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (cur_in < 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EINVAL;
        }
        has_in = true;
    }
    if (off_out) {
        if ((uintptr_t)off_out >= TASK_SIZE_MAX) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (copy_from_user(&cur_out, off_out, sizeof(s64)) != 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EFAULT;
        }
        if (cur_out < 0) {
            fput(in_file);
            fput(out_file);
            return -(s64)EINVAL;
        }
        has_out = true;
    }

    size_t total_copied = 0;
    char kbuf[4096];
    s64 err = 0;

    while (total_copied < len) {
        if (proc->sig_pending & ~proc->sig_blocked) {
            if (total_copied == 0) err = -(s64)EINTR;
            break;
        }

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

        if (nread <= 0) {
            if (nread < 0 && total_copied == 0) err = nread;
            break;
        }

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
            if (nwritten == -(s64)EPIPE) {
                sched_kill_process(proc->pid, 13 /* SIGPIPE */);
            }
            if (total_copied == 0) err = (nwritten < 0) ? nwritten : -(s64)EIO;
            break;
        }

        total_copied += (size_t)nwritten;
        if (nwritten < (s64)to_copy) break;
    }

    if (has_in && off_in) copy_to_user(off_in, &cur_in, sizeof(s64));
    if (has_out && off_out) copy_to_user(off_out, &cur_out, sizeof(s64));

    fput(in_file);
    fput(out_file);
    return (total_copied > 0) ? (s64)total_copied : err;
}

#define FALLOC_FL_KEEP_SIZE      0x01
#define FALLOC_FL_PUNCH_HOLE     0x02
#define FALLOC_FL_NO_HIDE_STALES 0x04
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_ZERO_RANGE     0x10
#define FALLOC_FL_INSERT_RANGE   0x20
#define FALLOC_FL_UNSHARE_RANGE  0x40

s64 sys_fallocate_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    int mode = (int)r->rsi;
    s64 offset = (s64)r->rdx;
    s64 len = (s64)r->r10;

    if (offset < 0 || len <= 0) return -(s64)EINVAL;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->handle_table[fd]) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    if (!file || !file->f_inode) return -(s64)EBADF;
    if ((file->f_flags & 3) == O_RDONLY) return -(s64)EBADF;  /* must be writable */

    s64 req_size = offset + len;
    if (req_size < offset) return -(s64)EINVAL;   /* range overflows */

    /* ext2 reserves real blocks here (so a later write cannot ENOSPC); other
     * filesystems fall back to extending i_size, which is what this call used
     * to do unconditionally. */
    s64 _ret = vfs_fallocate(file, mode, (u64)offset, (u64)len);
    fput(file);
    return _ret;
}

s64 sys_sync_file_range_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EBADF;
    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    s64 ret = 0;
    if (f->f_inode && f->f_inode->i_sb)
        ret = vfs_sync_fs(f->f_inode->i_sb);
    else
        vfs_sync_all();
    fput(f);
    return ret;
}

s64 sys_readahead_impl(pt_regs_t *r)
{
    int fd    = (int)(s32)r->rdi;
    u64 off   = r->rsi;
    u64 count = r->rdx;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EBADF;
    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    /* readahead(2) is posix_fadvise(POSIX_FADV_WILLNEED) with a fixed advice. */
    vfs_fadvise(f, off, count, 3 /* WILLNEED */);
    fput(f);
    return 0;
}

s64 sys_splice_impl(pt_regs_t *r)
{
    return sys_copy_file_range_impl(r);
}

s64 sys_tee_impl(pt_regs_t *r)
{
    return sys_copy_file_range_impl(r);
}

s64 sys_vmsplice_impl(pt_regs_t *r)
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

s64 sys_statx_impl(pt_regs_t *r)
{
    int dirfd = (int)(s32)r->rdi;
    const char *user_path = (const char *)r->rsi;
    int flags = (int)r->rdx;
    unsigned int mask = (unsigned int)r->r10;
    struct statx *statxbuf = (struct statx *)r->r8;
    (void)mask;

    if (!statxbuf) return -(s64)EINVAL;
    if ((uintptr_t)statxbuf >= TASK_SIZE_MAX) return -(s64)EFAULT;

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
        if (!proc) return -(s64)EBADF;
        file_t *file = fget(proc, dirfd);
        if (!file) return -(s64)EBADF;
        if (!file->f_inode) {
            fput(file);
            return -(s64)EBADF;
        }
        s64 ret = vfs_fstat(file, &kst);
        fput(file);
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

s64 sys_fadvise64_impl(pt_regs_t *r)
{
    int fd     = (int)(s32)r->rdi;
    u64 offset = r->rsi;
    u64 len    = r->rdx;
    int advice = (int)r->r10;

    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EBADF;

    switch (advice) {
        case 0: case 1: case 2: case 3: case 4: case 5:  /* POSIX_FADV_* */
            break;
        default:
            return -(s64)EINVAL;
    }

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 ret = vfs_fadvise(file, offset, len, advice);
    fput(file);
    return ret;
}

s64 sys_link_impl(pt_regs_t *r)
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

s64 sys_linkat_impl(pt_regs_t *r)
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

s64 sys_symlinkat_impl(pt_regs_t *r)
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

s64 sys_fchmodat_impl(pt_regs_t *r)
{
    int dfd = (int)(s32)r->rdi;
    const char *path = (const char *)r->rsi;
    u32 mode = (u32)r->rdx;

    char kpath[512];
    s64 err = copy_user_path_resolve_at(dfd, kpath, sizeof(kpath), path);
    if (err < 0) return err;
    return vfs_chmod(kpath, mode);
}

s64 sys_fchownat_impl(pt_regs_t *r)
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

s64 sys_renameat2_impl(pt_regs_t *r)
{
    int olddfd = (int)(s32)r->rdi;
    const char *oldpath = (const char *)r->rsi;
    int newdfd = (int)(s32)r->rdx;
    const char *newpath = (const char *)r->r10;
    unsigned int flags = (unsigned int)r->r8;

    char kold[512], knew[512];
    s64 err = copy_user_path_resolve_at(olddfd, kold, sizeof(kold), oldpath);
    if (err < 0) return err;
    err = copy_user_path_resolve_at(newdfd, knew, sizeof(knew), newpath);
    if (err < 0) return err;
    return vfs_rename_flags(kold, knew, flags);
}

s64 sys_renameat_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.r8 = 0;
    return sys_renameat2_impl(&sub);
}

s64 sys_flock_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    int operation = (int)r->rsi;
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS || !proc->handle_table[fd]) return -(s64)EBADF;
    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;
    s64 _ret = vfs_flock(file, operation);
    fput(file);
    return _ret;
}

s64 sys_fsync_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    s64 ret = 0;
    if (f->f_inode && f->f_inode->i_sb)
        ret = vfs_sync_fs(f->f_inode->i_sb);
    else
        vfs_sync_all();
    fput(f);
    return ret;
}

s64 sys_fdatasync_impl(pt_regs_t *r)
{
    return sys_fsync_impl(r);
}

s64 sys_sync_impl(pt_regs_t *r)
{
    (void)r;
    vfs_sync_all();
    return 0;
}

s64 sys_syncfs_impl(pt_regs_t *r)
{
    int fd = (int)(s32)r->rdi;
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    s64 ret = 0;
    if (f->f_inode && f->f_inode->i_sb)
        ret = vfs_sync_fs(f->f_inode->i_sb);
    else
        vfs_sync_all();
    fput(f);
    return ret;
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

s64 sys_eventfd2_impl(pt_regs_t *r)
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

s64 sys_eventfd_impl(pt_regs_t *r)
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

s64 sys_epoll_create1_impl(pt_regs_t *r)
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

s64 sys_epoll_create_impl(pt_regs_t *r)
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

s64 sys_epoll_ctl_impl(pt_regs_t *r)
{
    int epfd = (int)r->rdi;
    int op = (int)r->rsi;
    int fd = (int)r->rdx;
    const linux_epoll_event_t *event = (const linux_epoll_event_t *)r->r10;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (epfd == fd) return -(s64)EINVAL;

    file_t *epfile = fget(proc, epfd);
    if (!epfile) return -(s64)EBADF;
    if (epfile->f_op != &g_epoll_fops || !epfile->private_data) {
        fput(epfile);
        return -(s64)EINVAL;
    }
    epoll_ctx_t *ctx = (epoll_ctx_t *)epfile->private_data;

    file_t *target_file = fget(proc, fd);
    if (!target_file) {
        fput(epfile);
        return -(s64)EBADF;
    }
    fput(target_file);

    linux_epoll_event_t kevent;
    if (op != EPOLL_CTL_DEL) {
        if (!event || (uintptr_t)event >= TASK_SIZE_MAX) {
            fput(epfile);
            return -(s64)EFAULT;
        }
        if (copy_from_user(&kevent, event, sizeof(linux_epoll_event_t)) != 0) {
            fput(epfile);
            return -(s64)EFAULT;
        }
    }

    spinlock_lock(&ctx->lock);
    if (op == EPOLL_CTL_ADD) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                spinlock_unlock(&ctx->lock);
                fput(epfile);
                return -(s64)EEXIST;
            }
        }
        if (ctx->count >= EPOLL_MAX_ITEMS) {
            spinlock_unlock(&ctx->lock);
            fput(epfile);
            return -(s64)ENOSPC;
        }
        ctx->items[ctx->count].fd = fd;
        ctx->items[ctx->count].events = kevent.events;
        ctx->items[ctx->count].data = kevent.data;
        ctx->count++;
        spinlock_unlock(&ctx->lock);
        fput(epfile);
        return 0;
    } else if (op == EPOLL_CTL_MOD) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                ctx->items[i].events = kevent.events;
                ctx->items[i].data = kevent.data;
                spinlock_unlock(&ctx->lock);
                fput(epfile);
                return 0;
            }
        }
        spinlock_unlock(&ctx->lock);
        fput(epfile);
        return -(s64)ENOENT;
    } else if (op == EPOLL_CTL_DEL) {
        for (int i = 0; i < ctx->count; i++) {
            if (ctx->items[i].fd == fd) {
                ctx->items[i] = ctx->items[ctx->count - 1];
                ctx->count--;
                spinlock_unlock(&ctx->lock);
                fput(epfile);
                return 0;
            }
        }
        spinlock_unlock(&ctx->lock);
        fput(epfile);
        return -(s64)ENOENT;
    }
    spinlock_unlock(&ctx->lock);
    fput(epfile);
    return -(s64)EINVAL;
}

s64 sys_epoll_wait_impl(pt_regs_t *r)
{
    int epfd = (int)r->rdi;
    linux_epoll_event_t *events = (linux_epoll_event_t *)r->rsi;
    int maxevents = (int)r->rdx;
    int timeout_ms = (int)r->r10;

    if (maxevents <= 0 || maxevents > 1024) return -(s64)EINVAL;
    if (!events || (uintptr_t)events >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *epfile = fget(proc, epfd);
    if (!epfile) return -(s64)EBADF;
    if (epfile->f_op != &g_epoll_fops || !epfile->private_data) {
        fput(epfile);
        return -(s64)EINVAL;
    }
    epoll_ctx_t *ctx = (epoll_ctx_t *)epfile->private_data;

    linux_epoll_event_t *kevents = (linux_epoll_event_t *)kmalloc(sizeof(linux_epoll_event_t) * (size_t)maxevents);
    if (!kevents) {
        fput(epfile);
        return -(s64)ENOMEM;
    }

    u64 start_ticks = sched_get_ticks();
    u64 end_ticks = (timeout_ms > 0) ? (start_ticks + ((u64)timeout_ms + 9) / 10) : 0;

    for (;;) {
        int ready_count = 0;
        spinlock_lock(&ctx->lock);
        for (int i = 0; i < ctx->count && ready_count < maxevents; i++) {
            int tfd = ctx->items[i].fd;
            file_t *tf = fget(proc, tfd);
            if (!tf) continue;
            short rev = check_file_readiness(tf, (short)ctx->items[i].events);
            fput(tf);
            if (rev & ctx->items[i].events) {
                kevents[ready_count].events = (u32)(rev & ctx->items[i].events);
                kevents[ready_count].data = ctx->items[i].data;
                ready_count++;
            }
        }
        spinlock_unlock(&ctx->lock);

        if (ready_count > 0) {
            size_t not_copied = copy_to_user(events, kevents, sizeof(linux_epoll_event_t) * (size_t)ready_count);
            kfree(kevents);
            fput(epfile);
            if (not_copied != 0) return -(s64)EFAULT;
            return ready_count;
        }

        if (timeout_ms == 0) {
            kfree(kevents);
            fput(epfile);
            return 0;
        }
        if (timeout_ms > 0 && sched_get_ticks() >= end_ticks) {
            kfree(kevents);
            fput(epfile);
            return 0;
        }

        /* Interrupt on deliverable signals per POSIX/Linux */
        if (proc->sig_pending & ~proc->sig_blocked) {
            kfree(kevents);
            fput(epfile);
            return -(s64)EINTR;
        }

        sched_sleep(1);
    }
}

s64 sys_epoll_pwait_impl(pt_regs_t *r)
{
    const sigset_t *user_sigmask = (const sigset_t *)r->r10;
    size_t sigsetsize = (size_t)r->r8;
    
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    
    sigset_t old_mask = proc->sig_blocked;
    
    if (user_sigmask) {
        if (sigsetsize != sizeof(sigset_t)) return -(s64)EINVAL;
        if ((uintptr_t)user_sigmask >= TASK_SIZE_MAX) return -(s64)EFAULT;
        sigset_t kmask;
        if (copy_from_user(&kmask, user_sigmask, sizeof(sigset_t)) != 0) return -(s64)EFAULT;
        
        proc->sig_blocked = kmask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }
    
    s64 ret = sys_epoll_wait_impl(r);
    
    if (user_sigmask) {
        proc->sig_blocked = old_mask;
    }
    return ret;
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

s64 sys_timerfd_create_impl(pt_regs_t *r)
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


s64 sys_timerfd_settime_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int flags = (int)r->rsi;
    const struct itimerspec *new_value = (const struct itimerspec *)r->rdx;
    struct itimerspec *old_value = (struct itimerspec *)r->r10;

    (void)flags;
    if (!new_value || (uintptr_t)new_value >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (old_value && (uintptr_t)old_value >= TASK_SIZE_MAX) return -(s64)EFAULT;

    struct itimerspec new_val;
    if (copy_from_user(&new_val, new_value, sizeof(struct itimerspec)) != 0) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_timerfd_fops || !f->private_data) {
        fput(f);
        return -(s64)EINVAL;
    }
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)f->private_data;

    struct itimerspec old_val;
    memset(&old_val, 0, sizeof(old_val));

    spinlock_lock(&ctx->lock);
    if (old_value) {
        old_val.it_interval.tv_sec = (long)(ctx->interval_ms / 1000);
        old_val.it_interval.tv_nsec = (long)((ctx->interval_ms % 1000) * 1000000);
        if (ctx->expire_tick > 0) {
            u64 ticks = sched_get_ticks();
            if (ctx->expire_tick > ticks) {
                u64 rem_ms = (ctx->expire_tick - ticks) * 10;
                old_val.it_value.tv_sec = (long)(rem_ms / 1000);
                old_val.it_value.tv_nsec = (long)((rem_ms % 1000) * 1000000);
            }
        }
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

    fput(f);

    if (old_value) {
        copy_to_user(old_value, &old_val, sizeof(struct itimerspec));
    }
    return 0;
}

s64 sys_timerfd_gettime_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct itimerspec *curr_value = (struct itimerspec *)r->rsi;

    if (!curr_value || (uintptr_t)curr_value >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_timerfd_fops || !f->private_data) {
        fput(f);
        return -(s64)EINVAL;
    }
    timerfd_ctx_t *ctx = (timerfd_ctx_t *)f->private_data;

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
    fput(f);

    if (copy_to_user(curr_value, &val, sizeof(struct itimerspec)) != 0) return -(s64)EFAULT;
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

s64 sys_signalfd4_impl(pt_regs_t *r)
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
        if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;
        file_t *f = fget(proc, fd);
        if (!f) return -(s64)EBADF;
        if (f->f_op != &g_signalfd_fops || !f->private_data) {
            fput(f);
            return -(s64)EINVAL;
        }
        signalfd_ctx_t *ctx = (signalfd_ctx_t *)f->private_data;
        ctx->mask = smask;
        fput(f);
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

s64 sys_signalfd_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.r10 = 0;
    return sys_signalfd4_impl(&sub);
}

s64 sys_chroot_impl(pt_regs_t *r)
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

/* ── File handles ────────────────────────────────────────────────────────
 *
 * A file handle names an inode rather than a path, so it stays valid across
 * renames and can be stored and reopened later. ext2 encodes one as its
 * inode number plus the on-disk generation counter; tmpfs as its never-reused
 * inode number. Both are in their filesystem's export_operations.
 */
#define AT_SYMLINK_FOLLOW_FH  0x0400    /* AT_SYMLINK_FOLLOW */
#define AT_EMPTY_PATH_FH      0x1000    /* AT_EMPTY_PATH */

s64 sys_name_to_handle_at_impl(pt_regs_t *r)
{
    int dirfd                = (int)r->rdi;
    const char *upathname    = (const char *)r->rsi;
    struct file_handle *uh   = (struct file_handle *)r->rdx;
    int *umount_id           = (int *)r->r10;
    int flags                = (int)r->r8;

    if (!uh || (uintptr_t)uh >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (!umount_id || (uintptr_t)umount_id >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (flags & ~(AT_SYMLINK_FOLLOW_FH | AT_EMPTY_PATH_FH)) return -(s64)EINVAL;

    /* handle_bytes is both an input (how much room the caller has) and an
     * output (how much was needed), and it comes before the payload. */
    unsigned int avail = 0;
    if (copy_from_user(&avail, &uh->handle_bytes, sizeof(avail)) != 0) return -(s64)EFAULT;
    if (avail > MAX_HANDLE_SZ) return -(s64)EINVAL;

    char kpath[512];
    dentry_t *dentry = NULL;

    if ((flags & AT_EMPTY_PATH_FH) && (!upathname || !upathname[0])) {
        process_t *proc = sched_current_process();
        file_t *df = fget(proc, dirfd);
        if (!df) return -(s64)EBADF;
        dentry = df->f_dentry;
        if (!dentry || !dentry->d_inode) { fput(df); return -(s64)EBADF; }
        /* Hold the reference only long enough to read the inode out. */
        inode_t *inode = dentry->d_inode;
        s64 rc;
        {
            u32 fhbuf[MAX_HANDLE_SZ / sizeof(u32)];
            int fhlen = (int)(avail / sizeof(u32));
            export_operations_t *eops = inode->i_sb ? inode->i_sb->s_export_op : NULL;
            if (!eops || !eops->encode_fh) { fput(df); return -(s64)EOPNOTSUPP; }
            rc = eops->encode_fh(inode, fhbuf, &fhlen, NULL);
            if (rc == -(s64)EOVERFLOW) {
                unsigned int need = (unsigned int)fhlen * sizeof(u32);
                copy_to_user(&uh->handle_bytes, &need, sizeof(need));
                fput(df);
                return -(s64)EOVERFLOW;
            }
            if (rc < 0) { fput(df); return rc; }

            unsigned int used = (unsigned int)fhlen * sizeof(u32);
            int htype = (int)rc;
            vfsmount_t *m = mnt_find_for_sb(inode->i_sb);
            int mid = m ? (int)m->mnt_id : 0;
            if (copy_to_user(&uh->handle_bytes, &used, sizeof(used)) != 0 ||
                copy_to_user(&uh->handle_type, &htype, sizeof(htype)) != 0 ||
                copy_to_user(uh->f_handle, fhbuf, used) != 0 ||
                copy_to_user(umount_id, &mid, sizeof(mid)) != 0) {
                fput(df);
                return -(s64)EFAULT;
            }
        }
        fput(df);
        return 0;
    }

    s64 perr = copy_user_path_resolve_at(dirfd, kpath, sizeof(kpath), upathname);
    if (perr < 0) return perr;

    s64 lerr = (flags & AT_SYMLINK_FOLLOW_FH) ? vfs_path_lookup(kpath, &dentry)
                                              : vfs_path_lookup_nofollow(kpath, &dentry);
    if (lerr < 0 || !dentry || !dentry->d_inode) return -(s64)ENOENT;

    inode_t *inode = dentry->d_inode;
    export_operations_t *eops = inode->i_sb ? inode->i_sb->s_export_op : NULL;
    if (!eops || !eops->encode_fh) return -(s64)EOPNOTSUPP;

    u32 fhbuf[MAX_HANDLE_SZ / sizeof(u32)];
    int fhlen = (int)(avail / sizeof(u32));
    s64 rc = eops->encode_fh(inode, fhbuf, &fhlen, NULL);
    if (rc == -(s64)EOVERFLOW) {
        unsigned int need = (unsigned int)fhlen * sizeof(u32);
        if (copy_to_user(&uh->handle_bytes, &need, sizeof(need)) != 0) return -(s64)EFAULT;
        return -(s64)EOVERFLOW;
    }
    if (rc < 0) return rc;

    unsigned int used = (unsigned int)fhlen * sizeof(u32);
    int htype = (int)rc;
    vfsmount_t *m = mnt_find_for_sb(inode->i_sb);
    int mid = m ? (int)m->mnt_id : 0;

    if (copy_to_user(&uh->handle_bytes, &used, sizeof(used)) != 0 ||
        copy_to_user(&uh->handle_type, &htype, sizeof(htype)) != 0 ||
        copy_to_user(uh->f_handle, fhbuf, used) != 0 ||
        copy_to_user(umount_id, &mid, sizeof(mid)) != 0)
        return -(s64)EFAULT;
    return 0;
}

s64 sys_open_by_handle_at_impl(pt_regs_t *r)
{
    int mount_fd           = (int)r->rdi;
    struct file_handle *uh = (struct file_handle *)r->rsi;
    int flags              = (int)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* Opening by handle bypasses every path-based permission check on the
     * way to the inode, which is why Linux gates it on this capability. */
    if (!security_check_permission(proc, CAP_DAC_READ_SEARCH)) return -(s64)EPERM;
    if (!uh || (uintptr_t)uh >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* Copy the whole handle into the kernel: f_handle is variable-length and
     * must not be re-read from userspace after handle_bytes is validated. */
    u8 kh[sizeof(struct file_handle) + MAX_HANDLE_SZ];
    struct file_handle *h = (struct file_handle *)kh;
    if (copy_from_user(h, uh, sizeof(struct file_handle)) != 0) return -(s64)EFAULT;
    if (h->handle_bytes == 0 || h->handle_bytes > MAX_HANDLE_SZ) return -(s64)EINVAL;
    if (copy_from_user(h->f_handle, uh->f_handle, h->handle_bytes) != 0) return -(s64)EFAULT;

    file_t *mf = fget(proc, mount_fd);
    if (!mf) return -(s64)EBADF;
    super_block_t *sb = mf->f_inode ? mf->f_inode->i_sb : NULL;
    fput(mf);
    if (!sb || !sb->s_export_op || !sb->s_export_op->fh_to_dentry) return -(s64)ESTALE;

    dentry_t *dentry = sb->s_export_op->fh_to_dentry(sb, h);
    if (!dentry || !dentry->d_inode) return -(s64)ESTALE;

    inode_t *inode = dentry->d_inode;
    if ((flags & 3) != O_RDONLY && inode_is_rdonly(inode)) return -(s64)EROFS;
    if (S_ISDIR(inode->i_mode) && ((flags & 3) == O_WRONLY || (flags & 3) == O_RDWR))
        return -(s64)EISDIR;

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) return -(s64)ENOMEM;
    f->f_dentry = dentry;
    f->f_inode  = inode;
    f->f_op     = inode->i_fop;
    f->f_flags  = (u32)flags;
    f->f_pos    = 0;
    f->f_count  = 1;
    f->private_data = inode->i_private;
    if (f->f_op && f->f_op->open && f->f_op->open(inode, f) < 0) {
        kfree(f);
        return -(s64)EIO;
    }

    s64 fd = syscall_install_fd(proc, f, (flags & O_CLOEXEC) ? FD_CLOEXEC : 0);
    if (fd < 0) { vfs_close(f); return -(s64)EMFILE; }
    return fd;
}

/* ── Linux asynchronous I/O ─────────────────────────────────────────────── */

s64 sys_io_setup_impl(pt_regs_t *r)
{
    unsigned int nr_events = (unsigned int)r->rdi;
    u64 *uctxp = (u64 *)r->rsi;

    if (!uctxp || (uintptr_t)uctxp >= TASK_SIZE_MAX) return -(s64)EFAULT;

    /* io_setup(2) requires the context word to start out zero — a non-zero
     * one usually means the caller is reusing a variable that still holds a
     * live context, and silently overwriting it would leak that context. */
    u64 existing = 0;
    if (copy_from_user(&existing, uctxp, sizeof(existing)) != 0) return -(s64)EFAULT;
    if (existing != 0) return -(s64)EINVAL;

    u64 id = 0;
    s64 err = aio_setup(nr_events, &id);
    if (err < 0) return err;

    if (copy_to_user(uctxp, &id, sizeof(id)) != 0) {
        aio_destroy(id);
        return -(s64)EFAULT;
    }
    return 0;
}

s64 sys_io_destroy_impl(pt_regs_t *r)
{
    return aio_destroy((u64)r->rdi);
}

s64 sys_io_submit_impl(pt_regs_t *r)
{
    u64 ctx_id  = (u64)r->rdi;
    long nr     = (long)r->rsi;
    u64 *uiocbs = (u64 *)r->rdx;

    if (nr < 0) return -(s64)EINVAL;
    if (nr == 0) return 0;
    if (!uiocbs || (uintptr_t)uiocbs >= TASK_SIZE_MAX) return -(s64)EFAULT;
    return aio_submit(ctx_id, nr, uiocbs);
}

s64 sys_io_getevents_impl(pt_regs_t *r)
{
    u64 ctx_id   = (u64)r->rdi;
    long min_nr  = (long)r->rsi;
    long nr      = (long)r->rdx;
    struct io_event *uev = (struct io_event *)r->r10;
    const void *utimeout = (const void *)r->r8;

    if (nr > 0 && (!uev || (uintptr_t)uev >= TASK_SIZE_MAX)) return -(s64)EFAULT;

    u64 timeout_ns = 0;
    bool have_timeout = false;
    if (utimeout) {
        if ((uintptr_t)utimeout >= TASK_SIZE_MAX) return -(s64)EFAULT;
        struct { s64 tv_sec; s64 tv_nsec; } ts;
        if (copy_from_user(&ts, utimeout, sizeof(ts)) != 0) return -(s64)EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) return -(s64)EINVAL;
        timeout_ns = (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
        have_timeout = true;
    }
    return aio_getevents(ctx_id, min_nr, nr, uev, timeout_ns, have_timeout);
}

s64 sys_io_cancel_impl(pt_regs_t *r)
{
    u64 ctx_id = (u64)r->rdi;
    u64 uiocb  = (u64)r->rsi;
    struct io_event *uresult = (struct io_event *)r->rdx;

    if (!uiocb || uiocb >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (uresult && (uintptr_t)uresult >= TASK_SIZE_MAX) return -(s64)EFAULT;
    return aio_cancel(ctx_id, uiocb, uresult);
}

/* ── fanotify(7) ──────────────────────────────────────────────────────────
 *
 * fs/fanotify.c held a complete-looking implementation that nothing could
 * call: neither of these two numbers was ever registered, so every program
 * that tried to use fanotify got -ENOSYS and fell back (or failed). These
 * are the two entry points that make that file live.
 */
s64 sys_fanotify_init_impl(pt_regs_t *r)
{
    unsigned int flags         = (unsigned int)r->rdi;
    unsigned int event_f_flags = (unsigned int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    /* Watching arbitrary filesystem activity is a privileged operation in
     * Linux for the obvious reason, and programs check for EPERM here. */
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;

    file_t *filp = NULL;
    extern int fanotify_create(unsigned int flags, unsigned int event_f_flags, file_t **out_file);
    int err = fanotify_create(flags, event_f_flags, &filp);
    if (err < 0) return (s64)err;

    filp->f_fd_flags = (flags & 0x00000001 /* FAN_CLOEXEC */) ? FD_CLOEXEC : 0;
    filp->f_mode = 0600;

    s64 fd = syscall_install_fd(proc, filp, filp->f_fd_flags);
    if (fd < 0) {
        vfs_close(filp);
        return -(s64)EMFILE;
    }
    return fd;
}

s64 sys_fanotify_mark_impl(pt_regs_t *r)
{
    int fd                 = (int)r->rdi;
    unsigned int flags     = (unsigned int)r->rsi;
    u64 mask               = (u64)r->rdx;
    int dirfd              = (int)r->r10;
    const char *upathname  = (const char *)r->r8;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *filp = fget(proc, fd);
    if (!filp) return -(s64)EBADF;

    char path[256];
    path[0] = '\0';
    if (upathname) {
        if (copy_str_from_user(path, upathname, sizeof(path)) < 0) {
            fput(filp);
            return -(s64)EFAULT;
        }
    }

    extern int fanotify_add_mark(file_t *filp, unsigned int flags, u64 mask,
                                 int dirfd, const char *pathname);
    s64 ret = (s64)fanotify_add_mark(filp, flags, mask, dirfd,
                                     upathname ? path : NULL);
    fput(filp);
    return ret;
}

s64 sys_inotify_init1_impl(pt_regs_t *r)
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

s64 sys_inotify_init_impl(pt_regs_t *r)
{
    pt_regs_t sub = *r;
    sub.rdi = 0;
    return sys_inotify_init1_impl(&sub);
}

s64 sys_inotify_add_watch_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *user_path = (const char *)r->rsi;
    u32 mask = (u32)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_inotify_fops || !f->private_data) {
        fput(f);
        return -(s64)EINVAL;
    }

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), user_path);
    if (perr < 0) {
        fput(f);
        return perr;
    }

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(kpath, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        fput(f);
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
            fput(f);
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
            fput(f);
            return assigned_wd;
        }
    }

    spinlock_unlock(&ctx->lock);
    fput(f);
    return -(s64)ENOSPC;
}

s64 sys_inotify_rm_watch_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int wd = (int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *f = fget(proc, fd);
    if (!f) return -(s64)EBADF;
    if (f->f_op != &g_inotify_fops || !f->private_data) {
        fput(f);
        return -(s64)EINVAL;
    }

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
            fput(f);
            return 0;
        }
    }

    spinlock_unlock(&ctx->lock);
    fput(f);
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

s64 sys_setxattr_impl(pt_regs_t *r)
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

s64 sys_lsetxattr_impl(pt_regs_t *r)
{
    return sys_setxattr_impl(r);
}

s64 sys_fsetxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;
    const void *uval = (const void *)r->rdx;
    size_t size = (size_t)r->r10;
    int flags = (int)r->r8;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char kpath[64];
    if (file->f_inode) {
        snprintf(kpath, sizeof(kpath), "ino:%llu", (unsigned long long)file->f_inode->i_ino);
    } else {
        snprintf(kpath, sizeof(kpath), "file:%p", (void *)file);
    }

    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) {
        fput(file);
        return -(s64)EFAULT;
    }
    kname[sizeof(kname) - 1] = '\0';

    s64 ret = do_setxattr(kpath, kname, uval, size, flags);
    fput(file);
    return ret;
}

s64 sys_getxattr_impl(pt_regs_t *r)
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

s64 sys_lgetxattr_impl(pt_regs_t *r)
{
    return sys_getxattr_impl(r);
}

s64 sys_fgetxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;
    void *uval = (void *)r->rdx;
    size_t size = (size_t)r->r10;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char kpath[64];
    if (file->f_inode) {
        snprintf(kpath, sizeof(kpath), "ino:%llu", (unsigned long long)file->f_inode->i_ino);
    } else {
        snprintf(kpath, sizeof(kpath), "file:%p", (void *)file);
    }

    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) {
        fput(file);
        return -(s64)EFAULT;
    }
    kname[sizeof(kname) - 1] = '\0';

    s64 ret = do_getxattr(kpath, kname, uval, size);
    fput(file);
    return ret;
}

s64 sys_listxattr_impl(pt_regs_t *r)
{
    const char *upath = (const char *)r->rdi;
    char *ulist = (char *)r->rsi;
    size_t size = (size_t)r->rdx;

    char kpath[256];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), upath);
    if (perr < 0) return perr;

    return do_listxattr(kpath, ulist, size);
}

s64 sys_llistxattr_impl(pt_regs_t *r)
{
    return sys_listxattr_impl(r);
}

s64 sys_flistxattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    char *ulist = (char *)r->rsi;
    size_t size = (size_t)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char kpath[64];
    if (file->f_inode) {
        snprintf(kpath, sizeof(kpath), "ino:%llu", (unsigned long long)file->f_inode->i_ino);
    } else {
        snprintf(kpath, sizeof(kpath), "file:%p", (void *)file);
    }

    s64 ret = do_listxattr(kpath, ulist, size);
    fput(file);
    return ret;
}

s64 sys_removexattr_impl(pt_regs_t *r)
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

s64 sys_lremovexattr_impl(pt_regs_t *r)
{
    return sys_removexattr_impl(r);
}

s64 sys_fremovexattr_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const char *uname = (const char *)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return -(s64)EBADF;

    file_t *file = fget(proc, fd);
    if (!file) return -(s64)EBADF;

    char kpath[64];
    if (file->f_inode) {
        snprintf(kpath, sizeof(kpath), "ino:%llu", (unsigned long long)file->f_inode->i_ino);
    } else {
        snprintf(kpath, sizeof(kpath), "file:%p", (void *)file);
    }

    char kname[64];
    if (copy_from_user(kname, uname, sizeof(kname) - 1) != 0) {
        fput(file);
        return -(s64)EFAULT;
    }
    kname[sizeof(kname) - 1] = '\0';

    s64 ret = do_removexattr(kpath, kname);
    fput(file);
    return ret;
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
s64 sys_creat_impl(pt_regs_t *r)
{
    pt_regs_t s = *r;
    s.rdx = r->rsi;                                    /* mode  */
    s.rsi = (u64)(O_CREAT | O_WRONLY | O_TRUNC);       /* flags */
    return sys_open_impl(&s);
}

/* lchown(path, uid, gid) — chown without following a trailing symlink. */
s64 sys_lchown_impl(pt_regs_t *r)
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
    if ((uintptr_t)iov >= TASK_SIZE_MAX) return -(s64)EFAULT;

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
s64 sys_preadv_impl(pt_regs_t *r)  { return sys_p_rw_v(r, false); }
s64 sys_pwritev_impl(pt_regs_t *r) { return sys_p_rw_v(r, true);  }

/* mknod/mknodat — only regular files are creatable this way. Named FIFOs are
 * not implemented (anonymous pipe(2) only); char/block device nodes live in
 * devfs and are not created from userspace. */
static s64 do_mknod(const char *path, u32 mode, u64 dev)
{
    u32 fmt = mode & S_IFMT;
    if (fmt == 0) fmt = S_IFREG;

    /* Creating a device node hands its holder whatever the driver behind
     * that major/minor can do, so it is privileged — which is why a
     * container's /dev is populated by its supervisor and not from inside.
     * Every other node type is unprivileged. */
    if (fmt == S_IFCHR || fmt == S_IFBLK) {
        process_t *proc = sched_current_process();
        if (!proc) return -(s64)EPERM;
        if (!security_check_permission(proc, CAP_MKNOD)) return -(s64)EPERM;
    }

    return vfs_mknod(path, mode, dev);
}

s64 sys_mknod_impl(pt_regs_t *r)
{
    char kpath[512];
    s64 perr = copy_user_path_resolve(kpath, sizeof(kpath), (const char *)r->rdi);
    if (perr < 0) return perr;
    return do_mknod(kpath, (u32)r->rsi, (u64)r->rdx);
}
s64 sys_mknodat_impl(pt_regs_t *r)
{
    /* Honour absolute paths and AT_FDCWD; dirfd-relative paths are resolved by
     * copy_user_path_resolve() against the cwd like the other *at() stubs. */
    char kpath[512];
    s64 perr = copy_user_path_resolve_at((int)r->rdi, kpath, sizeof(kpath),
                                         (const char *)r->rsi);
    if (perr < 0) return perr;
    return do_mknod(kpath, (u32)r->rdx, (u64)r->r10);
}

/* futimesat(dirfd, path, times) — obsolete, superseded by utimensat(2), but
 * still just a real dirfd-relative timestamp set with `struct timeval`
 * (microsecond, not nanosecond) precision. Was a pure no-op before. */
s64 sys_futimesat_impl(pt_regs_t *r)
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

/* ============================================================================
 * mount(2)
 * ========================================================================= */

s64 sys_mount_impl(pt_regs_t *r)
{
    const char *usource   = (const char *)r->rdi;
    const char *utarget   = (const char *)r->rsi;
    const char *ufstype   = (const char *)r->rdx;
    unsigned long mflags  = (unsigned long)r->r10;
    const char *udata     = (const char *)r->r8;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!utarget) return -(s64)EFAULT;

    /* MS_BIND / MS_MOVE / MS_REMOUNT carry no filesystem type — util-linux
     * passes NULL there — so only the plain form requires one. */
    bool needs_type = !(mflags & (MS_BIND | MS_MOVE | MS_REMOUNT));
    if (needs_type && !ufstype) return -(s64)EFAULT;

    char source[128] = {0}, target[256] = {0}, fstype[32] = {0}, data[256] = {0};
    if (usource && copy_str_from_user(source, usource, sizeof(source)) < 0) return -(s64)EFAULT;
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return -(s64)EFAULT;
    if (ufstype && copy_str_from_user(fstype, ufstype, sizeof(fstype)) < 0) return -(s64)EFAULT;
    /* `data` is filesystem-specific and only conventionally a string. Every
     * filesystem here parses it as one, and a non-string caller passes a
     * pointer we must not dereference blindly, so a failed copy is simply
     * "no options" rather than EFAULT. */
    if (udata && (uintptr_t)udata < TASK_SIZE_MAX)
        (void)copy_str_from_user(data, udata, sizeof(data));

    return vfs_mount_flags(source[0] ? source : fstype, target,
                           fstype[0] ? fstype : NULL, mflags,
                           data[0] ? data : NULL);
}

/* ============================================================================
 * umount2(2)
 * ========================================================================= */

s64 sys_umount2_impl(pt_regs_t *r)
{
    const char *utarget = (const char *)r->rdi;
    int flags = (int)r->rsi;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;
    if (!security_check_permission(proc, CAP_SYS_ADMIN)) return -(s64)EPERM;
    if (!utarget || (uintptr_t)utarget >= TASK_SIZE_MAX) return -(s64)EFAULT;

    char target[256] = {0};
    if (copy_str_from_user(target, utarget, sizeof(target)) < 0) return -(s64)EFAULT;

    return vfs_umount(target, flags);
}

/* ============================================================================
 * fchmodat2(2) — fchmodat() with the flags argument it should always have had.
 * ========================================================================= */

s64 sys_fchmodat2_impl(pt_regs_t *r)
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
 * vhangup(2) — revoke the controlling terminal of every process on it.
 * ========================================================================= */

s64 sys_vhangup_impl(pt_regs_t *r)
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

s64 sys_pivot_root_impl(pt_regs_t *r)
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

    if (strcmp(newroot, putold) == 0) return -(s64)EINVAL;

    /* Perform pivot_root for calling process:
     * new_root becomes the new root directory of the process.
     * cwd is reset to "/" relative to the new root. */
    strncpy(proc->root, newroot, sizeof(proc->root) - 1);
    proc->root[sizeof(proc->root) - 1] = '\0';
    strcpy(proc->cwd, "/");
    return 0;
}

s64 sys_io_pgetevents_impl(pt_regs_t *r)
{
    (void)r;
    /* Aio pgetevents: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_io_uring_setup_impl(pt_regs_t *r)
{
    (void)r;
    /* io_uring_setup: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_io_uring_enter_impl(pt_regs_t *r)
{
    (void)r;
    /* io_uring_enter: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_io_uring_register_impl(pt_regs_t *r)
{
    (void)r;
    /* io_uring_register: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_open_tree_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: open_tree: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_move_mount_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: move_mount: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_fsopen_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: fsopen: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_fsconfig_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: fsconfig: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_fsmount_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: fsmount: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_fspick_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: fspick: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_mount_setattr_impl(pt_regs_t *r)
{
    (void)r;
    /* New Mount API: mount_setattr: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_statmount_impl(pt_regs_t *r)
{
    (void)r;
    /* statmount: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}

s64 sys_listmount_impl(pt_regs_t *r)
{
    (void)r;
    /* listmount: Currently unsupported, returning ENOSYS. Programs will fall back or handle it. */
    return -(s64)ENOSYS;
}
