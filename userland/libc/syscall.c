/* ============================================================================
 * AzamiOS Userspace — Syscall & POSIX Implementation
 * File: userland/libc/syscall.c
 * ============================================================================ */

#include "include/sys/syscall.h"
#include "include/sys/sysinfo.h"
#include "include/sys/utsname.h"
#include "include/sys/wait.h"
#include "include/sys/time.h"
#include "include/sys/stat.h"
#include "include/sys/statfs.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/signal.h"
#include "include/time.h"
#include "include/string.h"
#include "include/stdlib.h"

/* ── Standard I/O syscall wrappers ───────────────────────────────────────── */

ssize_t sys_read(int fd, void *buf, size_t count)
{
    return (ssize_t)syscall3(SYS_read, fd, (long)buf, (long)count);
}

ssize_t sys_write(int fd, const void *buf, size_t count)
{
    return (ssize_t)syscall3(SYS_write, fd, (long)buf, (long)count);
}

int sys_open(const char *path, int flags, int mode)
{
    return (int)syscall3(SYS_open, (long)path, flags, mode);
}

int sys_close(int fd)
{
    return (int)syscall1(SYS_close, fd);
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
    return (int)syscall0(SYS_fork);
}

int fork(void)
{
    return sys_fork();
}

int sys_execve(const char *path, char *const argv[], char *const envp[])
{
    return (int)syscall3(SYS_execve, (long)path, (long)argv, (long)envp);
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return sys_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[])
{
    char *const envp[] = { 0 };
    return execve(path, argv, envp);
}

