/* ============================================================================
 * AzamiOS Userspace — Syscall & POSIX Implementation
 * File: userland/libc/syscall.c
 * ============================================================================ */

#include "include/sys/syscall.h"
#include "include/sys/sysinfo.h"
#include "include/sys/utsname.h"
#include "include/sys/wait.h"
#include "include/sys/time.h"
#include "include/time.h"
#include "include/sys/stat.h"
#include "include/sys/statfs.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/signal.h"
#include "include/string.h"
#include "include/stdlib.h"
#include "include/errno.h"
#include "include/sys/times.h"
#include "include/sys/statvfs.h"
#include "include/sys/timeb.h"
#include "include/shadow.h"

int errno = 0;

static inline long __syscall_ret(long r)
{
    if (r < 0) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

/* ── Standard I/O syscall wrappers ───────────────────────────────────────── */

ssize_t sys_read(int fd, void *buf, size_t count)
{
    return (ssize_t)__syscall_ret(syscall3(SYS_read, fd, (long)buf, (long)count));
}

ssize_t sys_write(int fd, const void *buf, size_t count)
{
    return (ssize_t)__syscall_ret(syscall3(SYS_write, fd, (long)buf, (long)count));
}

int sys_open(const char *path, int flags, int mode)
{
    return (int)__syscall_ret(syscall3(SYS_open, (long)path, flags, mode));
}

int sys_close(int fd)
{
    return (int)__syscall_ret(syscall1(SYS_close, fd));
}

ssize_t read(int fd, void *buf, size_t count)
{
    return sys_read(fd, buf, count);
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return sys_write(fd, buf, count);
}

int close(int fd)
{
    return sys_close(fd);
}

int open(const char *path, int flags, ...)
{
    int mode = 0644;
    if (flags & O_CREAT) {
        __builtin_va_list vl;
        __builtin_va_start(vl, flags);
        mode = __builtin_va_arg(vl, int);
        __builtin_va_end(vl);
    }
    return sys_open(path, flags, mode);
}

int creat(const char *path, mode_t mode)
{
    return sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, (int)mode);
}

/* ── Process Lifecycle ───────────────────────────────────────────────────── */

int sys_fork(void)
{
    return (int)__syscall_ret(syscall0(SYS_fork));
}

int fork(void)
{
    return sys_fork();
}

extern char **environ;

int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)__syscall_ret(syscall3(SYS_execve, (long)path, (long)argv, (long)envp));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return sys_execve(path, argv, envp ? envp : environ);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    if (!file || !file[0]) return -1;
    if (strchr(file, '/')) {
        return execve(file, argv, envp);
    }

    const char *path_env = NULL;
    if (envp) {
        for (int i = 0; envp[i]; i++) {
            if (strncmp(envp[i], "PATH=", 5) == 0) {
                path_env = envp[i] + 5;
                break;
            }
        }
    }
    if (!path_env) path_env = "/bin:/sbin:/usr/bin:/usr/sbin:/";

    char path_buf[512];
    const char *cur = path_env;
    while (*cur) {
        const char *next = strchr(cur, ':');
        size_t dlen = next ? (size_t)(next - cur) : strlen(cur);
        if (dlen > 0 && dlen < sizeof(path_buf) - strlen(file) - 6) {
            strncpy(path_buf, cur, dlen);
            path_buf[dlen] = '\0';
            if (path_buf[dlen - 1] != '/') strcat(path_buf, "/");
            strcat(path_buf, file);

            execve(path_buf, argv, envp);

            /* Also try with .elf if extensionless */
            if (!strrchr(file, '.')) {
                strcat(path_buf, ".elf");
                execve(path_buf, argv, envp);
            }
        }
        if (!next) break;
        cur = next + 1;
    }

    return execve(file, argv, envp);
}

int execvp(const char *file, char *const argv[])
{
    return execvpe(file, argv, environ);
}

int execl(const char *path, const char *arg0, ...)
{
    char *argv[64];
    int argc = 0;
    argv[argc++] = (char *)arg0;

    __builtin_va_list ap;
    __builtin_va_start(ap, arg0);
    while (argc < 63) {
        char *arg = __builtin_va_arg(ap, char *);
        if (!arg) break;
        argv[argc++] = arg;
    }
    __builtin_va_end(ap);
    argv[argc] = NULL;

    return execv(path, argv);
}

