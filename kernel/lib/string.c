/* ============================================================================
 * AzamiOS — Freestanding String & Memory Utility Implementation
 * File: kernel/lib/string.c
 * ============================================================================ */

#include "string.h"
#include "../mm/kmalloc.h"

/* Set by cpu_enable_features_bsp() when the CPU advertises Enhanced REP MOVSB/
 * STOSB (CPUID.7.0:EBX.ERMS). On such parts a single `rep movsb` runs at full
 * cache-line width and beats a `rep movsq` + tail, and it needs no alignment
 * fix-ups; on older parts the qword loop is still the faster of the two, so we
 * keep both and branch once per call. Byte counts too small to amortise the
 * `rep` start-up cost go through a plain loop either way. */
extern u8 g_erms_enabled;

/* Below this, the microcode start-up cost of a REP dominates. */
#define REP_MIN_BYTES  64

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    u64 c8 = (u8)c;

    if (n < REP_MIN_BYTES) {
        for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c8;
        return dest;
    }

    if (g_erms_enabled) {
        __asm__ volatile(
            "rep stosb"
            : "+D"(d), "+c"(n)
            : "a"((u8)c)
            : "memory"
        );
        return dest;
    }

    u64 c64 = c8 * 0x0101010101010101UL;
    size_t qwords = n >> 3;
    size_t bytes  = n & 7;

    __asm__ volatile(
        "rep stosq"
        : "+D"(d), "+c"(qwords)
        : "a"(c64)
        : "memory"
    );
    if (bytes > 0) {
        __asm__ volatile(
            "rep stosb"
            : "+D"(d), "+c"(bytes)
            : "a"((u8)c)
            : "memory"
        );
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    if (n < REP_MIN_BYTES) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
        return dest;
    }

    if (g_erms_enabled) {
        __asm__ volatile(
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
        return dest;
    }

    size_t qwords = n >> 3;
    size_t bytes  = n & 7;

    __asm__ volatile(
        "rep movsq"
        : "+D"(d), "+S"(s), "+c"(qwords)
        :
        : "memory"
    );
    if (bytes > 0) {
        __asm__ volatile(
            "rep movsb"
            : "+D"(d), "+S"(s), "+c"(bytes)
            :
            : "memory"
        );
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dest;
    if (d < s || d >= s + n) {
        return memcpy(dest, src, n);
    }

    /* Overlapping, destination above source: copy downwards.
     *
     * Deliberately a hand-rolled backward loop rather than `std; rep movsb; cld`.
     * DF=1 is a global CPU flag, and the System V ABI (which every C function
     * the compiler emits assumes) requires DF=0 on entry — so any interrupt
     * taken inside that window would run its handler with the direction flag
     * inverted. The ISR stubs do `cld` on entry, but NMI and #MC do not go
     * through them, and the cost of getting this wrong is silent memory
     * corruption. Copying qwords first keeps it comparable in speed. */
    size_t i = n;
    while (i >= 8) {
        i -= 8;
        *(u64 *)(d + i) = *(const u64 *)(s + i);
    }
    while (i > 0) {
        i--;
        d[i] = s[i];
    }
    return dest;
}

int memcmp(const void *ptr1, const void *ptr2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)ptr1;
    const unsigned char *p2 = (const unsigned char *)ptr2;

    while (n >= 8) {
        u64 v1 = *(const u64 *)p1;
        u64 v2 = *(const u64 *)p2;
        if (v1 != v2) {
            break;
        }
        p1 += 8;
        p2 += 8;
        n -= 8;
    }

    while (n--) {
        if (*p1 != *p2) {
            return (int)*p1 - (int)*p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char *str)
{
    if (!str) return 0;
    const char *s = str;
    while ((uintptr_t)s & 7) {
        if (*s == '\0') return (size_t)(s - str);
        s++;
    }
    const u64 *w = (const u64 *)s;
    for (;;) {
        u64 val = *w;
        if (((val - 0x0101010101010101ULL) & ~val & 0x8080808080808080ULL) != 0) {
            break;
        }
        w++;
    }
    s = (const char *)w;
    while (*s) s++;
    return (size_t)(s - str);
}

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (n > 0 && *src != '\0') {
        *d++ = *src++;
        n--;
    }
    while (n > 0) {
        *d++ = '\0';
        n--;
    }
    return dest;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

static inline char tolower_c(char c)
{
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

int strcasecmp(const char *s1, const char *s2)
{
    while (*s1 && (tolower_c(*s1) == tolower_c(*s2))) {
        s1++;
        s2++;
    }
    return (int)(unsigned char)tolower_c(*s1) - (int)(unsigned char)tolower_c(*s2);
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    if ((char)c == '\0') return (char *)s;
    return NULL;
}

static void buf_putc(char *buf, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) {
        buf[*pos] = c;
    }
    (*pos)++;
}

static void buf_puts(char *buf, size_t size, size_t *pos, const char *s, int prec, u32 width, bool left_align, char pad)
{
    size_t slen = 0;
    while (s[slen]) slen++;
    if (prec >= 0 && (size_t)prec < slen) slen = (size_t)prec;

    size_t pad_count = (width > slen) ? (width - slen) : 0;

    if (!left_align) {
        for (size_t i = 0; i < pad_count; i++) buf_putc(buf, size, pos, pad);
    }

    for (size_t i = 0; i < slen; i++) {
        buf_putc(buf, size, pos, s[i]);
    }

    if (left_align) {
        for (size_t i = 0; i < pad_count; i++) buf_putc(buf, size, pos, ' ');
    }
}

static void buf_u64(char *buf, size_t size, size_t *pos, u64 v, u32 base, bool upper, u32 width, bool left_align, char pad)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char tmp[24];
    int len = 0;
    if (v == 0) tmp[len++] = '0';
    while (v) { tmp[len++] = digits[v % base]; v /= base; }
    int pad_count = (width > (u32)len) ? (int)(width - len) : 0;
    if (!left_align) {
        for (int i = 0; i < pad_count; i++) buf_putc(buf, size, pos, pad);
    }
    for (int i = len - 1; i >= 0; i--) buf_putc(buf, size, pos, tmp[i]);
    if (left_align) {
        for (int i = 0; i < pad_count; i++) buf_putc(buf, size, pos, ' ');
    }
}

static void buf_s64(char *buf, size_t size, size_t *pos, s64 v, u32 width, bool left_align, char pad)
{
    bool neg = false;
    u64 uv;
    if (v < 0) {
        neg = true;
        uv = (u64)(-(v + 1)) + 1;
    } else {
        uv = (u64)v;
    }
    char tmp[24];
    int len = 0;
    if (uv == 0) tmp[len++] = '0';
    while (uv) { tmp[len++] = '0' + (char)(uv % 10); uv /= 10; }
    int total_len = len + (neg ? 1 : 0);
    int pad_count = (width > (u32)total_len) ? (int)(width - total_len) : 0;
    if (pad == '0' && neg) {
        buf_putc(buf, size, pos, '-');
        for (int i = 0; i < pad_count; i++) buf_putc(buf, size, pos, '0');
        for (int i = len - 1; i >= 0; i--) buf_putc(buf, size, pos, tmp[i]);
    } else if (!left_align) {
        for (int i = 0; i < pad_count; i++) buf_putc(buf, size, pos, pad);
        if (neg) buf_putc(buf, size, pos, '-');
        for (int i = len - 1; i >= 0; i--) buf_putc(buf, size, pos, tmp[i]);
    } else {
        if (neg) buf_putc(buf, size, pos, '-');
        for (int i = len - 1; i >= 0; i--) buf_putc(buf, size, pos, tmp[i]);
        for (int i = 0; i < pad_count; i++) buf_putc(buf, size, pos, ' ');
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    size_t pos = 0;
    if (!fmt) return 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            buf_putc(buf, size, &pos, *p);
            continue;
        }
        p++;
        if (!*p) break;

        bool left_align = false;
        bool is_long = false, is_llong = false;
        u32 width = 0;
        int prec = -1;
        char pad = ' ';

        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') {
            if (*p == '-') left_align = true;
            else if (*p == '0') pad = '0';
            p++;
        }
        if (left_align) pad = ' ';

        while (*p >= '0' && *p <= '9') { width = width * 10 + (u32)(*p - '0'); p++; }
        if (*p == '.') {
            p++;
            prec = 0;
            while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
        }
        if (*p == 'l') { is_long = true; p++; }
        if (*p == 'l') { is_llong = true; p++; }
        if (*p == 'z') { is_long = true; p++; }
        if (!*p) break;

        switch (*p) {
        case 'd': case 'i': {
            s64 v = is_llong ? __builtin_va_arg(ap, s64)
                             : (is_long ? __builtin_va_arg(ap, long) : __builtin_va_arg(ap, int));
            buf_s64(buf, size, &pos, v, width, left_align, pad);
            break;
        }
        case 'u': {
            u64 v = is_llong ? __builtin_va_arg(ap, u64)
                             : (is_long ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned int));
            buf_u64(buf, size, &pos, v, 10, false, width, left_align, pad);
            break;
        }
        case 'x': {
            u64 v = is_llong ? __builtin_va_arg(ap, u64)
                             : (is_long ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned int));
            buf_u64(buf, size, &pos, v, 16, false, width, left_align, pad);
            break;
        }
        case 'X': {
            u64 v = is_llong ? __builtin_va_arg(ap, u64)
                             : (is_long ? __builtin_va_arg(ap, unsigned long) : __builtin_va_arg(ap, unsigned int));
            buf_u64(buf, size, &pos, v, 16, true, width, left_align, pad);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)__builtin_va_arg(ap, void *);
            buf_puts(buf, size, &pos, "0x", -1, 0, false, ' ');
            buf_u64(buf, size, &pos, (u64)v, 16, false, 16, false, '0');
            break;
        }
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            buf_puts(buf, size, &pos, s, prec, width, left_align, ' ');
            break;
        }
        case 'c': {
            char c = (char)__builtin_va_arg(ap, int);
            buf_putc(buf, size, &pos, c);
            break;
        }
        case '%':
            buf_putc(buf, size, &pos, '%');
            break;
        default:
            buf_putc(buf, size, &pos, '%');
            buf_putc(buf, size, &pos, *p);
            break;
        }
    }

    if (size > 0) {
        if (pos < size) {
            buf[pos] = '\0';
        } else {
            buf[size - 1] = '\0';
        }
    }
    return (int)pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

