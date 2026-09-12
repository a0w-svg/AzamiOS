/* ============================================================================
 * AzamiOS Userspace — Standard Library Implementation (POSIX-compatible)
 * File: userland/libc/stdlib.c
 * ============================================================================ */

#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/ctype.h"
#include "include/unistd.h"
#include "include/fcntl.h"
#include "include/locale.h"
#include "include/errno.h"
#include "include/sys/stat.h"
#include "include/sys/auxv.h"
#include "include/limits.h"
#include "include/inttypes.h"

/* ── Free-List Memory Allocator ──────────────────────────────────────────── */

typedef struct block_header {
    size_t size; /* User payload size */
    int is_free;
    int _pad;
    struct block_header *next;
    struct block_header *prev;
    size_t magic;
    size_t _pad2;
} block_header_t;

#define BLOCK_MAGIC         0x415A414D494D414CUL
#define ALIGNED_BLOCK_MAGIC 0x415A414D414C4947UL

/* How much the heap grows on a miss. The surplus over the current request is
 * left on the free list, so most malloc()s never reach the brk() syscall. */
#define MALLOC_ARENA_CHUNK  (256UL * 1024UL)

static block_header_t *g_block_head = NULL;
static block_header_t *g_block_tail = NULL; /* BUG-11: tail pointer for O(1) append */
static void *g_heap_top = NULL;

/* Guards g_block_head/g_block_tail/g_heap_top and every block header's
 * fields against concurrent access from multiple threads.
 *
 * This allocator had no locking at all until pthread_create() became real
 * (Phase 1 of the libc hardening work): every new thread's own startup
 * (thread_startup_trampoline() -> __init_thread_tls() in pthread.c) calls
 * malloc() to build its TLS block, so as of that change *every*
 * pthread_create() races this allocator against whatever the parent thread
 * is doing — not a corner case an app has to opt into. Same
 * test-and-set/yield spinlock pattern as pthread.c's pthread_spin_lock().
 * A malloc-free critical section is a hard invariant of this file: nothing
 * under the lock may itself call malloc/free/realloc, or it deadlocks
 * against itself (see how realloc() below avoids calling the public
 * malloc()/free() while holding the lock). */
static int g_malloc_lock = 0;

#define MALLOC_FUTEX_WAIT 128
#define MALLOC_FUTEX_WAKE 129

