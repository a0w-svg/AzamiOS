/**
 * stdlib.c — AzamiOS libc: complete standard utility library
 */
#include "include/stdlib.h"
#include "include/string.h"
#include <unistd.h>
#include <limits.h>

/* ── Numeric conversion ─────────────────────────────────────────── */

static void reverse(char *str, int length) {
    int start = 0;
    int end = length - 1;
    while (start < end) {
        char temp = str[start];
        str[start] = str[end];
        str[end] = temp;
        start++;
        end--;
    }
}

char *itoa(int value, char *str, int base) {
    int i = 0;
    int isNegative = 0;

    if (base < 2 || base > 36) {
        str[0] = '\0';
        return str;
    }

    if (value == 0) {
        str[i++] = '0';
        str[i] = '\0';
        return str;
    }

    unsigned int unum = (unsigned int)value;

    if (value < 0 && base == 10) {
        isNegative = 1;
        unum = (unsigned int)(-value);
    }

    while (unum != 0) {
        int rem = (int)(unum % (unsigned int)base);
        str[i++] = (rem > 9) ? (char)((rem - 10) + 'a') : (char)(rem + '0');
        unum = unum / (unsigned int)base;
    }

    if (isNegative) {
        str[i++] = '-';
    }

    str[i] = '\0';
    reverse(str, i);
    return str;
}

int atoi(const char *str) {
    return (int)strtol(str, (char **)0, 10);
}

long atol(const char *str) {
    return strtol(str, (char **)0, 10);
}

double atof(const char *str) {
    return strtod(str, (char **)0);
}

double strtod(const char *s, char **endptr) {
    while (*s == ' ' || *s == '\t') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    double val = 0.0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
    }

    if (*s == '.') {
        s++;
        double power = 10.0;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') / power;
            power *= 10.0;
            s++;
        }
    }

    if (endptr) *endptr = (char *)s;
    return sign * val;
}

/* ── Integer maths ──────────────────────────────────────────────── */

int abs(int n) {
    return (n < 0) ? -n : n;
}

long labs(long n) {
    return (n < 0) ? -n : n;
}

div_t div(int num, int den) {
    div_t res;
    res.quot = num / den;
    res.rem  = num % den;
    return res;
}

ldiv_t ldiv(long num, long den) {
    ldiv_t res;
    res.quot = num / den;
    res.rem  = num % den;
    return res;
}

/* ── Random numbers ─────────────────────────────────────────────── */

static unsigned long next_seed = 1;

int rand(void) {
    next_seed = next_seed * 1103515245 + 12345;
    return (unsigned int)(next_seed / 65536) % 32768;
}

void srand(unsigned int seed) {
    next_seed = seed;
}

/* ── Memory management (Free-list allocator backed by sbrk) ─────── */

typedef struct header {
    struct header *ptr;
    size_t size;
} header_t;

static header_t base;
static header_t *freep = 0;

static void free_impl(void *ap) {
    header_t *bp, *p;
    bp = (header_t *)ap - 1;
    for (p = freep; !(bp > p && bp < p->ptr); p = p->ptr) {
        if (p >= p->ptr && (bp > p || bp < p->ptr))
            break;
    }
    if (bp + bp->size == p->ptr) {
        bp->size += p->ptr->size;
        bp->ptr = p->ptr->ptr;
    } else {
        bp->ptr = p->ptr;
    }
    if (p + p->size == bp) {
        p->size += bp->size;
        p->ptr = bp->ptr;
    } else {
        p->ptr = bp;
    }
    freep = p;
}

static header_t *morecore(size_t nu) {
    char *cp;
    header_t *up;
    if (nu < 1024)
        nu = 1024;
    cp = (char *)sbrk((int)(nu * sizeof(header_t)));
    if (cp == (char *) -1)
        return 0;
    up = (header_t *)cp;
    up->size = nu;
    free_impl((void *)(up + 1));
    return freep;
}

void *malloc(size_t size) {
    header_t *p, *prevp;
    size_t nunits;
    if (size == 0) return 0;
    nunits = (size + sizeof(header_t) - 1) / sizeof(header_t) + 1;
    if ((prevp = freep) == 0) {
        base.ptr = freep = prevp = &base;
        base.size = 0;
    }
    for (p = prevp->ptr; ; prevp = p, p = p->ptr) {
        if (p->size >= nunits) {
            if (p->size == nunits)
                prevp->ptr = p->ptr;
            else {
                p->size -= nunits;
                p += p->size;
                p->size = nunits;
            }
            freep = prevp;
            return (void *)(p + 1);
        }
        if (p == freep)
            if ((p = morecore(nunits)) == 0)
                return 0;
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }
    
    header_t *bp = (header_t *)ptr - 1;
    size_t old_size = (bp->size - 1) * sizeof(header_t);
    if (old_size >= size) return ptr;
    
    void *new_ptr = malloc(size);
    if (!new_ptr) return 0;
    memcpy(new_ptr, ptr, old_size);
    free(ptr);
    return new_ptr;
}

void free(void *ptr) {
    if (!ptr) return;
    free_impl(ptr);
}

/* ── Process control ────────────────────────────────────────────── */

void abort(void) {
    _exit(134);
}

/* exit and exec are already implemented or declared */

/* ── Environment ────────────────────────────────────────────────── */

extern char **environ;

char *getenv(const char *name) {
    if (!environ || !name) return 0;
    size_t len = strlen(name);
    for (char **env = environ; *env; env++) {
        if (strncmp(*env, name, len) == 0 && (*env)[len] == '=') {
            return &(*env)[len + 1];
        }
    }
    return 0;
}

/* ── Sorting & searching ────────────────────────────────────────── */

static void swap(char *a, char *b, size_t size) {
    while (size--) {
        char tmp = *a;
        *a++ = *b;
        *b++ = tmp;
    }
}

void qsort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    if (nmemb < 2 || !base || !cmp) return;
    char *arr = (char *)base;
    char *pivot = arr + (nmemb / 2) * size;
    size_t i = 0, j = nmemb - 1;
    
    while (1) {
        while (cmp(arr + i * size, pivot) < 0) i++;
        while (cmp(arr + j * size, pivot) > 0) j--;
        if (i >= j) break;
        swap(arr + i * size, arr + j * size, size);
        if (pivot == arr + i * size) pivot = arr + j * size;
        else if (pivot == arr + j * size) pivot = arr + i * size;
        i++;
        if (j > 0) j--;
    }
    
    if (i > 1) qsort(arr, i, size, cmp);
    if (nmemb - i - 1 > 1) qsort(arr + (i + 1) * size, nmemb - i - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    const char *arr = (const char *)base;
    size_t low = 0, high = nmemb;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int res = cmp(key, arr + mid * size);
        if (res < 0) high = mid;
        else if (res > 0) low = mid + 1;
        else return (void *)(arr + mid * size);
    }
    return 0;
}