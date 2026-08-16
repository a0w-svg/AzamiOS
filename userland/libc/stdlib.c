/* ============================================================================
 * AzamiOS Userspace — Standard Library Implementation (POSIX-compatible)
 * File: userland/libc/stdlib.c
 * ============================================================================ */

#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/ctype.h"
#include "include/unistd.h"

/* ── Free-List Memory Allocator ──────────────────────────────────────────── */

typedef struct block_header {
    size_t size; /* User payload size */
    int is_free;
    struct block_header *next;
    struct block_header *prev;
    size_t magic;
} block_header_t;

#define BLOCK_MAGIC 0x415A414D494D414CUL

static block_header_t *g_block_head = NULL;
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
        g_heap_top = (void *)base;
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

    if (!g_block_head) {
        g_block_head = new_block;
    } else {
        block_header_t *tail = g_block_head;
        while (tail->next) tail = tail->next;
        tail->next = new_block;
        new_block->prev = tail;
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

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    block_header_t *hdr = ((block_header_t *)ptr) - 1;
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

void free(void *ptr)
{
    if (!ptr) return;
    block_header_t *hdr = ((block_header_t *)ptr) - 1;
    if (hdr->magic != BLOCK_MAGIC) return;

    hdr->is_free = 1;

    /* Coalesce forward (only if physically contiguous) */
    if (hdr->next && hdr->next->is_free && hdr->next->magic == BLOCK_MAGIC) {
        if ((char *)(hdr + 1) + hdr->size == (char *)hdr->next) {
            hdr->size += sizeof(block_header_t) + hdr->next->size;
            hdr->next = hdr->next->next;
            if (hdr->next) hdr->next->prev = hdr;
        }
    }

    /* Coalesce backward (only if physically contiguous) */
    if (hdr->prev && hdr->prev->is_free && hdr->prev->magic == BLOCK_MAGIC) {
        if ((char *)(hdr->prev + 1) + hdr->prev->size == (char *)hdr) {
            hdr->prev->size += sizeof(block_header_t) + hdr->size;
            hdr->prev->next = hdr->next;
            if (hdr->next) hdr->next->prev = hdr->prev;
        }
    }
}

void *aligned_alloc(size_t alignment, size_t size)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return NULL;
    if (alignment <= 16) return malloc(size);
    void *raw = malloc(size + alignment);
    if (!raw) return NULL;
    unsigned long addr = (unsigned long)raw;
    unsigned long al = (addr + alignment - 1) & ~(alignment - 1);
    return (void *)al;
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
    if (endptr) *endptr = (char *)nptr;
    return sign * val;
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

#define MAX_ENV_VARS 32
static char *g_env_table[MAX_ENV_VARS] = { NULL };

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
            return 0;
        }
    }
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!g_env_table[i]) {
            g_env_table[i] = entry;
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
            g_env_table[i] = NULL;
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

void exit(int status)
{
    _exit(status);
}

void abort(void)
{
    _exit(134); /* SIGABRT convention */
}