static void malloc_lock(void)
{
    int exp = 0;
    if (__atomic_compare_exchange_n(&g_malloc_lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;

    for (int i = 0; i < 100; i++) {
        __asm__ volatile("pause");
        exp = 0;
        if (__atomic_compare_exchange_n(&g_malloc_lock, &exp, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }

    while (1) {
        if (exp == 2 || __atomic_exchange_n(&g_malloc_lock, 2, __ATOMIC_ACQ_REL) != 0) {
            syscall4(SYS_futex, (long)&g_malloc_lock, MALLOC_FUTEX_WAIT, 2, 0);
        }
        exp = 0;
        if (__atomic_compare_exchange_n(&g_malloc_lock, &exp, 2, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
    }
}

static void malloc_unlock(void)
{
    if (__atomic_exchange_n(&g_malloc_lock, 0, __ATOMIC_RELEASE) == 2) {
        syscall4(SYS_futex, (long)&g_malloc_lock, MALLOC_FUTEX_WAKE, 1, 0);
    }
}

static void *malloc_unlocked(size_t size)
{
    if (size == 0) return NULL;
    size_t aligned_size = (size + 15) & ~15UL;

    /* 1. Try to find a suitable free block in the free list (first-fit) */
    block_header_t *curr = g_block_head;
    while (curr) {
        if (curr->magic == BLOCK_MAGIC && curr->is_free && curr->size >= aligned_size) {
            /* Check if we can split this block */
            if (curr->size >= aligned_size + sizeof(block_header_t) + 16) {
                block_header_t *split = (block_header_t *)((char *)(curr + 1) + aligned_size);
                split->size = curr->size - aligned_size - sizeof(block_header_t);
                split->is_free = 1;
                split->_pad = 0;
                split->_pad2 = 0;
                split->magic = BLOCK_MAGIC;
                split->next = curr->next;
                split->prev = curr;
                if (curr->next) curr->next->prev = split;
                else g_block_tail = split;
                curr->next = split;
                curr->size = aligned_size;
            }
            curr->is_free = 0;
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    /* 2. No reusable block. Grow the heap — but in large arena chunks, keeping
     * the surplus as one trailing free block. A run of small malloc()s then
     * costs a single brk() syscall instead of one per allocation, and every
     * allocation after the first is served from the free list above. */
    if (!g_heap_top) {
        long base = syscall1(SYS_brk, 0);
        if (base <= 0) return NULL;
        g_heap_top = (void *)(((unsigned long)base + 15UL) & ~15UL);
    }

    size_t total_alloc = sizeof(block_header_t) + aligned_size;

    size_t grow = total_alloc;
    if (grow < MALLOC_ARENA_CHUNK) grow = MALLOC_ARENA_CHUNK;
    grow = (grow + 4095UL) & ~4095UL;                 /* whole pages */

    long next_brk = (long)g_heap_top + (long)grow;
    long res = syscall1(SYS_brk, next_brk);
    if (res < next_brk) {
        /* The big request failed; retry with just what this call needs. */
        grow = (total_alloc + 15UL) & ~15UL;
        next_brk = (long)g_heap_top + (long)grow;
        res = syscall1(SYS_brk, next_brk);
        if (res < next_brk) return NULL;
    }

    block_header_t *new_block = (block_header_t *)g_heap_top;
    g_heap_top = (void *)next_brk;

    new_block->size = aligned_size;
    new_block->is_free = 0;
    new_block->_pad = 0;
    new_block->_pad2 = 0;
    new_block->magic = BLOCK_MAGIC;
    new_block->next = NULL;
    new_block->prev = NULL;

    /* BUG-11: O(1) append via tail pointer instead of O(n) walk */
    if (!g_block_head) {
        g_block_head = new_block;
        g_block_tail = new_block;
    } else {
        g_block_tail->next = new_block;
        new_block->prev = g_block_tail;
        g_block_tail = new_block;
    }

    /* Carve the remainder of the arena chunk into a trailing free block that
     * is physically contiguous with new_block, so free()'s coalescing can
     * still merge across it later. */
    if (grow >= total_alloc + sizeof(block_header_t) + 16) {
        block_header_t *rest = (block_header_t *)((char *)(new_block + 1) + aligned_size);
        rest->size = grow - total_alloc - sizeof(block_header_t);
        rest->is_free = 1;
        rest->_pad = 0;
        rest->_pad2 = 0;
        rest->magic = BLOCK_MAGIC;
        rest->next = NULL;
        rest->prev = new_block;
        new_block->next = rest;
        g_block_tail = rest;
    }

    return (void *)(new_block + 1);
}

void *malloc(size_t size)
{
    malloc_lock();
    void *p = malloc_unlocked(size);
    malloc_unlock();
    return p;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return NULL;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

static void free_unlocked(void *ptr);
void free(void *ptr);

/* realloc() holds the lock for its entire body and calls the *_unlocked
 * helpers directly rather than the public malloc()/free() — those take the
 * same lock themselves, and this lock isn't recursive. */
void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    malloc_lock();

    /* Check if pointer was allocated via aligned_alloc */
    if (((size_t *)ptr)[-2] == ALIGNED_BLOCK_MAGIC) {
        void *raw = ((void **)ptr)[-1];
        block_header_t *hdr = ((block_header_t *)raw) - 1;
        if (hdr->magic != BLOCK_MAGIC) { malloc_unlock(); return NULL; }

        size_t old_size = hdr->size;
        if (size <= old_size) {
            malloc_unlock();
            return ptr;
        }

        void *new_ptr = malloc_unlocked(size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size);
            free_unlocked(ptr);
        }
        malloc_unlock();
        return new_ptr;
    }

    block_header_t *hdr = ((block_header_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC) { malloc_unlock(); return NULL; }

    size_t old_size = hdr->size;
    if (size <= old_size) {
        malloc_unlock();
        return ptr;
    }

    /* BUG-12: in-place expansion when this block is at the end of the heap */
    size_t aligned_size = (size + 15) & ~15UL;
    if ((char *)(hdr + 1) + hdr->size == (char *)g_heap_top) {
        /* Block is at the heap frontier — extend via brk() */
        size_t extra = aligned_size - old_size;
        long next_brk = (long)g_heap_top + (long)extra;
        long res = syscall1(SYS_brk, next_brk);
        if (res >= next_brk) {
            g_heap_top = (void *)next_brk;
            hdr->size = aligned_size;
            /* Update tail if this was the tail */
            if (g_block_tail == hdr) { /* still the tail */ }
            malloc_unlock();
            return ptr;
        }
    }

    void *new_ptr = malloc_unlocked(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free_unlocked(ptr);
    }
    malloc_unlock();
    return new_ptr;
}

void free(void *ptr)
{
    if (!ptr) return;
    malloc_lock();
    free_unlocked(ptr);
    malloc_unlock();
}

static void free_unlocked(void *ptr)
{
    /* Check if pointer was allocated via aligned_alloc */
    if (((size_t *)ptr)[-2] == ALIGNED_BLOCK_MAGIC) {
        void *raw = ((void **)ptr)[-1];
        free_unlocked(raw);
        return;
    }

    block_header_t *hdr = ((block_header_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC) return;

    hdr->is_free = 1;

    /* Coalesce forward (only if physically contiguous) */
    if (hdr->next && hdr->next->is_free && hdr->next->magic == BLOCK_MAGIC) {
        if ((char *)(hdr + 1) + hdr->size == (char *)hdr->next) {
            block_header_t *victim = hdr->next;
            hdr->size += sizeof(block_header_t) + victim->size;
            hdr->next = victim->next;
            if (hdr->next) hdr->next->prev = hdr;
            /* A-10: keep g_block_tail valid after coalesce */
            if (g_block_tail == victim) g_block_tail = hdr;
        }
    }

    /* Coalesce backward (only if physically contiguous) */
    if (hdr->prev && hdr->prev->is_free && hdr->prev->magic == BLOCK_MAGIC) {
        if ((char *)(hdr->prev + 1) + hdr->prev->size == (char *)hdr) {
            block_header_t *victim = hdr;
            hdr->prev->size += sizeof(block_header_t) + hdr->size;
            hdr->prev->next = hdr->next;
            if (hdr->next) hdr->next->prev = hdr->prev;
            /* A-10: keep g_block_tail valid after backward coalesce */
            if (g_block_tail == victim) g_block_tail = hdr->prev;
        }
    }
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || size == 0) return NULL;
    if (alignment < sizeof(void *)) alignment = sizeof(void *);

    size_t extra = alignment + 2 * sizeof(void *);
    void *raw = malloc(size + extra);
    if (!raw) return NULL;

    unsigned long raw_addr = (unsigned long)raw + 2 * sizeof(void *);
    unsigned long aligned_addr = (raw_addr + alignment - 1) & ~(alignment - 1);

    ((void **)aligned_addr)[-1] = raw;
    ((size_t *)aligned_addr)[-2] = ALIGNED_BLOCK_MAGIC;

    return (void *)aligned_addr;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    if (!memptr) return EINVAL;
    if ((alignment % sizeof(void *)) != 0 || (alignment & (alignment - 1)) != 0) {
        return EINVAL;
    }
    void *ptr = aligned_alloc(alignment, size);
    if (!ptr) return ENOMEM;
    *memptr = ptr;
    return 0;
}

void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
    if (nmemb > 0 && size > (size_t)-1 / nmemb) {
        errno = ENOMEM;
        return NULL;
    }
    return realloc(ptr, nmemb * size);
}

void *valloc(size_t size)
{
    return aligned_alloc(4096, size);
}

void *pvalloc(size_t size)
{
    size_t rounded = (size + 4095) & ~4095UL;
    return aligned_alloc(4096, rounded);
}

size_t malloc_usable_size(void *ptr)
{
    if (!ptr) return 0;
    if (((size_t *)ptr)[-2] == ALIGNED_BLOCK_MAGIC) {
        void *raw = ((void **)ptr)[-1];
        block_header_t *hdr = ((block_header_t *)raw) - 1;
        if (hdr->magic == BLOCK_MAGIC) return hdr->size;
        return 0;
    }
    block_header_t *hdr = ((block_header_t *)ptr) - 1;
    if (hdr->magic == BLOCK_MAGIC) return hdr->size;
    return 0;
}


/* ── Numeric conversion ──────────────────────────────────────────────────── */

static unsigned long long _strtoull_core(const char *nptr, char **endptr,
                                          int base, int *negative)
{
    while (*nptr == ' ' || (*nptr >= '\t' && *nptr <= '\r')) nptr++;

    *negative = 0;
    if (*nptr == '-') { *negative = 1; nptr++; }
    else if (*nptr == '+') { nptr++; }

    if (base == 0) {
        if (nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) { base = 16; nptr += 2; }
        else if (nptr[0] == '0') { base = 8; nptr++; }
        else { base = 10; }
    } else if (base == 16 && nptr[0] == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }

    unsigned long long result = 0;
    const char *start = nptr;
    while (*nptr) {
        int digit;
        char c = *nptr;
        if      (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long long)base + (unsigned long long)digit;
        nptr++;
    }
    if (endptr) *endptr = (char *)((nptr == start) ? start : nptr);
    return result;
}

unsigned long long strtoull(const char *nptr, char **endptr, int base)
{
    int neg; return _strtoull_core(nptr, endptr, base, &neg);
}

unsigned long strtoul(const char *nptr, char **endptr, int base)
{
    int neg; return (unsigned long)_strtoull_core(nptr, endptr, base, &neg);
}

long long strtoll(const char *nptr, char **endptr, int base)
{
    int neg;
    unsigned long long v = _strtoull_core(nptr, endptr, base, &neg);
    return neg ? -(long long)v : (long long)v;
}

long strtol(const char *nptr, char **endptr, int base)
{
    return (long)strtoll(nptr, endptr, base);
}

int atoi(const char *nptr)
{
    return (int)strtol(nptr, (char **)0, 10);
}

long atol(const char *nptr)
{
    return strtol(nptr, (char **)0, 10);
}

long long atoll(const char *nptr)
{
    return strtoll(nptr, (char **)0, 10);
}

double strtod(const char *nptr, char **endptr)
{
    while (isspace((unsigned char)*nptr)) nptr++;
    int sign = 1;
    if (*nptr == '-') { sign = -1; nptr++; }
    else if (*nptr == '+') { nptr++; }

    double val = 0.0;
    while (isdigit((unsigned char)*nptr)) {
        val = val * 10.0 + (*nptr - '0');
        nptr++;
    }
    if (*nptr == '.') {
        nptr++;
        double frac = 0.1;
        while (isdigit((unsigned char)*nptr)) {
            val += (*nptr - '0') * frac;
            frac *= 0.1;
            nptr++;
        }
    }
    if (*nptr == 'e' || *nptr == 'E') {
        const char *exp_start = nptr;
        nptr++;
        int exp_sign = 1;
        if (*nptr == '-') { exp_sign = -1; nptr++; }
        else if (*nptr == '+') { nptr++; }

        int exp_val = 0;
        int exp_digits = 0;
        while (isdigit((unsigned char)*nptr)) {
            exp_val = exp_val * 10 + (*nptr - '0');
            exp_digits++;
            nptr++;
        }
        if (exp_digits > 0) {
            double factor = 1.0;
            for (int i = 0; i < exp_val; i++) factor *= 10.0;
            if (exp_sign > 0) val *= factor;
            else val /= factor;
        } else {
            nptr = exp_start;
        }
    }
    if (endptr) *endptr = (char *)nptr;
    return sign * val;
}

double atof(const char *nptr)
{
    return strtod(nptr, (char **)0);
}

float strtof(const char *nptr, char **endptr)
{
    return (float)strtod(nptr, endptr);
}

long double strtold(const char *nptr, char **endptr)
{
    return (long double)strtod(nptr, endptr);
}

char *itoa(int value, char *str, int base)
{
    if (!str) return (char *)0;
    if (base < 2 || base > 36) { *str = '\0'; return str; }

    char *rc = str;
    char *ptr = str;
    char *low;
    unsigned int uval;

    if (value < 0 && base == 10) {
        *ptr++ = '-';
        rc++;
        uval = (unsigned int)-value;
    } else {
        uval = (unsigned int)value;
    }

    do {
        *ptr++ = "0123456789abcdefghijklmnopqrstuvwxyz"[uval % (unsigned int)base];
        uval /= (unsigned int)base;
    } while (uval);

    *ptr-- = '\0';
    low = rc;
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return str;
}

/* ── Absolute value and division ─────────────────────────────────────────── */

int abs(int x)
{
    return x < 0 ? -x : x;
}

long labs(long x)
{
    return x < 0 ? -x : x;
}

long long llabs(long long x)
{
    return x < 0 ? -x : x;
}

div_t div(int numer, int denom)
{
    div_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

lldiv_t lldiv(long long numer, long long denom)
{
    lldiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

intmax_t imaxabs(intmax_t j)
{
    return j < 0 ? -j : j;
}

imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom)
{
    imaxdiv_t r;
    r.quot = numer / denom;
    r.rem  = numer % denom;
    return r;
}

intmax_t strtoimax(const char *nptr, char **endptr, int base)
{
    return (intmax_t)strtoll(nptr, endptr, base);
}

uintmax_t strtoumax(const char *nptr, char **endptr, int base)
{
    return (uintmax_t)strtoull(nptr, endptr, base);
}

/* ── Pseudo-random ───────────────────────────────────────────────────────── */

static unsigned int g_rand_state = 123456789;

void srand(unsigned int seed) { g_rand_state = seed; }

int rand(void)
{
    g_rand_state = g_rand_state * 1103515245 + 12345;
    return (int)((g_rand_state / 65536) % (RAND_MAX + 1U));
}

int rand_r(unsigned int *seedp)
{
    if (!seedp) return rand();
    *seedp = *seedp * 1103515245 + 12345;
    return (int)((*seedp / 65536) % (RAND_MAX + 1U));
}

long random(void)
{
    return (long)rand();
}

void srandom(unsigned int seed)
{
    srand(seed);
}

static unsigned short s_rand48_seed[3] = { 0x330e, 0xabcd, 0x1234 };
static unsigned short s_rand48_mult[3] = { 0xe66d, 0xdeec, 0x0005 };
static unsigned short s_rand48_add = 0x000b;

static void _drand48_step(unsigned short xsubi[3])
{
    unsigned long long x = (unsigned long long)xsubi[0] |
                          ((unsigned long long)xsubi[1] << 16) |
                          ((unsigned long long)xsubi[2] << 32);
    unsigned long long a = (unsigned long long)s_rand48_mult[0] |
                          ((unsigned long long)s_rand48_mult[1] << 16) |
                          ((unsigned long long)s_rand48_mult[2] << 32);
    unsigned long long next = (x * a + s_rand48_add) & 0xFFFFFFFFFFFFULL;
    xsubi[0] = (unsigned short)(next & 0xFFFF);
    xsubi[1] = (unsigned short)((next >> 16) & 0xFFFF);
    xsubi[2] = (unsigned short)((next >> 32) & 0xFFFF);
}

double erand48(unsigned short xsubi[3])
{
    _drand48_step(xsubi);
    unsigned long long x = (unsigned long long)xsubi[0] |
                          ((unsigned long long)xsubi[1] << 16) |
                          ((unsigned long long)xsubi[2] << 32);
    return (double)x / 281474976710656.0;
}

double drand48(void)
{
    return erand48(s_rand48_seed);
}

long lrand48(void)
{
    _drand48_step(s_rand48_seed);
    unsigned long high = ((unsigned long)s_rand48_seed[2] << 15) | ((unsigned long)s_rand48_seed[1] >> 1);
    return (long)(high & 0x7FFFFFFFL);
}

long nrand48(unsigned short xsubi[3])
{
    _drand48_step(xsubi);
    unsigned long high = ((unsigned long)xsubi[2] << 15) | ((unsigned long)xsubi[1] >> 1);
    return (long)(high & 0x7FFFFFFFL);
}

long mrand48(void)
{
    _drand48_step(s_rand48_seed);
    long high = (long)(((unsigned long)s_rand48_seed[2] << 16) | (unsigned long)s_rand48_seed[1]);
    return high;
}

long jrand48(unsigned short xsubi[3])
{
    _drand48_step(xsubi);
    long high = (long)(((unsigned long)xsubi[2] << 16) | (unsigned long)xsubi[1]);
    return high;
}

void srand48(long seedval)
{
    s_rand48_seed[0] = 0x330e;
    s_rand48_seed[1] = (unsigned short)(seedval & 0xFFFF);
    s_rand48_seed[2] = (unsigned short)((seedval >> 16) & 0xFFFF);
    s_rand48_mult[0] = 0xe66d;
    s_rand48_mult[1] = 0xdeec;
    s_rand48_mult[2] = 0x0005;
    s_rand48_add = 0x000b;
}

unsigned short *seed48(unsigned short seed16v[3])
{
    static unsigned short old[3];
    old[0] = s_rand48_seed[0];
    old[1] = s_rand48_seed[1];
    old[2] = s_rand48_seed[2];
    s_rand48_seed[0] = seed16v[0];
    s_rand48_seed[1] = seed16v[1];
    s_rand48_seed[2] = seed16v[2];
    return old;
}

void lcong48(unsigned short param[7])
{
    s_rand48_seed[0] = param[0];
    s_rand48_seed[1] = param[1];
    s_rand48_seed[2] = param[2];
    s_rand48_mult[0] = param[3];
    s_rand48_mult[1] = param[4];
    s_rand48_mult[2] = param[5];
    s_rand48_add     = param[6];
}

/* ── Reentrant Pseudo-Random (GNU Extensions) ────────────────────────────── */

static void _drand48_r_step(unsigned short xsubi[3], struct drand48_data *buffer)
{
    if (!buffer->__init) {
        buffer->__a = 0x5deece66dULL;
        buffer->__c = 0xb;
        buffer->__init = 1;
    }
    unsigned long long x = (unsigned long long)xsubi[0] |
                          ((unsigned long long)xsubi[1] << 16) |
                          ((unsigned long long)xsubi[2] << 32);
    unsigned long long next = (x * buffer->__a + buffer->__c) & 0xFFFFFFFFFFFFULL;
    xsubi[0] = (unsigned short)(next & 0xFFFF);
    xsubi[1] = (unsigned short)((next >> 16) & 0xFFFF);
    xsubi[2] = (unsigned short)((next >> 32) & 0xFFFF);
}

int drand48_r(struct drand48_data *buffer, double *result)
{
    if (!buffer || !result) return -1;
    _drand48_r_step(buffer->__x, buffer);
    unsigned long long x = (unsigned long long)buffer->__x[0] |
                          ((unsigned long long)buffer->__x[1] << 16) |
                          ((unsigned long long)buffer->__x[2] << 32);
    *result = (double)x / 281474976710656.0;
    return 0;
}

int erand48_r(unsigned short xsubi[3], struct drand48_data *buffer, double *result)
{
    if (!xsubi || !buffer || !result) return -1;
    _drand48_r_step(xsubi, buffer);
    unsigned long long x = (unsigned long long)xsubi[0] |
                          ((unsigned long long)xsubi[1] << 16) |
                          ((unsigned long long)xsubi[2] << 32);
    *result = (double)x / 281474976710656.0;
    return 0;
}

int lrand48_r(struct drand48_data *buffer, long *result)
{
    if (!buffer || !result) return -1;
    _drand48_r_step(buffer->__x, buffer);
    unsigned long high = ((unsigned long)buffer->__x[2] << 15) | ((unsigned long)buffer->__x[1] >> 1);
    *result = (long)(high & 0x7FFFFFFFL);
    return 0;
}

int nrand48_r(unsigned short xsubi[3], struct drand48_data *buffer, long *result)
{
    if (!xsubi || !buffer || !result) return -1;
    _drand48_r_step(xsubi, buffer);
    unsigned long high = ((unsigned long)xsubi[2] << 15) | ((unsigned long)xsubi[1] >> 1);
    *result = (long)(high & 0x7FFFFFFFL);
    return 0;
}

int mrand48_r(struct drand48_data *buffer, long *result)
{
    if (!buffer || !result) return -1;
    _drand48_r_step(buffer->__x, buffer);
    long high = (long)(((unsigned long)buffer->__x[2] << 16) | (unsigned long)buffer->__x[1]);
    *result = high;
    return 0;
}

int jrand48_r(unsigned short xsubi[3], struct drand48_data *buffer, long *result)
{
    if (!xsubi || !buffer || !result) return -1;
    _drand48_r_step(xsubi, buffer);
    long high = (long)(((unsigned long)xsubi[2] << 16) | (unsigned long)xsubi[1]);
    *result = high;
    return 0;
}

int srand48_r(long seedval, struct drand48_data *buffer)
{
    if (!buffer) return -1;
    buffer->__x[0] = 0x330e;
    buffer->__x[1] = (unsigned short)(seedval & 0xFFFF);
    buffer->__x[2] = (unsigned short)((seedval >> 16) & 0xFFFF);
    buffer->__old_x[0] = buffer->__x[0];
    buffer->__old_x[1] = buffer->__x[1];
    buffer->__old_x[2] = buffer->__x[2];
    buffer->__a = 0x5deece66dULL;
    buffer->__c = 0xb;
    buffer->__init = 1;
    return 0;
}

int seed48_r(unsigned short seed16v[3], struct drand48_data *buffer)
{
    if (!seed16v || !buffer) return -1;
    buffer->__old_x[0] = buffer->__x[0];
    buffer->__old_x[1] = buffer->__x[1];
    buffer->__old_x[2] = buffer->__x[2];
    buffer->__x[0] = seed16v[0];
    buffer->__x[1] = seed16v[1];
    buffer->__x[2] = seed16v[2];
    buffer->__a = 0x5deece66dULL;
    buffer->__c = 0xb;
    buffer->__init = 1;
    return 0;
}

int lcong48_r(unsigned short param[7], struct drand48_data *buffer)
{
    if (!param || !buffer) return -1;
    buffer->__x[0] = param[0];
    buffer->__x[1] = param[1];
    buffer->__x[2] = param[2];
    buffer->__a = (unsigned long long)param[3] |
                 ((unsigned long long)param[4] << 16) |
                 ((unsigned long long)param[5] << 32);
    buffer->__c = param[6];
    buffer->__init = 1;
    return 0;
}

static unsigned int s_rand_default_state[32] = { 123456789 };
static unsigned int *s_rand_state_ptr = s_rand_default_state;

char *initstate(unsigned int seed, char *state, size_t n)
{
    if (!state || n < 8) return NULL;
    char *old = (char *)s_rand_state_ptr;
    s_rand_state_ptr = (unsigned int *)state;
    srandom(seed);
    s_rand_state_ptr[0] = seed;
    return old;
}

char *setstate(char *state)
{
    if (!state) return NULL;
    char *old = (char *)s_rand_state_ptr;
    s_rand_state_ptr = (unsigned int *)state;
    return old;
}

/* ── Searching & Sorting ─────────────────────────────────────────────────── */

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *))
{
    size_t l = 0, r = nmemb;
    while (l < r) {
        size_t mid = l + (r - l) / 2;
        const void *elem = (const char *)base + mid * size;
        int cmp = compar(key, elem);
        if (cmp == 0) return (void *)elem;
        if (cmp < 0) r = mid;
        else l = mid + 1;
    }
    return NULL;
}

static void _qsort_swap(char *a, char *b, size_t size)
{
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

typedef int (*_cmp_r_fn)(const void *, const void *, void *);

/* Sift base[start] down through the max-heap that occupies base[0..end). */
static void _heap_sift(char *base, size_t size, size_t start, size_t end,
                       _cmp_r_fn cmp, void *arg)
{
    for (size_t root = start;;) {
        size_t child = 2 * root + 1;
        if (child >= end) break;
        if (child + 1 < end &&
            cmp(base + child * size, base + (child + 1) * size, arg) < 0)
            child++;
        if (cmp(base + root * size, base + child * size, arg) >= 0) break;
        _qsort_swap(base + root * size, base + child * size, size);
        root = child;
    }
}

/* Heapsort — the O(n log n) worst-case fallback the introsort guard drops to. */
static void _heapsort(char *base, size_t n, size_t size, _cmp_r_fn cmp, void *arg)
{
    for (size_t start = n / 2; start-- > 0; )
        _heap_sift(base, size, start, n, cmp, arg);
    for (size_t end = n; end-- > 1; ) {
        _qsort_swap(base, base + end * size, size);
        _heap_sift(base, size, 0, end, cmp, arg);
    }
}

/*
 * Shared sort core for qsort() and qsort_r() — an introsort.
 *
 *   - median-of-three pivot, so sorted / reverse-sorted input avoids O(n^2);
 *   - Hoare partitioning, which walks through runs of equal keys instead of
 *     piling them on one side (all-equal input was O(n^2) in the old Lomuto
 *     version);
 *   - recurse into the smaller partition, loop on the larger — stack depth
 *     O(log n), not O(n);
 *   - when quicksort recursion exceeds 2*floor(log2 n), the partition split
 *     has gone bad too many times: fall back to heapsort for a hard
 *     O(n log n) ceiling on adversarial input;
 *   - insertion sort for short spans.
 */
#define QSORT_SMALL 12

static void _qsort_core(char *base, size_t nmemb, size_t size,
                        _cmp_r_fn cmp, void *arg, int depth)
{
    while (nmemb > QSORT_SMALL) {
        if (depth-- <= 0) {
            _heapsort(base, nmemb, size, cmp, arg);
            return;
        }

        char *lo  = base;
        char *mid = base + (nmemb / 2) * size;
        char *hi  = base + (nmemb - 1) * size;

        /* Order lo <= mid <= hi, then move the median into lo as the pivot. */
        if (cmp(lo, mid, arg) > 0)  _qsort_swap(lo, mid, size);
        if (cmp(lo, hi, arg) > 0)   _qsort_swap(lo, hi, size);
        if (cmp(mid, hi, arg) > 0)  _qsort_swap(mid, hi, size);
        _qsort_swap(lo, mid, size);

        char *i = lo;
        char *j = hi + size;
        for (;;) {
            do { i += size; } while (i <= hi && cmp(i, lo, arg) < 0);
            do { j -= size; } while (cmp(j, lo, arg) > 0);
            if (i >= j) break;
            _qsort_swap(i, j, size);
        }
        _qsort_swap(lo, j, size);            /* pivot to its final slot */

        size_t left_n  = (size_t)((j - lo) / size);
        size_t right_n = nmemb - left_n - 1;
        char  *right   = j + size;

        if (left_n < right_n) {
            _qsort_core(lo, left_n, size, cmp, arg, depth);
            base = right; nmemb = right_n;
        } else {
            _qsort_core(right, right_n, size, cmp, arg, depth);
            base = lo; nmemb = left_n;
        }
    }

    for (size_t k = 1; k < nmemb; k++) {
        char *cur = base + k * size;
        for (char *p = cur; p > base && cmp(p - size, p, arg) > 0; p -= size)
            _qsort_swap(p - size, p, size);
    }
}

static int _qsort_depth_limit(size_t n)
{
    int k = 0;
    while (n > 1) { n >>= 1; k++; }
    return 2 * k + 1;
}

static int _qsort_cmp_shim(const void *a, const void *b, void *arg)
{
    return ((int (*)(const void *, const void *))arg)(a, b);
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb < 2 || size == 0 || !base || !compar) return;
    _qsort_core((char *)base, nmemb, size, _qsort_cmp_shim, (void *)compar,
                _qsort_depth_limit(nmemb));
}

void qsort_r(void *base, size_t nmemb, size_t size,
             int (*compar)(const void *, const void *, void *), void *arg)
{
    if (nmemb < 2 || size == 0 || !base || !compar) return;
    _qsort_core((char *)base, nmemb, size, compar, arg, _qsort_depth_limit(nmemb));
}

/* ── CSPRNG (arc4random family) — kernel-backed, no userspace state ──────── */

static int user_cpu_has_rdrand(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;
    unsigned a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    cached = (c & (1u << 30)) ? 1 : 0;
    return cached;
}

void arc4random_buf(void *buf, size_t nbytes)
{
    unsigned char *p = (unsigned char *)buf;
    while (nbytes) {
        size_t chunk = nbytes > 256 ? 256 : nbytes;   /* getentropy(2) cap */
        if (getentropy(p, chunk) != 0) {
            /* The kernel CSPRNG never fails in practice; degrade rather than
             * hand back an uninitialised buffer if it somehow does. */
            for (size_t i = 0; i < chunk; i++) p[i] = (unsigned char)rand();
        }
        /* Hardware entropy injection: if CPU supports RDRAND, fold it into
         * the buffer. This provides dual-source defense-in-depth and ensures
         * immediate divergence upon fork(). */
        if (user_cpu_has_rdrand()) {
            size_t qwords = chunk / 8;
            uint64_t *q = (uint64_t *)p;
            for (size_t i = 0; i < qwords; i++) {
                uint64_t hw = 0;
                unsigned char ok = 0;
                __asm__ volatile("rdrand %0; setc %1" : "=r"(hw), "=qm"(ok));
                if (ok) q[i] ^= hw;
            }
            size_t rem = chunk & 7;
            if (rem) {
                uint64_t hw = 0;
                unsigned char ok = 0;
                __asm__ volatile("rdrand %0; setc %1" : "=r"(hw), "=qm"(ok));
                if (ok) {
                    unsigned char *tail = p + qwords * 8;
                    for (size_t i = 0; i < rem; i++) {
                        tail[i] ^= (unsigned char)(hw >> (i * 8));
                    }
                }
            }
        }
        p += chunk;
        nbytes -= chunk;
    }
}

unsigned int arc4random(void)
{
    unsigned int v;
    arc4random_buf(&v, sizeof(v));
    return v;
}

unsigned int arc4random_uniform(unsigned int upper_bound)
{
    if (upper_bound < 2) return 0;
    /* Rejection sampling to avoid modulo bias. */
    unsigned int min = (unsigned int)(-upper_bound) % upper_bound;
    unsigned int r;
    do { r = arc4random(); } while (r < min);
    return r % upper_bound;
}

/* The pool is the kernel CSPRNG, reseeded by the kernel; these BSD-era hooks
 * that let a program stir in its own entropy are obsolete and do nothing. */
void arc4random_stir(void) { }
void arc4random_addrandom(unsigned char *dat, int datlen) { (void)dat; (void)datlen; }

/* ── Environment & Process ───────────────────────────────────────────────── */

#define MAX_ENV_VARS 64
static char *g_env_table[MAX_ENV_VARS + 1] = { NULL };
char **environ = g_env_table;

static void _ensure_env_synced(void)
{
    environ = g_env_table;
}

char *getenv(const char *name)
{
    if (!name) return NULL;
    size_t nlen = strlen(name);
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (g_env_table[i] && strncmp(g_env_table[i], name, nlen) == 0 && g_env_table[i][nlen] == '=') {
            return g_env_table[i] + nlen + 1;
        }
    }
    if (strcmp(name, "PATH") == 0) return "/bin:/sbin:/usr/bin:/usr/sbin:/";
    if (strcmp(name, "USER") == 0) return "root";
    if (strcmp(name, "HOME") == 0) return "/root";
    if (strcmp(name, "SHELL") == 0) return "/bin/sh.elf";
    if (strcmp(name, "TERM") == 0) return "azami";
    return NULL;
}

char *secure_getenv(const char *name)
{
    if (!name) return NULL;
    if (getauxval(AT_SECURE) != 0 ||
        getuid() != geteuid() ||
        getgid() != getegid()) {
        return NULL;
    }
    return getenv(name);
}

int setenv(const char *name, const char *value, int overwrite)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    if (!value) value = "";

    char *existing = getenv(name);
    if (existing && !overwrite) return 0;

    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    char *entry = (char *)malloc(nlen + 1 + vlen + 1);
    if (!entry) return -1;

    strcpy(entry, name);
    strcat(entry, "=");
    strcat(entry, value);

    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (g_env_table[i] && strncmp(g_env_table[i], name, nlen) == 0 && g_env_table[i][nlen] == '=') {
            free(g_env_table[i]);
            g_env_table[i] = entry;
            _ensure_env_synced();
            return 0;
        }
    }
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!g_env_table[i]) {
            g_env_table[i] = entry;
            g_env_table[i + 1] = NULL;
            _ensure_env_synced();
            return 0;
        }
    }
    free(entry);
    return -1;
}

int unsetenv(const char *name)
{
    if (!name || !name[0] || strchr(name, '=')) return -1;
    size_t nlen = strlen(name);
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (g_env_table[i] && strncmp(g_env_table[i], name, nlen) == 0 && g_env_table[i][nlen] == '=') {
            free(g_env_table[i]);
            /* Shift remaining entries left */
            for (int j = i; j < MAX_ENV_VARS - 1; j++) {
                g_env_table[j] = g_env_table[j + 1];
            }
            g_env_table[MAX_ENV_VARS - 1] = NULL;
            _ensure_env_synced();
            return 0;
        }
    }
    return 0;
}

