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
#ifndef MB_CUR_MAX
#define MB_CUR_MAX   4
#endif

#ifndef INT_MAX
#define INT_MAX   2147483647
#define INT_MIN   (-2147483647 - 1)
#endif
#ifndef LONG_MAX
#define LONG_MAX  9223372036854775807L
#define LONG_MIN  (-9223372036854775807L - 1L)
#endif

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
long double        strtold(const char *nptr, char **endptr);
char              *itoa(int value, char *str, int base);

/* Absolute value and division */
int       abs(int x);
long      labs(long x);
long long llabs(long long x);
div_t     div(int numer, int denom);
ldiv_t    ldiv(long numer, long denom);
lldiv_t   lldiv(long long numer, long long denom);

/* Memory extras */
void  *reallocarray(void *ptr, size_t nmemb, size_t size);
void  *valloc(size_t size);
void  *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);

/* Pseudo-random */
int             rand(void);
void            srand(unsigned int seed);
int             rand_r(unsigned int *seedp);
long            random(void);
void            srandom(unsigned int seed);
double          drand48(void);
double          erand48(unsigned short xsubi[3]);
long            lrand48(void);
long            nrand48(unsigned short xsubi[3]);
long            mrand48(void);
long            jrand48(unsigned short xsubi[3]);
void            srand48(long seedval);
unsigned short *seed48(unsigned short seed16v[3]);
void            lcong48(unsigned short param[7]);

/* Reentrant pseudo-random (GNU extensions) */
struct drand48_data {
    unsigned short __x[3];
    unsigned short __old_x[3];
    unsigned short __c;
    unsigned short __init;
    unsigned long long __a;
};

int   drand48_r(struct drand48_data *buffer, double *result);
int   erand48_r(unsigned short xsubi[3], struct drand48_data *buffer, double *result);
int   lrand48_r(struct drand48_data *buffer, long *result);
int   nrand48_r(unsigned short xsubi[3], struct drand48_data *buffer, long *result);
int   mrand48_r(struct drand48_data *buffer, long *result);
int   jrand48_r(unsigned short xsubi[3], struct drand48_data *buffer, long *result);
int   srand48_r(long seedval, struct drand48_data *buffer);
int   seed48_r(unsigned short seed16v[3], struct drand48_data *buffer);
int   lcong48_r(unsigned short param[7], struct drand48_data *buffer);
char *initstate(unsigned int seed, char *state, size_t n);
char *setstate(char *state);

/* CSPRNG (BSD extension) — kernel-backed via getentropy(2) */
unsigned int arc4random(void);
unsigned int arc4random_uniform(unsigned int upper_bound);
void         arc4random_buf(void *buf, size_t nbytes);
/* Deprecated seeding hooks — no-ops: the pool lives in the kernel. */
void         arc4random_stir(void);
void         arc4random_addrandom(unsigned char *dat, int datlen);

/* Searching and sorting */
void  qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));
void  qsort_r(void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *, void *), void *arg);
void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));

/* Temporary paths & file creation */
int   mkstemp(char *tmpl);
int   mkostemp(char *tmpl, int flags);
int   mkstemps(char *tmpl, int suffixlen);
int   mkostemps(char *tmpl, int suffixlen, int flags);
char *mkdtemp(char *tmpl);
char *mktemp(char *tmpl);
char *realpath(const char *path, char *resolved_path);
char *canonicalize_file_name(const char *path);
int   getsubopt(char **optionp, char * const *tokens, char **valuep);

/* Radix-64 conversion (POSIX.1-2001 XSI) */
char *l64a(long value);
long  a64l(const char *s);

/* User interaction and system status */
int rpmatch(const char *response);
int getloadavg(double loadavg[], int nelem);

/* Multibyte characters */
int mblen(const char *s, size_t n);

/* Pseudo-terminal functions (POSIX) */
int   posix_openpt(int flags);
int   grantpt(int fd);
int   unlockpt(int fd);
char *ptsname(int fd);
int   ptsname_r(int fd, char *buf, size_t buflen);

/* Environment and process control */
extern char **environ;
char *getenv(const char *name);
char *secure_getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   clearenv(void);
int   system(const char *command);
void  exit(int status) __attribute__((noreturn));
void  _Exit(int status) __attribute__((noreturn));
void  quick_exit(int status) __attribute__((noreturn));
int   at_quick_exit(void (*fn)(void));
int   on_exit(void (*fn)(int, void *), void *arg);
void  abort(void)       __attribute__((noreturn));
int   atexit(void (*fn)(void));

/* GCC runtime support — stack protector & C++ atexit machinery */
#include <stdint.h>
extern uintptr_t __stack_chk_guard;
void __stack_chk_fail(void) __attribute__((noreturn));
int  __cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void __cxa_finalize(void *dso);