int execlp(const char *file, const char *arg0, ...)
{
    char *argv[64];
    int argc = 0;
    argv[argc++] = (char *)arg0;

    __builtin_va_list ap;
    __builtin_va_start(ap, arg0);
    while (argc < 63) {
        char *arg = __builtin_va_arg(ap, char *);
        if (!arg) break;
        argv[argc++] = arg;
    }
    __builtin_va_end(ap);
    argv[argc] = NULL;

    return execvp(file, argv);
}

int execle(const char *path, const char *arg0, ...)
{
    char *argv[64];
    int argc = 0;
    argv[argc++] = (char *)arg0;

    __builtin_va_list ap;
    __builtin_va_start(ap, arg0);
    while (argc < 63) {
        char *arg = __builtin_va_arg(ap, char *);
        if (!arg) break;
        argv[argc++] = arg;
    }
    char *const *envp = __builtin_va_arg(ap, char *const *);
    __builtin_va_end(ap);
    argv[argc] = NULL;

    return execve(path, argv, envp);
}

void sys_exit(int status)
{
    syscall1(SYS_exit, status);
    for (;;) { __asm__ volatile("pause"); }
}

void _exit(int status)
{
    sys_exit(status);
    for (;;) { __asm__ volatile("pause"); }
}


int sys_getpid(void)
{
    return (int)syscall0(SYS_getpid);
}

int getpid(void)
{
    return sys_getpid();
}

int sys_getppid(void)
{
    return (int)syscall0(SYS_getppid);
}

int getppid(void)
{
    return sys_getppid();
}

int sys_getpgrp(void)
{
    return (int)syscall0(SYS_getpgrp);
}

int getpgrp(void)
{
    return sys_getpgrp();
}

uid_t sys_getuid(void)
{
    return (uid_t)syscall0(SYS_getuid);
}

uid_t getuid(void)
{
    return sys_getuid();
}

uid_t sys_geteuid(void)
{
    return (uid_t)syscall0(SYS_geteuid);
}

uid_t geteuid(void)
{
    return sys_geteuid();
}

gid_t sys_getgid(void)
{
    return (gid_t)syscall0(SYS_getgid);
}

gid_t getgid(void)
{
    return sys_getgid();
}

gid_t sys_getegid(void)
{
    return (gid_t)syscall0(SYS_getegid);
}

gid_t getegid(void)
{
    return sys_getegid();
}

int sys_setpgid(int pid, int pgid)
{
    return (int)__syscall_ret(syscall2(SYS_setpgid, pid, pgid));
}

int setpgid(int pid, int pgid)
{
    return sys_setpgid(pid, pgid);
}

int sys_setsid(void)
{
    return (int)__syscall_ret(syscall0(SYS_setsid));
}

int setsid(void)
{
    return sys_setsid();
}

/* ── File Operations & Metadata ─────────────────────────────────────────── */

ssize_t sys_lseek(int fd, ssize_t offset, int whence)
{
    return (ssize_t)__syscall_ret(syscall3(SYS_lseek, fd, offset, whence));
}

ssize_t lseek(int fd, ssize_t offset, int whence)
{
    return sys_lseek(fd, offset, whence);
}

int sys_stat(const char *path, struct stat *statbuf)
{
    return (int)__syscall_ret(syscall2(SYS_stat, (long)path, (long)statbuf));
}

int stat(const char *path, struct stat *statbuf)
{
    return sys_stat(path, statbuf);
}

int sys_lstat(const char *path, struct stat *statbuf)
{
    return (int)__syscall_ret(syscall2(SYS_lstat, (long)path, (long)statbuf));
}

int lstat(const char *path, struct stat *statbuf)
{
    return sys_lstat(path, statbuf);
}

int sys_fstat(int fd, struct stat *statbuf)
{
    return (int)__syscall_ret(syscall2(SYS_fstat, fd, (long)statbuf));
}

int fstat(int fd, struct stat *statbuf)
{
    return sys_fstat(fd, statbuf);
}

int sys_statfs(const char *path, struct statfs *buf)
{
    return (int)__syscall_ret(syscall2(SYS_statfs, (long)path, (long)buf));
}

