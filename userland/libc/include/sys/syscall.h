/* ============================================================================
 * AzamiOS Userspace — Syscall Interface Header
 * File: userland/libc/include/sys/syscall.h
 * ============================================================================ */
#pragma once

typedef unsigned long size_t;
typedef long ssize_t;

/* ── Linux-compatible syscall numbers ─────────────────────────────────────── */
#define SYS_read          0
#define SYS_write         1
#define SYS_open          2
#define SYS_close         3
#define SYS_stat          4
#define SYS_fstat         5
#define SYS_lstat         6
#define SYS_poll          7
#define SYS_waitpid       7
#define SYS_lseek         8
#define SYS_mmap          9
#define SYS_munmap        11
#define SYS_brk           12
#define SYS_rt_sigaction  13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn  15
#define SYS_ioctl         16
#define SYS_readv         19
#define SYS_writev        20
#define SYS_access        21
#define SYS_pipe          22
#define SYS_select        23
#define SYS_dup           32
#define SYS_dup2          33
#define SYS_pause         34
#define SYS_nanosleep     35
#define SYS_alarm         37
#define SYS_getpid        39
#define SYS_socket        41
#define SYS_connect       42
#define SYS_accept        43
#define SYS_sendto        44
#define SYS_recvfrom      45
#define SYS_shutdown      48
#define SYS_bind          49
#define SYS_listen        50
#define SYS_getsockname   51
#define SYS_getpeername   52
#define SYS_setsockopt    54
#define SYS_getsockopt    55
#define SYS_fork          57
#define SYS_execve        59
#define SYS_exit          60
#define SYS_wait4         61
#define SYS_kill          62
#define SYS_uname         63
#define SYS_fcntl         72
#define SYS_truncate      76
#define SYS_ftruncate     77
#define SYS_getdents      78
#define SYS_getcwd        79
#define SYS_chdir         80
#define SYS_rename        82
#define SYS_mkdir         83
#define SYS_rmdir         84
#define SYS_unlink        87
#define SYS_symlink       88
#define SYS_readlink      89
#define SYS_chmod         90
#define SYS_fchmod        91
#define SYS_chown         92
#define SYS_fchown        93
#define SYS_umask         95
#define SYS_gettimeofday  96
#define SYS_sysinfo       99
#define SYS_times         100
#define SYS_getuid        102
#define SYS_getgid        104
#define SYS_setuid        105
#define SYS_setgid        106
#define SYS_geteuid       107
#define SYS_getegid       108
#define SYS_setpgid       109
#define SYS_getppid       110
#define SYS_getpgrp       111
#define SYS_setsid        112
#define SYS_statfs        137
#define SYS_fstatfs       138
#define SYS_reboot        169
#define SYS_time          201
#define SYS_getdents64    217
#define SYS_clock_gettime 228
#define SYS_exit_group    231
#define SYS_dup3          292
#define SYS_pipe2         293

/* ── Azami-specific syscall numbers ───────────────────────────────────────── */
#define SYS_AZ_CHANNEL_CREATE  512
#define SYS_AZ_CHANNEL_SEND    513
#define SYS_AZ_CHANNEL_RECV    514
#define SYS_AZ_SHMEM_CREATE    515
#define SYS_AZ_SHMEM_MAP       516
#define SYS_AZ_OBJECT_CREATE   517
#define SYS_AZ_OBJECT_OPEN     518
#define SYS_AZ_OBJECT_CLOSE    519
#define SYS_AZ_FB_INFO         520
#define SYS_AZ_FB_MAP          521
#define SYS_AZ_SPAWN           522
#define SYS_AZ_YIELD           523
#define SYS_AZ_THREAD_CREATE   524
#define SYS_AZ_SYSSTAT         525
#define SYS_AZ_SHMEM_DESTROY   526
#define SYS_AZ_SHMEM_UNMAP     527
#define SYS_AZ_SET_TIMER       529

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
