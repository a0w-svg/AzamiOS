/**
 * stdio.h — AzamiOS libc: complete C standard I/O
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#ifndef NULL
#define NULL ((void*)0)
#endif

/* ── Constants ──────────────────────────────────────────────────── */
#define EOF      (-1)
#define BUFSIZ   512
#define FILENAME_MAX 128

/* ── FILE type ──────────────────────────────────────────────────── */
#define _FILE_FLAG_EOF  (1 << 0)
#define _FILE_FLAG_ERR  (1 << 1)
#define _FILE_FLAG_RDONLY (1 << 2)
#define _FILE_FLAG_WRONLY (1 << 3)

typedef struct _FILE {
    int   fd;       /* underlying file descriptor (-1 = closed) */
    int   flags;    /* _FILE_FLAG_* bitmask                     */
    long  pos;      /* current byte offset                      */
    /* one-byte read-back buffer (for ungetc) */
    int   unget;    /* -1 = empty, else the char                */
} FILE;

/* Standard streams — defined in stdio.c */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* ── Formatted output ────────────────────────────────────────────── */
int  printf  (const char *fmt, ...);
int  fprintf (FILE *fp, const char *fmt, ...);
int  sprintf (char *buf, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  vprintf (const char *fmt, va_list ap);
int  vfprintf(FILE *fp, const char *fmt, va_list ap);
int  vsprintf(char *buf, const char *fmt, va_list ap);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ── Formatted input ─────────────────────────────────────────────── */
int  scanf  (const char *fmt, ...);
int  fscanf (FILE *fp, const char *fmt, ...);
int  sscanf (const char *str, const char *fmt, ...);
int  vscanf (const char *fmt, va_list ap);
int  vsscanf(const char *str, const char *fmt, va_list ap);

/* ── Character I/O ───────────────────────────────────────────────── */
int  putchar(int c);
int  getchar(void);
int  puts   (const char *s);
char *gets  (char *buf);          /* unsafe but included for compat */

int  fputc(int c, FILE *fp);
int  fgetc(FILE *fp);
int  fputs(const char *s, FILE *fp);
char *fgets(char *buf, int n, FILE *fp);
int  ungetc(int c, FILE *fp);

/* Aliases */
#define getc(fp)    fgetc(fp)
#define putc(c, fp) fputc(c, fp)

/* ── File operations ─────────────────────────────────────────────── */
FILE *fopen (const char *path, const char *mode);
int   fclose(FILE *fp);
size_t fread (void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int   fseek (FILE *fp, long offset, int whence);
long  ftell (FILE *fp);
void  rewind(FILE *fp);
int   fflush(FILE *fp);

/* ── Status predicates ───────────────────────────────────────────── */
int  feof  (FILE *fp);
int  ferror(FILE *fp);
void clearerr(FILE *fp);

/* fseek whence constants */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* ── Misc ────────────────────────────────────────────────────────── */
void  exit(int code);
void  exec(const char *filename);
void  perror(const char *msg);

#endif /* _STDIO_H */