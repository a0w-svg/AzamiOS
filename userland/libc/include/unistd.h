/* ============================================================================
 * AzamiOS Userspace — Standard Symbolic Constants and Types (unistd.h)
 * File: userland/libc/include/unistd.h
 * ============================================================================ */
#pragma once

#include "sys/syscall.h"

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
int     fork(void);
int     execve(const char *path, char *const argv[], char *const envp[]);
int     execv(const char *path, char *const argv[]);
int     execvp(const char *file, char *const argv[]);
void    _exit(int status) __attribute__((noreturn));
int     getpid(void);
int     getppid(void);
int     getpgrp(void);
int     setpgid(int pid, int pgid);
int     setsid(void);

/* File I/O */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
ssize_t lseek(int fd, ssize_t offset, int whence);
int     dup(int oldfd);
int     dup2(int oldfd, int newfd);
int     pipe(int pipefd[2]);
int     pipe2(int pipefd[2], int flags);
int     isatty(int fd);

/* Filesystem */
int     unlink(const char *pathname);
int     rmdir(const char *pathname);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);
int     access(const char *pathname, int mode);
int     truncate(const char *path, ssize_t length);
int     ftruncate(int fd, ssize_t length);

/* Timing / Sleeping */
unsigned int sleep(unsigned int seconds);
int          usleep(unsigned long usec);
