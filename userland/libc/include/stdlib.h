/* ============================================================================
 * AzamiOS Userspace — Standard Library Header (stdlib.h)
 * File: userland/libc/include/stdlib.h
 * ============================================================================ */
#pragma once

#include "sys/syscall.h"
#include "stdbool.h"

#ifndef NULL
#  define NULL ((void *)0)
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX     0x7FFFFFFF

#define INT_MAX   2147483647
#define INT_MIN   (-2147483647 - 1)
#define LONG_MAX  9223372036854775807L
#define LONG_MIN  (-9223372036854775807L - 1)

typedef struct {
    int quot;
    int rem;
} div_t;

typedef struct {
    long quot;
    long rem;
} ldiv_t;

typedef struct {
    long long quot;
    long long rem;
} lldiv_t;

/* Memory allocation */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
void *aligned_alloc(size_t alignment, size_t size);
int   posix_memalign(void **memptr, size_t alignment, size_t size);

/* Numeric conversion */
int                atoi(const char *nptr);
long               atol(const char *nptr);
long long          atoll(const char *nptr);
long               strtol(const char *nptr, char **endptr, int base);
long long          strtoll(const char *nptr, char **endptr, int base);
unsigned long      strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
double             atof(const char *nptr);
double             strtod(const char *nptr, char **endptr);
float              strtof(const char *nptr, char **endptr);
char              *itoa(int value, char *str, int base);

/* Absolute value and division */
int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int numer, int denom);
ldiv_t    ldiv(long numer, long denom);
lldiv_t   lldiv(long long numer, long long denom);

/* Pseudo-random */
int             rand(void);
void            srand(unsigned int seed);
double          drand48(void);
double          erand48(unsigned short xsubi[3]);
long            lrand48(void);
long            nrand48(unsigned short xsubi[3]);
long            mrand48(void);
long            jrand48(unsigned short xsubi[3]);
void            srand48(long seedval);
unsigned short *seed48(unsigned short seed16v[3]);
void            lcong48(unsigned short param[7]);

/* Searching and sorting */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* Temporary paths & file creation */
int   mkstemp(char *tmpl);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);
char *realpath(const char *path, char *resolved_path);
int   getsubopt(char **optionp, char * const *tokens, char **valuep);

/* Environment and process control */
extern char **environ;
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   clearenv(void);
int   system(const char *command);
void  exit(int status) __attribute__((noreturn));
void  abort(void)       __attribute__((noreturn));
int   atexit(void (*fn)(void));

/* GCC runtime support — stack protector & C++ atexit machinery */
#include <stdint.h>
extern uintptr_t __stack_chk_guard;
void __stack_chk_fail(void) __attribute__((noreturn));
int  __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