int statfs(const char *path, struct statfs *buf)
{
    return sys_statfs(path, buf);
}

int sys_fstatfs(int fd, struct statfs *buf)
{
    return (int)__syscall_ret(syscall2(SYS_fstatfs, fd, (long)buf));
}

int fstatfs(int fd, struct statfs *buf)
{
    return sys_fstatfs(fd, buf);
}

int sys_chmod(const char *path, mode_t mode)
{
    return (int)__syscall_ret(syscall2(SYS_chmod, (long)path, mode));
}

int chmod(const char *path, mode_t mode)
{
    return sys_chmod(path, mode);
}

int sys_fchmod(int fd, mode_t mode)
{
    return (int)__syscall_ret(syscall2(SYS_fchmod, fd, mode));
}

int fchmod(int fd, mode_t mode)
{
    return sys_fchmod(fd, mode);
}

int sys_chown(const char *path, uint32_t uid, uint32_t gid)
{
    return (int)__syscall_ret(syscall3(SYS_chown, (long)path, uid, gid));
}

int chown(const char *path, uint32_t uid, uint32_t gid)
{
    return sys_chown(path, uid, gid);
}

int sys_fchown(int fd, uint32_t uid, uint32_t gid)
{
    return (int)__syscall_ret(syscall3(SYS_fchown, fd, uid, gid));
}

int fchown(int fd, uint32_t uid, uint32_t gid)
{
    return sys_fchown(fd, uid, gid);
}

mode_t sys_umask(mode_t mask)
{
    return (mode_t)syscall1(SYS_umask, mask);
}

mode_t umask(mode_t mask)
{
    return sys_umask(mask);
}

int sys_link(const char *oldpath, const char *newpath)
{
    return (int)__syscall_ret(syscall2(SYS_link, (long)oldpath, (long)newpath));
}

int link(const char *oldpath, const char *newpath)
{
    return sys_link(oldpath, newpath);
}

int sys_symlink(const char *target, const char *linkpath)
{
    return (int)__syscall_ret(syscall2(SYS_symlink, (long)target, (long)linkpath));
}

int symlink(const char *target, const char *linkpath)
{
    return sys_symlink(target, linkpath);
}

ssize_t sys_readlink(const char *path, char *buf, size_t bufsiz)
{
    return (ssize_t)__syscall_ret(syscall3(SYS_readlink, (long)path, (long)buf, (long)bufsiz));
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    return sys_readlink(path, buf, bufsiz);
}

/* ── Descriptors, Pipes & Multiplexing ───────────────────────────────────── */

int sys_pipe(int pipefd[2])
{
    return (int)__syscall_ret(syscall1(SYS_pipe, (long)pipefd));
}

int pipe(int pipefd[2])
{
    return sys_pipe(pipefd);
}

int sys_pipe2(int pipefd[2], int flags)
{
    return (int)__syscall_ret(syscall2(SYS_pipe2, (long)pipefd, flags));
}

int pipe2(int pipefd[2], int flags)
{
    return sys_pipe2(pipefd, flags);
}

int sys_dup(int oldfd)
{
    return (int)__syscall_ret(syscall1(SYS_dup, oldfd));
}

int dup(int oldfd)
{
    return sys_dup(oldfd);
}

int sys_dup2(int oldfd, int newfd)
{
    return (int)__syscall_ret(syscall2(SYS_dup2, oldfd, newfd));
}

int dup2(int oldfd, int newfd)
{
    return sys_dup2(oldfd, newfd);
}

int sys_dup3(int oldfd, int newfd, int flags)
{
    return (int)__syscall_ret(syscall3(SYS_dup3, oldfd, newfd, flags));
}

int dup3(int oldfd, int newfd, int flags)
{
    return sys_dup3(oldfd, newfd, flags);
}

int sys_fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    __builtin_va_list vl;
    __builtin_va_start(vl, cmd);
    arg = __builtin_va_arg(vl, unsigned long);
    __builtin_va_end(vl);
    return (int)__syscall_ret(syscall3(SYS_fcntl, fd, cmd, arg));
}

int fcntl(int fd, int cmd, ...)
{
    unsigned long arg = 0;
    __builtin_va_list vl;
    __builtin_va_start(vl, cmd);
    arg = __builtin_va_arg(vl, unsigned long);
    __builtin_va_end(vl);
    return sys_fcntl(fd, cmd, arg);
}