/* Truncation-safe variants: the result is what landed in the buffer, so it is
 * always < size and an accumulating caller can never step past the end. */
int vscnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    if (size == 0) return 0;
    int ret = vsnprintf(buf, size, fmt, ap);
    if (ret < 0) return 0;
    return (size_t)ret < size ? ret : (int)(size - 1);
}

int scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vscnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if ((char)c == '\0') return (char *)s;
    return (char *)last;
}

char *strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *dup = (char *)kmalloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len + 1);
    return dup;
}

char *strndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = strnlen(s, n);
    char *dup = (char *)kmalloc(len + 1);
    if (!dup) return NULL;
    memcpy(dup, s, len);
    dup[len] = '\0';
    return dup;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char uc = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == uc) return (void *)(p + i);
    }
    return NULL;
}

size_t strnlen(const char *str, size_t maxlen)
{
    if (!str) return 0;
    size_t len = 0;
    while (len < maxlen && str[len] != '\0') {
        len++;
    }
    return len;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (*d) d++;
    while (n > 0 && *src != '\0') {
        *d++ = *src++;
        n--;
    }
    *d = '\0';
    return dest;
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && (tolower_c(*s1) == tolower_c(*s2))) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return (int)(unsigned char)tolower_c(*s1) - (int)(unsigned char)tolower_c(*s2);
}

