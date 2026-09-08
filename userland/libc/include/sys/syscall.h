/* ============================================================================
 * AzamiOS Userspace — Syscall Interface Header
 * File: userland/libc/include/sys/syscall.h
 * ============================================================================ */
#pragma once

typedef unsigned long size_t;
typedef long ssize_t;

/* Canonical syscall-number / uapi-struct definitions, shared verbatim
 * with the kernel (kernel/syscall/syscall.h). This copy is regenerated
 * from ../../../../include/azami/uapi/syscall_nr.h by
 * userland/Makefile's uapi-sync target on every build — never hand-edit
 * it or add a SYS_* define anywhere else. See scripts/check_uapi_sync.sh. */
#include "../azami/uapi/syscall_nr.h"

struct stat;
struct statfs;
struct utsname;
struct sysinfo;

/* ── Function declarations for syscall wrappers ──────────────────────────── */
ssize_t sys_read(int fd, void *buf, size_t count);
ssize_t sys_write(int fd, const void *buf, size_t count);
int sys_open(const char *path, int flags, int mode);
int sys_close(int fd);
int sys_fork(void);
int sys_execve(const char *path, char *const argv[], char *const envp[]);
void sys_exit(int status) __attribute__((noreturn));
int sys_getpid(void);
int sys_getppid(void);
int sys_getpgrp(void);
int sys_setpgid(int pid, int pgid);
int sys_setsid(void);
ssize_t sys_lseek(int fd, ssize_t offset, int whence);
int sys_stat(const char *path, struct stat *statbuf);
int sys_lstat(const char *path, struct stat *statbuf);
int sys_fstat(int fd, struct stat *statbuf);
int sys_statfs(const char *path, struct statfs *buf);
int sys_fstatfs(int fd, struct statfs *buf);
int sys_chmod(const char *path, unsigned int mode);
int sys_fchmod(int fd, unsigned int mode);
int sys_chown(const char *path, unsigned int uid, unsigned int gid);
int sys_fchown(int fd, unsigned int uid, unsigned int gid);
unsigned int sys_umask(unsigned int mask);
int sys_link(const char *oldpath, const char *newpath);
int sys_symlink(const char *target, const char *linkpath);
ssize_t sys_readlink(const char *path, char *buf, size_t bufsiz);
int sys_pipe(int pipefd[2]);
int sys_pipe2(int pipefd[2], int flags);
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_dup3(int oldfd, int newfd, int flags);
int sys_fcntl(int fd, int cmd, ...);
int sys_ioctl(int fd, unsigned long request, ...);
ssize_t sys_getcwd(char *buf, size_t size);
int sys_chdir(const char *path);
int sys_unlink(const char *path);
int sys_rename(const char *oldpath, const char *newpath);
int sys_mkdir(const char *path, unsigned int mode);
int sys_rmdir(const char *path);
int sys_access(const char *path, int mode);
int sys_truncate(const char *path, ssize_t length);
int sys_ftruncate(int fd, ssize_t length);
int sys_getdents64(int fd, void *dirp, size_t count);
int sys_wait4(int pid, int *wstatus, int options);
int sys_kill(int pid, int sig);
int sys_uname(struct utsname *buf);
int sys_sysinfo(struct sysinfo *info);
int sys_reboot(int magic1, int magic2, int cmd, void *arg);
int sys_openat(int dirfd, const char *path, int flags, int mode);
int sys_mkdirat(int dirfd, const char *path, unsigned int mode);
int sys_fstatat(int dirfd, const char *path, struct stat *statbuf, int flags);
int sys_unlinkat(int dirfd, const char *path, int flags);
ssize_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t bufsiz);
int sys_faccessat(int dirfd, const char *path, int mode, int flags);
int sys_fchmodat(int dirfd, const char *path, unsigned int mode, int flags);
int sys_fchownat(int dirfd, const char *path, unsigned int uid, unsigned int gid, int flags);
int sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
int sys_linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
struct timespec;
int sys_renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int sys_utimensat(int dirfd, const char *pathname, const struct timespec *times, int flags);

/* ── Inline syscall wrappers (0–6 arguments) ──────────────────────────────── */

static inline long syscall0(long n)
{
    unsigned long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall1(long n, long a1)
{
    unsigned long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall2(long n, long a1, long a2)
{
    unsigned long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall3(long n, long a1, long a2, long a3)
{
    unsigned long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4)
{
    unsigned long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
    unsigned long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx", "r11", "memory");
    return (long)ret;
}

static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
    unsigned long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return (long)ret;
}