int sys_ioctl(int fd, unsigned long request, ...)
{
    unsigned long arg = 0;
    __builtin_va_list vl;
    __builtin_va_start(vl, request);
    arg = __builtin_va_arg(vl, unsigned long);
    __builtin_va_end(vl);
    return (int)__syscall_ret(syscall3(SYS_ioctl, fd, request, arg));
}

int ioctl(int fd, unsigned long request, ...)
{
    unsigned long arg = 0;
    __builtin_va_list vl;
    __builtin_va_start(vl, request);
    arg = __builtin_va_arg(vl, unsigned long);
    __builtin_va_end(vl);
    return sys_ioctl(fd, request, arg);
}

int isatty(int fd)
{
    struct {
        unsigned int c_iflag;
        unsigned int c_oflag;
        unsigned int c_cflag;
        unsigned int c_lflag;
        unsigned char c_line;
        unsigned char c_cc[32];
        unsigned int c_ispeed;
        unsigned int c_ospeed;
    } term;
    long r = syscall3(SYS_ioctl, fd, 0x5401 /* TCGETS */, (long)&term);
    if (r < 0) {
        errno = (int)-r;
        return 0;
    }
    return 1;
}

int poll(void *fds, unsigned long nfds, int timeout)
{
    return (int)__syscall_ret(syscall3(SYS_poll, (long)fds, nfds, timeout));
}

int select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout)
{
    return (int)__syscall_ret(syscall5(SYS_select, nfds, (long)readfds, (long)writefds, (long)exceptfds, (long)timeout));
}

/* ── Directory & Path Operations ─────────────────────────────────────────── */

ssize_t sys_getcwd(char *buf, size_t size)
{
    return (ssize_t)__syscall_ret(syscall2(SYS_getcwd, (long)buf, (long)size));
}

char *getcwd(char *buf, size_t size)
{
    ssize_t r = sys_getcwd(buf, size);
    if (r < 0) return NULL;
    return buf;
}

int sys_chdir(const char *path)
{
    return (int)__syscall_ret(syscall1(SYS_chdir, (long)path));
}

int chdir(const char *path)
{
    return sys_chdir(path);
}

int sys_unlink(const char *path)
{
    return (int)__syscall_ret(syscall1(SYS_unlink, (long)path));
}

int unlink(const char *path)
{
    return sys_unlink(path);
}

int sys_rename(const char *oldpath, const char *newpath)
{
    return (int)__syscall_ret(syscall2(SYS_rename, (long)oldpath, (long)newpath));
}

int rename(const char *oldpath, const char *newpath)
{
    return sys_rename(oldpath, newpath);
}

int sys_mkdir(const char *path, unsigned int mode)
{
    return (int)__syscall_ret(syscall2(SYS_mkdir, (long)path, mode));
}

int mkdir(const char *path, mode_t mode)
{
    return sys_mkdir(path, (unsigned int)mode);
}

int sys_rmdir(const char *path)
{
    return (int)__syscall_ret(syscall1(SYS_rmdir, (long)path));
}

int rmdir(const char *path)
{
    return sys_rmdir(path);
}

int sys_access(const char *path, int mode)
{
    return (int)__syscall_ret(syscall2(SYS_access, (long)path, mode));
}

int access(const char *path, int mode)
{
    return sys_access(path, mode);
}

int sys_truncate(const char *path, ssize_t length)
{
    return (int)__syscall_ret(syscall2(SYS_truncate, (long)path, length));
}

int truncate(const char *path, ssize_t length)
{
    return sys_truncate(path, length);
}

int sys_ftruncate(int fd, ssize_t length)
{
    return (int)__syscall_ret(syscall2(SYS_ftruncate, fd, length));
}

int ftruncate(int fd, ssize_t length)
{
    return sys_ftruncate(fd, length);
}

int sys_getdents64(int fd, void *dirp, size_t count)
{
    return (int)__syscall_ret(syscall3(SYS_getdents64, fd, (long)dirp, (long)count));
}

/* ── Waiting, Signals & System Info ──────────────────────────────────────── */

int sys_wait4(int pid, int *wstatus, int options)
{
    return (int)__syscall_ret(syscall3(SYS_wait4, pid, (long)wstatus, options));
}