int execvp(const char *file, char *const argv[])
{
    if (strchr(file, '/')) {
        return execv(file, argv);
    }

    /* Search standard paths: /, /bin/, /usr/bin/ */
    char buf[256];
    if (strlen(file) + 6 < sizeof(buf)) {
        strcpy(buf, "/");
        strcat(buf, file);
        if (strrchr(buf, '.') == NULL) strcat(buf, ".elf");
        int ret = execv(buf, argv);
        if (ret >= 0) return ret;
    }
    return execv(file, argv);
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

int sys_setpgid(int pid, int pgid)
{
    return (int)syscall2(SYS_setpgid, pid, pgid);
}

int setpgid(int pid, int pgid)
{
    return sys_setpgid(pid, pgid);
}

int sys_setsid(void)
{
    return (int)syscall0(SYS_setsid);
}

int setsid(void)
{
    return sys_setsid();
}

/* ── File Operations & Metadata ─────────────────────────────────────────── */

ssize_t sys_lseek(int fd, ssize_t offset, int whence)
{
    return (ssize_t)syscall3(SYS_lseek, fd, offset, whence);
}

ssize_t lseek(int fd, ssize_t offset, int whence)
{
    return sys_lseek(fd, offset, whence);
}

int sys_stat(const char *path, struct stat *statbuf)
{
    return (int)syscall2(SYS_stat, (long)path, (long)statbuf);
}

int stat(const char *path, struct stat *statbuf)
{
    return sys_stat(path, statbuf);
}

int sys_lstat(const char *path, struct stat *statbuf)
{
    return (int)syscall2(SYS_lstat, (long)path, (long)statbuf);
}

int lstat(const char *path, struct stat *statbuf)
{
    return sys_lstat(path, statbuf);
}

int sys_fstat(int fd, struct stat *statbuf)
{
    return (int)syscall2(SYS_fstat, fd, (long)statbuf);
}

int fstat(int fd, struct stat *statbuf)
{
    return sys_fstat(fd, statbuf);
}

int sys_statfs(const char *path, struct statfs *buf)
{
    return (int)syscall2(SYS_statfs, (long)path, (long)buf);
}

int statfs(const char *path, struct statfs *buf)
{
    return sys_statfs(path, buf);
}

int sys_fstatfs(int fd, struct statfs *buf)
{
    return (int)syscall2(SYS_fstatfs, fd, (long)buf);
}

int fstatfs(int fd, struct statfs *buf)
{
    return sys_fstatfs(fd, buf);
}

int sys_chmod(const char *path, mode_t mode)
{
    return (int)syscall2(SYS_chmod, (long)path, mode);
}

int chmod(const char *path, mode_t mode)
{
    return sys_chmod(path, mode);
}

int sys_fchmod(int fd, mode_t mode)
{
    return (int)syscall2(SYS_fchmod, fd, mode);
}

int fchmod(int fd, mode_t mode)
{
    return sys_fchmod(fd, mode);
}

int sys_chown(const char *path, uint32_t uid, uint32_t gid)
{
    return (int)syscall3(SYS_chown, (long)path, uid, gid);
}

int chown(const char *path, uint32_t uid, uint32_t gid)
{
    return sys_chown(path, uid, gid);
}

int sys_fchown(int fd, uint32_t uid, uint32_t gid)
{
    return (int)syscall3(SYS_fchown, fd, uid, gid);
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

int sys_symlink(const char *target, const char *linkpath)
{
    return (int)syscall2(SYS_symlink, (long)target, (long)linkpath);
}

int symlink(const char *target, const char *linkpath)
{
    return sys_symlink(target, linkpath);
}

ssize_t sys_readlink(const char *path, char *buf, size_t bufsiz)
{
    return (ssize_t)syscall3(SYS_readlink, (long)path, (long)buf, (long)bufsiz);
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    return sys_readlink(path, buf, bufsiz);
}

/* ── Descriptors, Pipes & Multiplexing ───────────────────────────────────── */

int sys_pipe(int pipefd[2])
{
    return (int)syscall1(SYS_pipe, (long)pipefd);
}

int pipe(int pipefd[2])
{
    return sys_pipe(pipefd);
}

int sys_pipe2(int pipefd[2], int flags)
{
    return (int)syscall2(SYS_pipe2, (long)pipefd, flags);
}

int pipe2(int pipefd[2], int flags)
{
    return sys_pipe2(pipefd, flags);
}

int sys_dup(int oldfd)
{
    return (int)syscall1(SYS_dup, oldfd);
}

int dup(int oldfd)
{
    return sys_dup(oldfd);
}

int sys_dup2(int oldfd, int newfd)
{
    return (int)syscall2(SYS_dup2, oldfd, newfd);
}

int dup2(int oldfd, int newfd)
{
    return sys_dup2(oldfd, newfd);
}

int sys_dup3(int oldfd, int newfd, int flags)
{
    (void)flags;
    return (int)syscall2(SYS_dup3, oldfd, newfd);
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
    return (int)syscall3(SYS_fcntl, fd, cmd, arg);
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
    return (int)syscall3(SYS_ioctl, fd, request, arg);
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
    (void)fd;
    return 1;
}

int poll(void *fds, unsigned long nfds, int timeout)
{
    return (int)syscall3(SYS_poll, (long)fds, nfds, timeout);
}

int select(int nfds, void *readfds, void *writefds, void *exceptfds, void *timeout)
{
    return (int)syscall5(SYS_select, nfds, (long)readfds, (long)writefds, (long)exceptfds, (long)timeout);
}

/* ── Directory & Path Operations ─────────────────────────────────────────── */

ssize_t sys_getcwd(char *buf, size_t size)
{
    return (ssize_t)syscall2(SYS_getcwd, (long)buf, (long)size);
}

char *getcwd(char *buf, size_t size)
{
    ssize_t r = sys_getcwd(buf, size);
    if (r < 0) return NULL;
    return buf;
}

int sys_chdir(const char *path)
{
    return (int)syscall1(SYS_chdir, (long)path);
}

int chdir(const char *path)
{
    return sys_chdir(path);
}

int sys_unlink(const char *path)
{
    return (int)syscall1(SYS_unlink, (long)path);
}

int unlink(const char *path)
{
    return sys_unlink(path);
}

int sys_rename(const char *oldpath, const char *newpath)
{
    return (int)syscall2(SYS_rename, (long)oldpath, (long)newpath);
}

int rename(const char *oldpath, const char *newpath)
{
    return sys_rename(oldpath, newpath);
}

int sys_mkdir(const char *path, unsigned int mode)
{
    return (int)syscall2(SYS_mkdir, (long)path, mode);
}

int mkdir(const char *path, mode_t mode)
{
    return sys_mkdir(path, (unsigned int)mode);
}

int sys_rmdir(const char *path)
{
    return (int)syscall1(SYS_rmdir, (long)path);
}

int rmdir(const char *path)
{
    return sys_rmdir(path);
}

int sys_access(const char *path, int mode)
{
    return (int)syscall2(SYS_access, (long)path, mode);
}

int access(const char *path, int mode)
{
    return sys_access(path, mode);
}

int sys_truncate(const char *path, ssize_t length)
{
    return (int)syscall2(SYS_truncate, (long)path, length);
}

int truncate(const char *path, ssize_t length)
{
    return sys_truncate(path, length);
}

int sys_ftruncate(int fd, ssize_t length)
{
    return (int)syscall2(SYS_ftruncate, fd, length);
}

int ftruncate(int fd, ssize_t length)
{
    return sys_ftruncate(fd, length);
}

int sys_getdents64(int fd, void *dirp, size_t count)
{
    return (int)syscall3(SYS_getdents64, fd, (long)dirp, (long)count);
}

/* ── Waiting, Signals & System Info ──────────────────────────────────────── */

int sys_wait4(int pid, int *wstatus, int options)
{
    return (int)syscall3(SYS_wait4, pid, (long)wstatus, options);
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
    return (int)syscall2(SYS_kill, pid, sig);
}

int kill(int pid, int sig)
{
    return sys_kill(pid, sig);
}

int sys_uname(struct utsname *buf)
{
    return (int)syscall1(SYS_uname, (long)buf);
}

int uname(struct utsname *buf)
{
    return sys_uname(buf);
}

int sys_sysinfo(struct sysinfo *info)
{
    return (int)syscall1(SYS_sysinfo, (long)info);
}

int sysinfo(struct sysinfo *info)
{
    return sys_sysinfo(info);
}

int sys_reboot(int magic1, int magic2, int cmd, void *arg)
{
    (void)magic1; (void)magic2; (void)arg;
    return (int)syscall4(SYS_reboot, 0xfee1dead, 672274793, cmd, 0);
}

/* ── Timing & Sleeping ───────────────────────────────────────────────────── */

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)syscall2(SYS_nanosleep, (long)req, (long)rem);
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
    return (int)syscall2(SYS_gettimeofday, (long)tv, 0);
}