int putenv(char *string)
{
    if (!string) return -1;
    char *eq = strchr(string, '=');
    if (!eq) return -1;
    size_t nlen = (size_t)(eq - string);
    char name[64];
    if (nlen >= sizeof(name)) return -1;
    strncpy(name, string, nlen);
    name[nlen] = '\0';
    return setenv(name, eq + 1, 1);
}

int system(const char *command)
{
    if (!command) return 1; /* Shell is always available in AzamiOS */
    int pid = sys_fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        char *const argv[] = { "/bin/sh.elf", "-c", (char *)command, NULL };
        sys_execve("/bin/sh.elf", argv, environ);
        sys_execve("/sh.elf", argv, environ);
        _exit(127);
    }
    int status = 0;
    sys_wait4(pid, &status, 0);
    return status;
}

/* no_stack_protector: this function reseeds __stack_chk_guard itself, and a
 * protected function's epilogue checks its saved canary against whatever
 * the global holds *now* — not the placeholder it held at this function's
 * own entry. Without this attribute, __libc_init() would fail its own
 * canary check on return, every time, for every process. See the matching
 * comment on kernel/security/security.c's security_init(), which reseeds
 * the kernel-side guard the same way and needs the same exemption. */
void __libc_init(int argc, char **argv, char **envp) __attribute__((no_stack_protector));
void __libc_init(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;

    /* Reseed the stack canary before anything else runs. __stack_chk_guard
     * starts life as the fixed constant above — enough to satisfy the
     * linker, not enough to stop an attacker who has read this binary from
     * predicting it. getentropy() already falls back to rand() if
     * SYS_getrandom ever fails, so this always leaves the guard non-zero;
     * zero the low byte the same way the kernel does (security_init(), in
     * kernel/security/security.c) so a NUL-terminated string overflow can't
     * copy the canary forward intact. */
    uintptr_t canary = 0;
    if (getentropy(&canary, sizeof canary) == 0 && canary != 0)
        __stack_chk_guard = canary & ~(uintptr_t)0xFF;

    /* Pick the widest string/memory scanners this CPU supports before the
     * first strdup() below reaches for strlen()/memcpy(). Skipping this only
     * costs the AVX2 path — the SSE2 default is always correct. */
    extern void __libc_simd_init(void);
    __libc_simd_init();

    if (envp) {
        int i = 0;
        while (envp[i] && i < MAX_ENV_VARS) {
            g_env_table[i] = strdup(envp[i]);
            i++;
        }
        g_env_table[i] = NULL;
        environ = g_env_table;
    }
}

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    fprintf(stderr, "Assertion failed: %s (%s: %s: %d)\n", expr, file, func ? func : "unknown", line);
    abort();
}