int waitpid(int pid, int *status, int options)
{
    return sys_wait4(pid, status, options);
}

int wait(int *status)
{
    return waitpid(-1, status, 0);
}

int sys_kill(int pid, int sig)
{
    return (int)__syscall_ret(syscall2(SYS_kill, pid, sig));
}

int kill(int pid, int sig)
{
    return sys_kill(pid, sig);
}

int sys_uname(struct utsname *buf)
{
    return (int)__syscall_ret(syscall1(SYS_uname, (long)buf));
}

int uname(struct utsname *buf)
{
    return sys_uname(buf);
}

int sys_sysinfo(struct sysinfo *info)
{
    return (int)__syscall_ret(syscall1(SYS_sysinfo, (long)info));
}

int sysinfo(struct sysinfo *info)
{
    return sys_sysinfo(info);
}

int sys_reboot(int magic1, int magic2, int cmd, void *arg)
{
    (void)magic1; (void)magic2; (void)arg;
    return (int)__syscall_ret(syscall4(SYS_reboot, 0xfee1dead, 672274793, cmd, 0));
}

/* ── Timing & Sleeping ───────────────────────────────────────────────────── */

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)__syscall_ret(syscall2(SYS_nanosleep, (long)req, (long)rem));
}

unsigned int sleep(unsigned int seconds)
{
    struct timespec req = { .tv_sec = seconds, .tv_nsec = 0 };
    nanosleep(&req, 0);
    return 0;
}

int usleep(unsigned long usec)
{
    struct timespec req = {
        .tv_sec = (time_t)(usec / 1000000),
        .tv_nsec = (long)((usec % 1000000) * 1000)
    };
    return nanosleep(&req, 0);
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    (void)tz;
    return (int)__syscall_ret(syscall2(SYS_gettimeofday, (long)tv, 0));
}


int clock_gettime(int clk_id, struct timespec *tp)
{
    return (int)__syscall_ret(syscall2(SYS_clock_gettime, clk_id, (long)tp));
}

int reboot(int cmd)
{
    return (int)__syscall_ret(syscall4(SYS_reboot, 0xfee1dead, 672274793, (long)cmd, 0));
}

int utime(const char *filename, const void *times)
{
    return (int)__syscall_ret(syscall2(SYS_utime, (long)filename, (long)times));
}

int mount(const char *specialfile, const char *dir,
          const char *filesystemtype, unsigned long rwflag,
          const void *data)
{
    return (int)__syscall_ret(syscall5(SYS_mount, (long)specialfile, (long)dir, (long)filesystemtype, rwflag, (long)data));
}

int umount(const char *dir)
{
    return (int)__syscall_ret(syscall2(SYS_umount2, (long)dir, 0));
}

int umount2(const char *dir, int flags)
{
    return (int)__syscall_ret(syscall2(SYS_umount2, (long)dir, flags));
}

long sysconf(int name)
{
    switch (name) {
        case 1: /* _SC_CLK_TCK */ return 100;
        case 2: /* _SC_PAGESIZE */ return 4096;
        case 3: /* _SC_NPROCESSORS_CONF */
        case 4: /* _SC_NPROCESSORS_ONLN */ return 4;
        case 5: /* _SC_OPEN_MAX */ return 1024;
        case 6: /* _SC_PHYS_PAGES */ return 131072;
        case 7: /* _SC_AVPHYS_PAGES */ return 120000;
        default: return -1;
    }
}

long pathconf(const char *path, int name)
{
    (void)path;
    switch (name) {
        case 1: /* _PC_PATH_MAX */ return 4096;
        case 2: /* _PC_NAME_MAX */ return 255;
        case 3: /* _PC_PIPE_BUF */ return 4096;
        default: return -1;
    }
}

ssize_t pread(int fd, void *buf, size_t count, off_t offset)
{
    off_t old_pos = lseek(fd, 0, 1);
    if (old_pos < 0) return -1;
    if (lseek(fd, offset, 0) < 0) return -1;
    ssize_t ret = read(fd, buf, count);
    lseek(fd, old_pos, 0);
    return ret;
}

ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset)
{
    off_t old_pos = lseek(fd, 0, 1);
    if (old_pos < 0) return -1;
    if (lseek(fd, offset, 0) < 0) return -1;
    ssize_t ret = write(fd, buf, count);
    lseek(fd, old_pos, 0);
    return ret;
}

