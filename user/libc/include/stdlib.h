/**
 * stdlib.h — AzamiOS libc: complete standard utility library
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>
#include <sys/types.h>

/* ── Numeric conversion ─────────────────────────────────────────── */
char  *itoa  (int value, char *str, int base);
int    atoi  (const char *str);
long   atol  (const char *str);
double atof  (const char *str);
long        strtol (const char *s, char **endptr, int base);
unsigned long strtoul(const char *s, char **endptr, int base);
double       strtod (const char *s, char **endptr);

/* ── Integer maths ──────────────────────────────────────────────── */
int  abs (int n);
long labs(long n);

typedef struct { int quot, rem;   } div_t;
typedef struct { long quot, rem;  } ldiv_t;
div_t  div (int num, int den);
ldiv_t ldiv(long num, long den);

/* ── Random numbers ─────────────────────────────────────────────── */
#define RAND_MAX 0x7FFFFFFF
int  rand (void);
void srand(unsigned int seed);

/* ── Memory management ──────────────────────────────────────────── */
void *malloc (size_t size);
void *calloc (size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free   (void *ptr);

/* ── Process control ────────────────────────────────────────────── */
void exit(int status)           __attribute__((noreturn));
void _exit(int status)          __attribute__((noreturn));
void abort(void)                __attribute__((noreturn));
void exec(const char *filename);

/* ── Environment ────────────────────────────────────────────────── */
char *getenv(const char *name);

/* ── Sorting & searching ────────────────────────────────────────── */
void  qsort  (void *base, size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base,
              size_t nmemb, size_t size,
              int (*cmp)(const void *, const void *));

#endif /* _STDLIB_H */