/* ── Built-in locale table ─────────────────────────────────────────────────
 *
 * Each entry describes the formatting rules for one named locale.  Only the
 * fields that differ between locales need to be non-empty; the rest default
 * to the POSIX C-locale values.
 *
 * Supported names:
 *   "C", "POSIX"            — ISO C locale (decimal point = ".", no grouping)
 *   "en_US", "en_US.UTF-8"  — US English   (decimal ".", thousands ",")
 *   "de_DE", "de_DE.UTF-8"  — German       (decimal ",", thousands ".")
 *   "fr_FR", "fr_FR.UTF-8"  — French       (decimal ",", thousands "\xc2\xa0" NBSP)
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    const char *names[4];   /* NULL-terminated list of alternate names */
    /* numeric */
    const char *decimal_point;
    const char *thousands_sep;
    const char *grouping;
    /* monetary */
    const char *int_curr_symbol;
    const char *currency_symbol;
    const char *mon_decimal_point;
    const char *mon_thousands_sep;
    const char *mon_grouping;
    const char *positive_sign;
    const char *negative_sign;
    char        frac_digits;
    char        int_frac_digits;
    char        p_cs_precedes;
    char        p_sep_by_space;
    char        n_cs_precedes;
    char        n_sep_by_space;
    char        p_sign_posn;
    char        n_sign_posn;
} locale_entry_t;