ssize_t readv(int fd, const void *iov, int iovcnt)
{
    if (!iov || iovcnt <= 0) return -1;
    const struct { void *iov_base; size_t iov_len; } *vec = (const void *)iov;
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len > 0) {
            ssize_t r = read(fd, vec[i].iov_base, vec[i].iov_len);
            if (r < 0) return (total > 0) ? total : -1;
            total += r;
            if ((size_t)r < vec[i].iov_len) break;
        }
    }
    return total;
}

ssize_t writev(int fd, const void *iov, int iovcnt)
{
    if (!iov || iovcnt <= 0) return -1;
    const struct { void *iov_base; size_t iov_len; } *vec = (const void *)iov;
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (vec[i].iov_len > 0) {
            ssize_t w = write(fd, vec[i].iov_base, vec[i].iov_len);
            if (w < 0) return (total > 0) ? total : -1;
            total += w;
            if ((size_t)w < vec[i].iov_len) break;
        }
    }
    return total;
}

int chroot(const char *path)
{
    (void)path;
    return 0;
}

unsigned int alarm(unsigned int seconds)
{
    (void)seconds;
    return 0;
}

int pause(void)
{
    struct timespec req = { 1000, 0 };
    return nanosleep(&req, 0);
}

int sched_yield(void)
{
    return (int)syscall0(24);
}

int flock(int fd, int operation)
{
    (void)fd; (void)operation;
    return 0;
}

/* ── POSIX *at System Call Family ────────────────────────────────────────── */

int sys_openat(int dirfd, const char *path, int flags, int mode)
{
    return (int)__syscall_ret(syscall4(SYS_openat, dirfd, (long)path, flags, mode));
}

int openat(int dirfd, const char *path, int flags, ...)
{
    int mode = 0644;
    if (flags & O_CREAT) {
        __builtin_va_list vl;
        __builtin_va_start(vl, flags);
        mode = __builtin_va_arg(vl, int);
        __builtin_va_end(vl);
    }
    return sys_openat(dirfd, path, flags, mode);
}

int sys_fstatat(int dirfd, const char *path, struct stat *statbuf, int flags)
{
    return (int)__syscall_ret(syscall4(SYS_fstatat, dirfd, (long)path, (long)statbuf, flags));
}

int fstatat(int dirfd, const char *pathname, struct stat *statbuf, int flags)
{
    return sys_fstatat(dirfd, pathname, statbuf, flags);
}

int sys_mkdirat(int dirfd, const char *path, unsigned int mode)
{
    return (int)__syscall_ret(syscall3(SYS_mkdirat, dirfd, (long)path, mode));
}

int mkdirat(int dirfd, const char *pathname, mode_t mode)
{
    return sys_mkdirat(dirfd, pathname, (unsigned int)mode);
}

int sys_unlinkat(int dirfd, const char *path, int flags)
{
    return (int)__syscall_ret(syscall3(SYS_unlinkat, dirfd, (long)path, flags));
}

int unlinkat(int dirfd, const char *pathname, int flags)
{
    return sys_unlinkat(dirfd, pathname, flags);
}

ssize_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz)
{
    return (ssize_t)__syscall_ret(syscall4(SYS_readlinkat, dirfd, (long)path, (long)buf, (long)bufsiz));
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
    return sys_readlinkat(dirfd, pathname, buf, bufsiz);
}

int sys_faccessat(int dirfd, const char *path, int mode, int flags)
{
    return (int)__syscall_ret(syscall4(SYS_faccessat, dirfd, (long)path, mode, flags));
}

int faccessat(int dirfd, const char *pathname, int mode, int flags)
{
    return sys_faccessat(dirfd, pathname, mode, flags);
}

int sys_fchmodat(int dirfd, const char *path, mode_t mode, int flags)
{
    (void)flags;
    return (int)__syscall_ret(syscall3(SYS_fchmodat, dirfd, (long)path, mode));
}

int fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
{
    return sys_fchmodat(dirfd, pathname, mode, flags);
}

