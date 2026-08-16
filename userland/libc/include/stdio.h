/* ============================================================================
 * AzamiOS Userspace — Standard I/O Header  (POSIX-compatible)
 * File: user/libc/include/stdio.h
 * ============================================================================ */
#pragma once

#include "sys/syscall.h"
#include <stdarg.h>

/* ── FILE type (minimal, fd-backed) ─────────────────────────────────────── */
typedef struct _FILE {
    int  fd;           /* underlying file descriptor */
    int  err;          /* error flag */
    int  eof;          /* EOF flag  */
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
int  fputs(const char *s, FILE *stream);
int  puts(const char *s);
char *fgets(char *s, int n, FILE *stream);

/* ── Formatted output ───────────────────────────────────────────────────── */
int printf(const char *fmt, ...)  __attribute__((format(printf, 1, 2)));
int fprintf(FILE *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *buf, const char *fmt, ...)    __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* ── Formatted input ────────────────────────────────────────────────────── */
int sscanf(const char *str, const char *fmt, ...) __attribute__((format(scanf, 2, 3)));

/* ── File I/O (fd-based wrappers) ───────────────────────────────────────── */
FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int    fseek(FILE *stream, long offset, int whence);
long   ftell(FILE *stream);
int    fflush(FILE *stream);
int    feof(FILE *stream);
int    ferror(FILE *stream);
void   clearerr(FILE *stream);
int    fileno(FILE *stream);
void   rewind(FILE *stream);

/* ── Misc ───────────────────────────────────────────────────────────────── */
void   perror(const char *s);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);
int    rename(const char *oldpath, const char *newpath);
int    remove(const char *pathname);