#define CHAR_MAX_VAL 127

static const locale_entry_t s_locales[] = {
    /* C / POSIX */
    {
        .names           = { "C", "POSIX", "C.UTF-8", NULL },
        .decimal_point   = ".",  .thousands_sep   = "",    .grouping        = "",
        .int_curr_symbol = "",   .currency_symbol = "",
        .mon_decimal_point = "", .mon_thousands_sep = "",  .mon_grouping    = "",
        .positive_sign   = "",   .negative_sign   = "",
        .frac_digits = CHAR_MAX_VAL, .int_frac_digits = CHAR_MAX_VAL,
        .p_cs_precedes = CHAR_MAX_VAL, .p_sep_by_space = CHAR_MAX_VAL,
        .n_cs_precedes = CHAR_MAX_VAL, .n_sep_by_space = CHAR_MAX_VAL,
        .p_sign_posn = CHAR_MAX_VAL, .n_sign_posn = CHAR_MAX_VAL,
    },
    /* en_US */
    {
        .names             = { "en_US", "en_US.UTF-8", "en_US.utf8", NULL },
        .decimal_point     = ".",    .thousands_sep   = ",",  .grouping = "\3",
        .int_curr_symbol   = "USD ", .currency_symbol = "$",
        .mon_decimal_point = ".",    .mon_thousands_sep = ",", .mon_grouping = "\3",
        .positive_sign     = "",     .negative_sign   = "-",
        .frac_digits = 2, .int_frac_digits = 2,
        .p_cs_precedes = 1, .p_sep_by_space = 0,
        .n_cs_precedes = 1, .n_sep_by_space = 0,
        .p_sign_posn = 1, .n_sign_posn = 1,
    },
    /* de_DE */
    {
        .names             = { "de_DE", "de_DE.UTF-8", "de_DE.utf8", NULL },
        .decimal_point     = ",",    .thousands_sep   = ".", .grouping = "\3",
        .int_curr_symbol   = "EUR ", .currency_symbol = "\xe2\x82\xac", /* UTF-8 € */
        .mon_decimal_point = ",",    .mon_thousands_sep = ".", .mon_grouping = "\3",
        .positive_sign     = "",     .negative_sign   = "-",
        .frac_digits = 2, .int_frac_digits = 2,
        .p_cs_precedes = 0, .p_sep_by_space = 1,
        .n_cs_precedes = 0, .n_sep_by_space = 1,
        .p_sign_posn = 1, .n_sign_posn = 1,
    },
    /* fr_FR */
    {
        .names             = { "fr_FR", "fr_FR.UTF-8", "fr_FR.utf8", NULL },
        .decimal_point     = ",",    .thousands_sep   = "\xc2\xa0", /* NBSP */
        .grouping          = "\3",
        .int_curr_symbol   = "EUR ", .currency_symbol = "\xe2\x82\xac",
        .mon_decimal_point = ",",    .mon_thousands_sep = "\xc2\xa0",
        .mon_grouping      = "\3",
        .positive_sign     = "",     .negative_sign   = "-",
        .frac_digits = 2, .int_frac_digits = 2,
        .p_cs_precedes = 0, .p_sep_by_space = 1,
        .n_cs_precedes = 0, .n_sep_by_space = 1,
        .p_sign_posn = 1, .n_sign_posn = 1,
    },
};
#define LOCALE_COUNT ((int)(sizeof(s_locales) / sizeof(s_locales[0])))