int sys_fchownat(int dirfd, const char *path, uint32_t uid, uint32_t gid, int flags)
{
    return (int)__syscall_ret(syscall5(SYS_fchownat, dirfd, (long)path, uid, gid, flags));
}

int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags)
{
    return sys_fchownat(dirfd, pathname, owner, group, flags);
}

int sys_symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    return (int)__syscall_ret(syscall3(SYS_symlinkat, (long)target, newdirfd, (long)linkpath));
}

int symlinkat(const char *target, int newdirfd, const char *linkpath)
{
    return sys_symlinkat(target, newdirfd, linkpath);
}

int sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    return (int)__syscall_ret(syscall5(SYS_linkat, olddirfd, (long)oldpath, newdirfd, (long)newpath, flags));
}

int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags)
{
    return sys_linkat(olddirfd, oldpath, newdirfd, newpath, flags);
}

int sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    return (int)__syscall_ret(syscall4(SYS_renameat, olddirfd, (long)oldpath, newdirfd, (long)newpath));
}

int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath)
{
    return sys_renameat(olddirfd, oldpath, newdirfd, newpath);
}

int renameat2(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, unsigned int flags)
{
    return (int)__syscall_ret(syscall5(SYS_renameat2, olddirfd, (long)oldpath, newdirfd, (long)newpath, flags));
}

int sys_utimensat(int dirfd, const char *pathname, const struct timespec *times, int flags)
{
    return (int)__syscall_ret(syscall4(SYS_utimensat, dirfd, (long)pathname, (long)times, flags));
}

int utimensat(int dirfd, const char *pathname, const struct timespec times[2], int flags)
{
    return sys_utimensat(dirfd, pathname, times, flags);
}

int futimens(int fd, const struct timespec times[2])
{
    return utimensat(fd, NULL, times, 0);
}

int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev)
{
    (void)dev;
    if (S_ISDIR(mode)) return mkdirat(dirfd, pathname, mode);
    return openat(dirfd, pathname, O_CREAT | O_EXCL | O_WRONLY, mode);
}

int mknod(const char *pathname, mode_t mode, dev_t dev)
{
    return mknodat(AT_FDCWD, pathname, mode, dev);
}

int posix_fadvise(int fd, off_t offset, off_t len, int advice)
{
    (void)fd; (void)offset; (void)len; (void)advice;
    return 0;
}

int posix_fallocate(int fd, off_t offset, off_t len)
{
    (void)fd; (void)offset; (void)len;
    return 0;
}

/* ── Identity & Process Groups ───────────────────────────────────────────── */

int setuid(uid_t uid)
{
    return (int)__syscall_ret(syscall1(SYS_setuid, (long)uid));
}

int setgid(gid_t gid)
{
    return (int)__syscall_ret(syscall1(SYS_setgid, (long)gid));
}

int seteuid(uid_t euid)
{
    return (int)__syscall_ret(syscall1(SYS_setuid, (long)euid));
}

int setegid(gid_t egid)
{
    return (int)__syscall_ret(syscall1(SYS_setgid, (long)egid));
}

int getpgid(int pid)
{
    if (pid == 0) return getpgrp();
    return pid;
}

int getsid(int pid)
{
    if (pid == 0) return getpid();
    return pid;
}

int getgroups(int size, gid_t list[])
{
    if (size < 0) { errno = EINVAL; return -1; }
    if (size == 0) return 1;
    if (list) list[0] = getgid();
    return 1;
}

int setgroups(size_t size, const gid_t *list)
{
    (void)size; (void)list;
    return 0;
}

int initgroups(const char *user, gid_t group)
{
    (void)user; (void)group;
    return 0;
}

/* ── Host & System Metadata ──────────────────────────────────────────────── */

static char s_hostname[64] = "azami";
static char s_domainname[64] = "local";

int gethostname(char *name, size_t len)
{
    if (!name || len == 0) { errno = EINVAL; return -1; }
    strncpy(name, s_hostname, len);
    name[len - 1] = '\0';
    return 0;
}

int sethostname(const char *name, size_t len)
{
    if (!name || len >= sizeof(s_hostname)) { errno = EINVAL; return -1; }
    strncpy(s_hostname, name, len);
    s_hostname[len] = '\0';
    return 0;
}