char *strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return NULL;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack;
            const char *n = needle;
            while (*h && *n && *h == *n) {
                h++;
                n++;
            }
            if (!*n) return (char *)haystack;
        }
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r) {
            if (*r == *s) return count;
            r++;
        }
        count++;
        s++;
    }
    return count;
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*a == *s) return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

char *strsep(char **stringp, const char *delim)
{
    if (!stringp || !*stringp) return NULL;
    char *start = *stringp;
    char *p = start;
    while (*p) {
        const char *d = delim;
        while (*d) {
            if (*p == *d) {
                *p = '\0';
                *stringp = p + 1;
                return start;
            }
            d++;
        }
        p++;
    }
    *stringp = NULL;
    return start;
}

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    char *s = str ? str : *saveptr;
    if (!s) return NULL;

    /* Skip leading delimiters */
    s += strspn(s, delim);
    if (*s == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    /* Find token end */
    char *token = s;
    s = strpbrk(token, delim);
    if (!s) {
        *saveptr = NULL;
    } else {
        *s = '\0';
        *saveptr = s + 1;
    }
    return token;
}

void memzero_explicit(void *s, size_t count)
{
    memset(s, 0, count);
    __asm__ volatile("" : : "r"(s) : "memory");
}

int crypto_memneq(const void *a, const void *b, size_t size)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    unsigned char res = 0;
    for (size_t i = 0; i < size; i++) {
        res |= (pa[i] ^ pb[i]);
    }
    return res != 0;
}