/* Active locale per-category: all start at index 0 (C locale) */
static int s_lc_cat[LC_ALL + 1];  /* indexed by LC_CTYPE .. LC_ALL */

/* lconv active for LC_NUMERIC / LC_MONETARY categories */
static struct lconv s_active_lconv;

/* Opaque locale_t object; used by newlocale/uselocale */
struct _az_locale_obj {
    int locale_idx;    /* index into s_locales[] */
    int category_mask;
};


/* Rebuild s_active_lconv from the current category selections */
static void _rebuild_lconv(void)
{
    int ni = s_lc_cat[LC_NUMERIC];
    int mi = s_lc_cat[LC_MONETARY];
    if (ni < 0 || ni >= LOCALE_COUNT) ni = 0;
    if (mi < 0 || mi >= LOCALE_COUNT) mi = 0;

    const locale_entry_t *n = &s_locales[ni];
    const locale_entry_t *m = &s_locales[mi];

    s_active_lconv.decimal_point       = (char *)n->decimal_point;
    s_active_lconv.thousands_sep       = (char *)n->thousands_sep;
    s_active_lconv.grouping            = (char *)n->grouping;
    s_active_lconv.int_curr_symbol     = (char *)m->int_curr_symbol;
    s_active_lconv.currency_symbol     = (char *)m->currency_symbol;
    s_active_lconv.mon_decimal_point   = (char *)m->mon_decimal_point;
    s_active_lconv.mon_thousands_sep   = (char *)m->mon_thousands_sep;
    s_active_lconv.mon_grouping        = (char *)m->mon_grouping;
    s_active_lconv.positive_sign       = (char *)m->positive_sign;
    s_active_lconv.negative_sign       = (char *)m->negative_sign;
    s_active_lconv.int_frac_digits     = m->int_frac_digits;
    s_active_lconv.frac_digits         = m->frac_digits;
    s_active_lconv.p_cs_precedes       = m->p_cs_precedes;
    s_active_lconv.p_sep_by_space      = m->p_sep_by_space;
    s_active_lconv.n_cs_precedes       = m->n_cs_precedes;
    s_active_lconv.n_sep_by_space      = m->n_sep_by_space;
    s_active_lconv.p_sign_posn         = m->p_sign_posn;
    s_active_lconv.n_sign_posn         = m->n_sign_posn;
    /* POSIX.1-2008 int_* fields from monetary locale */
    s_active_lconv.int_p_cs_precedes   = m->p_cs_precedes;
    s_active_lconv.int_n_cs_precedes   = m->n_cs_precedes;
    s_active_lconv.int_p_sep_by_space  = m->p_sep_by_space;
    s_active_lconv.int_n_sep_by_space  = m->n_sep_by_space;
    s_active_lconv.int_p_sign_posn     = m->p_sign_posn;
    s_active_lconv.int_n_sign_posn     = m->n_sign_posn;
}

/* Look up locale name in the table; returns index, or -1 if not found */
static int _find_locale(const char *name)
{
    if (!name || !*name) return 0;  /* empty string → "C" */
    for (int i = 0; i < LOCALE_COUNT; i++) {
        for (int j = 0; s_locales[i].names[j]; j++) {
            if (strcmp(s_locales[i].names[j], name) == 0) return i;
        }
    }
    return -1;  /* not found */
}

char *setlocale(int category, const char *locale)
{
    /* Query mode: locale == NULL → return current name */
    if (!locale) {
        int idx = 0;
        if (category >= LC_CTYPE && category <= LC_MESSAGES)
            idx = s_lc_cat[category];
        else if (category == LC_ALL)
            idx = s_lc_cat[LC_ALL];
        if (idx < 0 || idx >= LOCALE_COUNT) idx = 0;
        return (char *)s_locales[idx].names[0];
    }

    int new_idx = _find_locale(locale);
    if (new_idx < 0) return NULL;  /* unsupported locale → error */

    if (category == LC_ALL) {
        for (int c = LC_CTYPE; c <= LC_MESSAGES; c++)
            s_lc_cat[c] = new_idx;
        s_lc_cat[LC_ALL] = new_idx;
    } else if (category >= LC_CTYPE && category <= LC_MESSAGES) {
        s_lc_cat[category] = new_idx;
        /* Recompute LC_ALL: pick LC_ALL_IDX only if all cats agree */
        int all_same = s_lc_cat[LC_CTYPE];
        for (int c = LC_NUMERIC; c <= LC_MESSAGES; c++)
            if (s_lc_cat[c] != all_same) { all_same = -1; break; }
        s_lc_cat[LC_ALL] = (all_same >= 0) ? all_same : 0;
    } else {
        return NULL;
    }

    _rebuild_lconv();
    return (char *)s_locales[new_idx].names[0];
}