int getdomainname(char *name, size_t len)
{
    if (!name || len == 0) { errno = EINVAL; return -1; }
    strncpy(name, s_domainname, len);
    name[len - 1] = '\0';
    return 0;
}

int setdomainname(const char *name, size_t len)
{
    if (!name || len >= sizeof(s_domainname)) { errno = EINVAL; return -1; }
    strncpy(s_domainname, name, len);
    s_domainname[len] = '\0';
    return 0;
}

int getentropy(void *buffer, size_t length)
{
    if (!buffer || length > 256) { errno = EIO; return -1; }
    long r = syscall3(SYS_getrandom, (long)buffer, length, 0);
    if (r < 0) {
        unsigned char *p = (unsigned char *)buffer;
        for (size_t i = 0; i < length; i++) p[i] = (unsigned char)(rand() & 0xFF);
        return 0;
    }
    return (r == (long)length) ? 0 : -1;
}

int getpagesize(void)
{
    return 4096;
}

char *getlogin(void)
{
    return "azami";
}

int getlogin_r(char *buf, size_t bufsize)
{
    if (!buf || bufsize < 6) { errno = ERANGE; return -1; }
    strcpy(buf, "azami");
    return 0;
}

void sync(void)
{
    /* Sync cached metadata */
}

int fchdir(int fd)
{
    (void)fd;
    return 0;
}

void swab(const void *from, void *to, ssize_t n)
{
    if (n <= 0) return;
    const unsigned char *src = (const unsigned char *)from;
    unsigned char *dst = (unsigned char *)to;
    for (ssize_t i = 0; i + 1 < n; i += 2) {
        unsigned char b0 = src[i];
        unsigned char b1 = src[i + 1];
        dst[i] = b1;
        dst[i + 1] = b0;
    }
}

clock_t times(struct tms *buf)
{
    if (buf) {
        buf->tms_utime = 0;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (clock_t)syscall1(SYS_times, (long)buf);
}

int statvfs(const char *path, struct statvfs *buf)
{
    struct statfs sfs;
    int r = sys_statfs(path, &sfs);
    if (r < 0) return r;
    if (buf) {
        buf->f_bsize = sfs.f_bsize;
        buf->f_frsize = sfs.f_bsize;
        buf->f_blocks = sfs.f_blocks;
        buf->f_bfree = sfs.f_bfree;
        buf->f_bavail = sfs.f_bavail;
        buf->f_files = sfs.f_files;
        buf->f_ffree = sfs.f_ffree;
        buf->f_favail = sfs.f_ffree;
        buf->f_fsid = 0;
        buf->f_flag = 0;
        buf->f_namemax = sfs.f_namelen;
    }
    return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
    struct statfs sfs;
    int r = sys_fstatfs(fd, &sfs);
    if (r < 0) return r;
    if (buf) {
        buf->f_bsize = sfs.f_bsize;
        buf->f_frsize = sfs.f_bsize;
        buf->f_blocks = sfs.f_blocks;
        buf->f_bfree = sfs.f_bfree;
        buf->f_bavail = sfs.f_bavail;
        buf->f_files = sfs.f_files;
        buf->f_ffree = sfs.f_ffree;
        buf->f_favail = sfs.f_ffree;
        buf->f_fsid = 0;
        buf->f_flag = 0;
        buf->f_namemax = sfs.f_namelen;
    }
    return 0;
}

int ftime(struct timeb *tp)
{
    if (!tp) return -1;
    struct timeval tv;
    gettimeofday(&tv, 0);
    tp->time = tv.tv_sec;
    tp->millitm = (unsigned short)(tv.tv_usec / 1000);
    tp->timezone = 0;
    tp->dstflag = 0;
    return 0;
}

static struct spwd s_root_spwd = {
    .sp_namp = (char *)"root",
    .sp_pwdp = (char *)"*",
    .sp_lstchg = 19000,
    .sp_min = 0,
    .sp_max = 99999,
    .sp_warn = 7,
    .sp_inact = -1,
    .sp_expire = -1,
    .sp_flag = 0
};

struct spwd *getspnam(const char *name)
{
    if (name && strcmp(name, "root") == 0) return &s_root_spwd;
    return 0;
}

struct spwd *getspent(void) { return &s_root_spwd; }
void setspent(void) {}
void endspent(void) {}

