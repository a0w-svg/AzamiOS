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

static void malloc_lock(void)
{
    int spin = 0;
    while (__sync_lock_test_and_set(&g_malloc_lock, 1)) {
        if (++spin < 100) {
            __asm__ volatile("pause");
        } else {
            syscall0(SYS_AZ_YIELD);
            spin = 0;
        }
    }
}

static void malloc_unlock(void)
{
    __sync_lock_release(&g_malloc_lock);
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

void __libc_init(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv;

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
    if (!path) return NULL;
    if (!resolved_path) {
        resolved_path = (char *)malloc(512);
        if (!resolved_path) return NULL;
    }

    if (path[0] == '/') {
        strncpy(resolved_path, path, 511);
        resolved_path[511] = '\0';
    } else {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "/");
        if (cwd[strlen(cwd) - 1] != '/') strcat(cwd, "/");
        snprintf(resolved_path, 512, "%s%s", cwd, path);
    }
    return resolved_path;
}

int mkstemp(char *template)
{
    if (!template) return -1;
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) return -1;

    static unsigned long s_counter = 12345;
    s_counter += (unsigned long)getpid() + 17;
    snprintf(template + len - 6, 7, "%06lx", s_counter % 1000000);

    return open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
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

void exit(int status)
{
    _exit(status);
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