struct lconv *localeconv(void)
{
    /* Ensure initialised (first call before any setlocale) */
    static int s_inited = 0;
    if (!s_inited) { _rebuild_lconv(); s_inited = 1; }
    return &s_active_lconv;
}

/* ── POSIX.1-2008 locale_t API ────────────────────────────────────────────── */

locale_t newlocale(int category_mask, const char *locale, locale_t base)
{
    int idx = _find_locale(locale);
    if (idx < 0) return (locale_t)0;  /* NULL = error */

    struct _az_locale_obj *obj;
    if (base && base != LC_GLOBAL_LOCALE) {
        obj = base;
        obj->category_mask |= category_mask;
    } else {
        obj = (struct _az_locale_obj *)malloc(sizeof(struct _az_locale_obj));
        if (!obj) return (locale_t)0;
        obj->category_mask = category_mask;
    }
    /* The new locale overrides the requested categories */
    if (category_mask) obj->locale_idx = idx;
    return obj;
}

locale_t uselocale(locale_t newloc)
{
    /* We use a single global; per-thread locale_t is a future extension */
    static locale_t s_current = (locale_t)0;
    locale_t old = s_current ? s_current : LC_GLOBAL_LOCALE;

    if (newloc && newloc != LC_GLOBAL_LOCALE) {
        s_current = newloc;
        /* Apply to the global setlocale state */
        if (newloc->locale_idx >= 0 && newloc->locale_idx < LOCALE_COUNT) {
            setlocale(LC_ALL, s_locales[newloc->locale_idx].names[0]);
        }
    } else if (newloc == LC_GLOBAL_LOCALE) {
        s_current = (locale_t)0;
    }
    return old;
}

locale_t duplocale(locale_t locobj)
{
    if (!locobj) return (locale_t)0;
    struct _az_locale_obj *obj = (struct _az_locale_obj *)malloc(
        sizeof(struct _az_locale_obj));
    if (!obj) return (locale_t)0;
    if (locobj == LC_GLOBAL_LOCALE) {
        obj->locale_idx    = s_lc_cat[LC_ALL];
        obj->category_mask = LC_ALL_MASK;
    } else {
        *obj = *locobj;
    }
    return obj;
}

void freelocale(locale_t locobj)
{
    if (!locobj || locobj == LC_GLOBAL_LOCALE) return;
    free(locobj);
}


char *realpath(const char *path, char *resolved_path)
{
    if (!path || !*path) {
        errno = ENOENT;
        return NULL;
    }

    int allocated = 0;
    if (!resolved_path) {
        resolved_path = (char *)malloc(PATH_MAX);
        if (!resolved_path) {
            errno = ENOMEM;
            return NULL;
        }
        allocated = 1;
    }

    char work[PATH_MAX];
    char link_buf[PATH_MAX];
    int symlink_depth = 0;

    if (path[0] == '/') {
        if (strlen(path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        strcpy(work, path);
    } else {
        if (!getcwd(work, sizeof(work))) {
            strcpy(work, "/");
        }
        size_t clen = strlen(work);
        if (clen == 0 || work[clen - 1] != '/') {
            if (clen + 1 >= PATH_MAX) { errno = ENAMETOOLONG; goto fail; }
            work[clen++] = '/';
            work[clen] = '\0';
        }
        if (clen + strlen(path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        strcat(work, path);
    }

    resolved_path[0] = '/';
    resolved_path[1] = '\0';
    size_t out_len = 1;

    char *p = work;
    while (*p == '/') p++;

    while (*p) {
        char *end = strchr(p, '/');
        size_t seg_len = end ? (size_t)(end - p) : strlen(p);
        char segment[256];
        if (seg_len >= sizeof(segment)) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        memcpy(segment, p, seg_len);
        segment[seg_len] = '\0';
        p = end ? end : p + seg_len;
        while (*p == '/') p++;

        if (strcmp(segment, ".") == 0 || seg_len == 0) {
            continue;
        }
        if (strcmp(segment, "..") == 0) {
            if (out_len > 1) {
                while (out_len > 1 && resolved_path[out_len - 1] != '/') {
                    out_len--;
                }
                if (out_len > 1) {
                    out_len--;
                }
                resolved_path[out_len] = '\0';
            }
            continue;
        }

        size_t prev_out_len = out_len;
        if (out_len > 1) {
            if (out_len + 1 >= PATH_MAX) { errno = ENAMETOOLONG; goto fail; }
            resolved_path[out_len++] = '/';
        }
        if (out_len + seg_len >= PATH_MAX) {
            errno = ENAMETOOLONG;
            goto fail;
        }
        memcpy(resolved_path + out_len, segment, seg_len);
        out_len += seg_len;
        resolved_path[out_len] = '\0';

        struct stat st;
        if (lstat(resolved_path, &st) < 0) {
            goto fail;
        }

        if (S_ISLNK(st.st_mode)) {
            if (++symlink_depth > 32) {
                errno = ELOOP;
                goto fail;
            }
            ssize_t llen = readlink(resolved_path, link_buf, sizeof(link_buf) - 1);
            if (llen < 0) goto fail;
            link_buf[llen] = '\0';

            out_len = prev_out_len;
            resolved_path[out_len] = '\0';

            if (*p) {
                size_t rem_len = strlen(p);
                if ((size_t)llen + 1 + rem_len >= PATH_MAX) {
                    errno = ENAMETOOLONG;
                    goto fail;
                }
                if (link_buf[llen - 1] != '/') link_buf[llen++] = '/';
                strcpy(link_buf + llen, p);
            }

            if (link_buf[0] == '/') {
                resolved_path[0] = '/';
                resolved_path[1] = '\0';
                out_len = 1;
            }

            strcpy(work, link_buf);
            p = work;
            while (*p == '/') p++;
        } else if (*p != '\0' && !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            goto fail;
        }
    }

    if (out_len == 0) {
        resolved_path[0] = '/';
        resolved_path[1] = '\0';
    }
    return resolved_path;

fail:
    if (allocated) free(resolved_path);
    return NULL;
}

char *canonicalize_file_name(const char *path)
{
    return realpath(path, NULL);
}

int mkostemps(char *template, int suffixlen, int flags)
{
    if (!template || suffixlen < 0) { errno = EINVAL; return -1; }
    size_t len = strlen(template);
    if (len < (size_t)suffixlen + 6) { errno = EINVAL; return -1; }
    char *x_start = template + len - suffixlen - 6;
    if (memcmp(x_start, "XXXXXX", 6) != 0) { errno = EINVAL; return -1; }

    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int tries = 0; tries < 100; tries++) {
        unsigned int rnd = arc4random();
        for (int i = 0; i < 6; i++) {
            x_start[i] = chars[rnd % 62];
            rnd /= 62;
            if (i == 2) rnd = arc4random();
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL | flags, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) return -1;
    }
    return -1;
}

int mkostemp(char *template, int flags)
{
    return mkostemps(template, 0, flags);
}

int mkstemps(char *template, int suffixlen)
{
    return mkostemps(template, suffixlen, 0);
}

int mkstemp(char *template)
{
    return mkostemps(template, 0, 0);
}

char *mktemp(char *template)
{
    if (!template) return NULL;
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) return template;

    static unsigned long s_counter = 54321;
    s_counter += (unsigned long)getpid() + 31;
    snprintf(template + len - 6, 7, "%06lx", s_counter % 1000000);
    return template;
}

char *mkdtemp(char *template)
{
    if (!template) return NULL;
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) { errno = EINVAL; return NULL; }

    static unsigned long s_counter = 77777;
    s_counter += (unsigned long)getpid() + 19;
    snprintf(template + len - 6, 7, "%06lx", s_counter % 1000000);

    if (mkdir(template, 0700) < 0) return NULL;
    return template;
}

/* ── Radix-64 Conversion (POSIX.1-2001 XSI) ──────────────────────────────── */

static const char s_b64_table[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

char *l64a(long value)
{
    static char buf[7];
    if (value == 0) {
        buf[0] = '\0';
        return buf;
    }
    uint32_t v = (uint32_t)value;
    int idx = 0;
    while (v != 0 && idx < 6) {
        buf[idx++] = s_b64_table[v & 0x3F];
        v >>= 6;
    }
    buf[idx] = '\0';
    return buf;
}

long a64l(const char *s)
{
    if (!s || !*s) return 0L;
    uint32_t result = 0;
    int shift = 0;
    for (int i = 0; s[i] && i < 6; i++) {
        char c = s[i];
        int val;
        if (c == '.') val = 0;
        else if (c == '/') val = 1;
        else if (c >= '0' && c <= '9') val = c - '0' + 2;
        else if (c >= 'A' && c <= 'Z') val = c - 'A' + 12;
        else if (c >= 'a' && c <= 'z') val = c - 'a' + 38;
        else break;
        result |= ((uint32_t)val << shift);
        shift += 6;
    }
    return (long)(int32_t)result;
}

/* ── User Interaction and Status ─────────────────────────────────────────── */

int rpmatch(const char *response)
{
    if (!response) return -1;
    while (*response == ' ' || *response == '\t') response++;
    if (*response == 'y' || *response == 'Y') return 1;
    if (*response == 'n' || *response == 'N') return 0;
    return -1;
}

int getloadavg(double loadavg[], int nelem)
{
    if (!loadavg || nelem <= 0) return 0;
    int fd = open("/proc/loadavg", O_RDONLY);
    if (fd < 0) return -1;

    char buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    int count = 0;
    char *p = buf;
    for (int i = 0; i < nelem && i < 3; i++) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n') break;
        char *end;
        loadavg[i] = strtod(p, &end);
        if (end == p) break;
        p = end;
        count++;
    }
    return count;
}

/* ── Multibyte Character Handling ────────────────────────────────────────── */

int mblen(const char *s, size_t n)
{
    if (!s) return 0;
    if (n == 0) return -1;
    if (*s == '\0') return 0;

    unsigned char c = (unsigned char)*s;
    if (c < 0x80) return 1;

    if ((c & 0xE0) == 0xC0) {
        if (n < 2) { errno = EILSEQ; return -1; }
        if (c < 0xC2 || ((unsigned char)s[1] & 0xC0) != 0x80) {
            errno = EILSEQ;
            return -1;
        }
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        if (n < 3) { errno = EILSEQ; return -1; }
        if (((unsigned char)s[1] & 0xC0) != 0x80 || ((unsigned char)s[2] & 0xC0) != 0x80) {
            errno = EILSEQ;
            return -1;
        }
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        if (n < 4) { errno = EILSEQ; return -1; }
        if (((unsigned char)s[1] & 0xC0) != 0x80 ||
            ((unsigned char)s[2] & 0xC0) != 0x80 ||
            ((unsigned char)s[3] & 0xC0) != 0x80) {
            errno = EILSEQ;
            return -1;
        }
        return 4;
    }

    errno = EILSEQ;
    return -1;
}

int clearenv(void)
{
    if (environ) {
        environ[0] = NULL;
    }
    return 0;
}

int getsubopt(char **optionp, char * const *tokens, char **valuep)
{
    if (!optionp || !*optionp || !tokens || !valuep) return -1;
    char *p = *optionp;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *optionp = p; return -1; }

    char *comma = strchr(p, ',');
    if (comma) {
        *comma = '\0';
        *optionp = comma + 1;
    } else {
        *optionp = p + strlen(p);
    }

    char *equal = strchr(p, '=');
    if (equal) {
        *equal = '\0';
        *valuep = equal + 1;
    } else {
        *valuep = NULL;
    }

    for (int i = 0; tokens[i]; i++) {
        if (strcmp(tokens[i], p) == 0) {
            return i;
        }
    }
    return -1;
}