int clock_gettime(int clk_id, struct timespec *tp)
{
    return (int)syscall2(SYS_clock_gettime, clk_id, (long)tp);
}

/* ── Sockets & Networking ────────────────────────────────────────────────── */

int socket(int domain, int type, int protocol)
{
    return (int)syscall3(SYS_socket, domain, type, protocol);
}

int connect(int sockfd, const void *addr, size_t addrlen)
{
    return (int)syscall3(SYS_connect, sockfd, (long)addr, (long)addrlen);
}

int bind(int sockfd, const void *addr, size_t addrlen)
{
    return (int)syscall3(SYS_bind, sockfd, (long)addr, (long)addrlen);
}

int listen(int sockfd, int backlog)
{
    return (int)syscall2(SYS_listen, sockfd, backlog);
}

int accept(int sockfd, void *addr, size_t *addrlen)
{
    return (int)syscall3(SYS_accept, sockfd, (long)addr, (long)addrlen);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const void *dest_addr, size_t addrlen)
{
    (void)flags; (void)dest_addr; (void)addrlen;
    return (ssize_t)syscall3(SYS_sendto, sockfd, (long)buf, (long)len);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, void *src_addr, size_t *addrlen)
{
    (void)flags; (void)src_addr; (void)addrlen;
    return (ssize_t)syscall3(SYS_recvfrom, sockfd, (long)buf, (long)len);
}

int shutdown(int sockfd, int how)
{
    return (int)syscall2(SYS_shutdown, sockfd, how);
}

int reboot(int cmd)
{
    return (int)syscall4(SYS_reboot, 0xfee1dead, 672274793, (long)cmd, 0);
}
