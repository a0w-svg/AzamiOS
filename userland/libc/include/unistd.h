/* ============================================================================
 * AzamiOS Userspace — Standard Symbolic Constants and Types (unistd.h)
 * File: userland/libc/include/unistd.h
 * ============================================================================
 */
#pragma once

#include "sys/syscall.h"
#include "sys/types.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* Process control */
int fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execvpe(const char *file, char *const argv[], char *const envp[]);
int execl(const char *path, const char *arg0, ...);
int execlp(const char *file, const char *arg0, ...);
int execle(const char *path, const char *arg0, ...);
void _exit(int status) __attribute__((noreturn));
int getpid(void);
int getppid(void);
int getpgrp(void);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t euid);
int setegid(gid_t egid);
int setpgid(int pid, int pgid);
int getpgid(int pid);
int setsid(void);
int getsid(int pid);
int getgroups(int size, gid_t list[]);
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t group);

/* File I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
ssize_t pread(int fd, void *buf, size_t count, off_t offset);
ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
int close(int fd);
ssize_t lseek(int fd, ssize_t offset, int whence);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
int pipe(int pipefd[2]);
int pipe2(int pipefd[2], int flags);
int isatty(int fd);
int fsync(int fd);
int fdatasync(int fd);
void sync(void);

/* Filesystem */
int unlink(const char *pathname);
int unlinkat(int dirfd, const char *pathname, int flags);
int rmdir(const char *pathname);
int chdir(const char *path);
int fchdir(int fd);
int chroot(const char *path);
char *getcwd(char *buf, size_t size);
int access(const char *pathname, int mode);
int faccessat(int dirfd, const char *pathname, int mode, int flags);
int truncate(const char *path, ssize_t length);
int ftruncate(int fd, ssize_t length);
int link(const char *oldpath, const char *newpath);
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
int symlink(const char *target, const char *linkpath);
int symlinkat(const char *target, int newdirfd, const char *linkpath);
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz);
int chown(const char *pathname, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
int lchown(const char *pathname, uid_t owner, gid_t group);

/* System & Signals */
unsigned int alarm(unsigned int seconds);
int pause(void);
long sysconf(int name);
long pathconf(const char *path, int name);
int getpagesize(void);
int gethostname(char *name, size_t len);
int sethostname(const char *name, size_t len);
int getdomainname(char *name, size_t len);
int setdomainname(const char *name, size_t len);
int getentropy(void *buffer, size_t length);
char *getlogin(void);
int getlogin_r(char *buf, size_t bufsize);
void swab(const void *from, void *to, ssize_t n);

/* Timing / Sleeping */
unsigned int sleep(unsigned int seconds);
int usleep(unsigned long usec);