void abort(void)
{
    _exit(134); /* SIGABRT convention */
}

/* ── GCC Runtime Support ─────────────────────────────────────────────────── */

/* Stack protector canary — glibc initialises this from a random value;
 * for AzamiOS we seed it with a compile-time constant that the kernel
 * overwrites at process start.  A non-zero value is enough to satisfy
 * the linker.  Our crt0 could update it via getrandom() if desired. */
uintptr_t __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;

__attribute__((noreturn))
void __stack_chk_fail(void)
{
    /* Stack smashing detected — terminate immediately */
    _exit(127);
    __builtin_unreachable();
}

/* ── atexit / __cxa_atexit / __cxa_finalize ─────────────────────────────── */

#define ATEXIT_MAX 64

typedef struct {
    void (*fn)(void *);
    void *arg;
    void *dso; /* DSO handle (ignored in static builds) */
} atexit_entry_t;

static atexit_entry_t g_atexit_table[ATEXIT_MAX];
static int            g_atexit_count = 0;

/* __cxa_atexit — called by global C++ destructors */
int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    if (g_atexit_count >= ATEXIT_MAX) return -1;
    g_atexit_table[g_atexit_count].fn  = fn;
    g_atexit_table[g_atexit_count].arg = arg;
    g_atexit_table[g_atexit_count].dso = dso;
    g_atexit_count++;
    return 0;
}

/* __cxa_finalize — run destructors registered for a DSO (or all if dso==NULL) */
void __cxa_finalize(void *dso)
{
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (!g_atexit_table[i].fn) continue;
        if (dso == NULL || g_atexit_table[i].dso == dso) {
            void (*fn)(void *) = g_atexit_table[i].fn;
            void *arg          = g_atexit_table[i].arg;
            g_atexit_table[i].fn = NULL; /* prevent double-call */
            fn(arg);
        }
    }
}

/* atexit — POSIX wrapper: adapts a plain void(*)(void) into __cxa_atexit */
int atexit(void (*fn)(void))
{
    return __cxa_atexit((void (*)(void *))fn, NULL, NULL);
}

/* ── on_exit & quick_exit / at_quick_exit / exit ─────────────────────────── */

#define ON_EXIT_MAX 32
typedef struct {
    void (*fn)(int, void *);
    void *arg;
} on_exit_entry_t;

static on_exit_entry_t g_on_exit_table[ON_EXIT_MAX];
static int            g_on_exit_count = 0;

int on_exit(void (*fn)(int, void *), void *arg)
{
    if (!fn || g_on_exit_count >= ON_EXIT_MAX) return -1;
    g_on_exit_table[g_on_exit_count].fn  = fn;
    g_on_exit_table[g_on_exit_count].arg = arg;
    g_on_exit_count++;
    return 0;
}

#define QUICK_EXIT_MAX 32
static void (*g_quick_exit_table[QUICK_EXIT_MAX])(void);
static int  g_quick_exit_count = 0;

int at_quick_exit(void (*fn)(void))
{
    if (!fn || g_quick_exit_count >= QUICK_EXIT_MAX) return -1;
    g_quick_exit_table[g_quick_exit_count++] = fn;
    return 0;
}

void _Exit(int status)
{
    _exit(status);
}

void quick_exit(int status)
{
    for (int i = g_quick_exit_count - 1; i >= 0; i--) {
        if (g_quick_exit_table[i]) {
            void (*fn)(void) = g_quick_exit_table[i];
            g_quick_exit_table[i] = NULL;
            fn();
        }
    }
    _Exit(status);
}

void exit(int status)
{
    /* 1. Run atexit & C++ destructors registered via __cxa_atexit */
    __cxa_finalize(NULL);

    /* 2. Run on_exit callbacks in reverse registration order */
    for (int i = g_on_exit_count - 1; i >= 0; i--) {
        if (g_on_exit_table[i].fn) {
            void (*fn)(int, void *) = g_on_exit_table[i].fn;
            void *arg               = g_on_exit_table[i].arg;
            g_on_exit_table[i].fn = NULL;
            fn(status, arg);
        }
    }

    /* 3. Flush open stdio streams */
    fflush(NULL);

    /* 4. Terminate process */
    _exit(status);
}

