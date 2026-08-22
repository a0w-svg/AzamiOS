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

static block_header_t *g_block_head = NULL;
static block_header_t *g_block_tail = NULL; /* BUG-11: tail pointer for O(1) append */
static void *g_heap_top = NULL;

void *malloc(size_t size)
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
                curr->next = split;
                curr->size = aligned_size;
            }
            curr->is_free = 0;
            return (void *)(curr + 1);
        }
        curr = curr->next;
    }

    /* 2. No suitable block found, allocate new memory from OS via sys_brk */
    if (!g_heap_top) {
        long base = syscall1(SYS_brk, 0);
        if (base <= 0) return NULL;
        g_heap_top = (void *)(((unsigned long)base + 15UL) & ~15UL);
    }

    size_t total_alloc = sizeof(block_header_t) + aligned_size;
    long next_brk = (long)g_heap_top + (long)total_alloc;
    long res = syscall1(SYS_brk, next_brk);
    if (res < next_brk) {
        return NULL;
    }

    block_header_t *new_block = (block_header_t *)g_heap_top;
    g_heap_top = (void *)next_brk;

    new_block->size = aligned_size;
    new_block->is_free = 0;
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

    return (void *)(new_block + 1);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (nmemb != 0 && total / nmemb != size) return NULL;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void free(void *ptr);

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }

    /* Check if pointer was allocated via aligned_alloc */
    if (((size_t *)ptr)[-2] == ALIGNED_BLOCK_MAGIC) {
        void *raw = ((void **)ptr)[-1];
        block_header_t *hdr = ((block_header_t *)raw) - 1;
        if (hdr->magic != BLOCK_MAGIC) return NULL;

        size_t old_size = hdr->size;
        if (size <= old_size) {
            return ptr;
        }

        void *new_ptr = malloc(size);
        if (new_ptr) {
            memcpy(new_ptr, ptr, old_size);
            free(ptr);
        }
        return new_ptr;
    }

    block_header_t *hdr = ((block_header_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC) return NULL;

    size_t old_size = hdr->size;
    if (size <= old_size) {
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
            return ptr;
        }
    }

    void *new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, old_size);
        free(ptr);
    }
    return new_ptr;
}

void free(void *ptr)
{
    if (!ptr) return;

    /* Check if pointer was allocated via aligned_alloc */
    if (((size_t *)ptr)[-2] == ALIGNED_BLOCK_MAGIC) {
        void *raw = ((void **)ptr)[-1];
        free(raw);
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
        uval = (unsigned int)(0 - value);
    } else {
        uval = (unsigned int)value;
    }

    low = ptr;
    do {
        int rem = (int)(uval % (unsigned int)base);
        *ptr++ = (char)(rem < 10 ? '0' + rem : 'a' + rem - 10);
        uval /= (unsigned int)base;
    } while (uval);

    *ptr-- = '\0';
    while (low < ptr) {
        char tmp = *low;
        *low++ = *ptr;
        *ptr-- = tmp;
    }
    return rc;
}

/* ── Absolute value and division ─────────────────────────────────────────── */

int abs(int x) { return (x < 0) ? -x : x; }
long labs(long x) { return (x < 0) ? -x : x; }
long long llabs(long long x) { return (x < 0) ? -x : x; }

div_t div(int numer, int denom)
{
    div_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

ldiv_t ldiv(long numer, long denom)
{
    ldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
    return r;
}

lldiv_t lldiv(long long numer, long long denom)
{
    lldiv_t r;
    r.quot = numer / denom;
    r.rem = numer % denom;
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

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb < 2 || size == 0 || !base) return;

    char *b = (char *)base;
    char *pivot = b + (nmemb - 1) * size;
    size_t i = 0;

    for (size_t j = 0; j < nmemb - 1; j++) {
        if (compar(b + j * size, pivot) <= 0) {
            if (i != j) _qsort_swap(b + i * size, b + j * size, size);
            i++;
        }
    }
    _qsort_swap(b + i * size, pivot, size);

    if (i > 1) qsort(b, i, size, compar);
    if (nmemb - i - 1 > 1) qsort(b + (i + 1) * size, nmemb - i - 1, size, compar);
}

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

static struct lconv s_posix_lconv = {
    .decimal_point     = ".",
    .thousands_sep     = "",
    .grouping          = "",
    .int_curr_symbol   = "",
    .currency_symbol   = "",
    .mon_decimal_point = "",
    .mon_thousands_sep = "",
    .mon_grouping      = "",
    .positive_sign     = "",
    .negative_sign     = "",
    .int_frac_digits   = 127,
    .frac_digits       = 127,
    .p_cs_precedes     = 127,
    .p_sep_by_space    = 127,
    .n_cs_precedes     = 127,
    .n_sep_by_space    = 127,
    .p_sign_posn       = 127,
    .n_sign_posn       = 127,
};

char *setlocale(int category, const char *locale)
{
    (void)category;
    static char s_locale_c[] = "C";
    if (!locale || !*locale || strcmp(locale, "C") == 0 || strcmp(locale, "POSIX") == 0) {
        return s_locale_c;
    }
    return s_locale_c;
}

struct lconv *localeconv(void)
{
    return &s_posix_lconv;
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

