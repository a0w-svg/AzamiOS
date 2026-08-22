/* ============================================================================
 * AzamiOS Userspace — Standard I/O Header  (POSIX-compatible)
 * File: userland/libc/include/stdio.h
 * ============================================================================ */
#pragma once

#include "sys/syscall.h"
#include "sys/types.h"
#include <stdarg.h>

#define EOF (-1)

#define BUFSIZ 1024
#define FILENAME_MAX 4096
#define FOPEN_MAX 64
#define TMP_MAX 10000
#define L_tmpnam 64

/* ── FILE type (minimal, fd-backed) ─────────────────────────────────────── */
typedef struct _FILE {
    int  fd;           /* underlying file descriptor */
    int  err;          /* error flag */
    int  eof;          /* EOF flag  */
    int  unget_char;   /* buffered ungetc character (-1 if none) */
    int  is_pipe;      /* 1 if opened by popen */
    int  pipe_pid;     /* child PID for popen */
} FILE;

#define stdin  ((FILE *)(void *)0)   /* fd = 0 read via sys_read  */
#define stdout ((FILE *)(void *)1)   /* fd = 1 write via sys_write */
#define stderr ((FILE *)(void *)2)   /* fd = 2 write via sys_write */

/* Seek origins — match Linux */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ── Core character I/O ─────────────────────────────────────────────────── */
int  putchar(int c);
int  getchar(void);
int  fputc(int c, FILE *stream);
int  fgetc(FILE *stream);
int  ungetc(int c, FILE *stream);
int  fputs(const char *s, FILE *stream);
int  puts(const char *s);
char *fgets(char *s, int n, FILE *stream);

/* ── Formatted output ───────────────────────────────────────────────────── */
int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int fprintf(FILE *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...)    __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);

/* ── Formatted input ────────────────────────────────────────────────────── */
int sscanf(const char *str, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *str, const char *fmt, va_list ap);

/* ── File I/O (fd-based wrappers) ───────────────────────────────────────── */
FILE  *fopen(const char *path, const char *mode);
FILE  *fdopen(int fd, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *stream);
FILE  *popen(const char *command, const char *type);
int    pclose(FILE *stream);
int    fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
int    fseeko(FILE *stream, off_t offset, int whence);
off_t  ftello(FILE *stream);
int    fflush(FILE *stream);
int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
int    fileno(FILE *stream);
void   rewind(FILE *stream);

/* ── Temporary files & Line input ────────────────────────────────────────── */
FILE   *tmpfile(void);
char   *tmpnam(char *s);
char   *tempnam(const char *dir, const char *pfx);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream);

/* ── Misc ───────────────────────────────────────────────────────────────── */
void   perror(const char *s);
int    rename(const char *oldpath, const char *newpath);
int    renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
int    remove(const char *pathname);
