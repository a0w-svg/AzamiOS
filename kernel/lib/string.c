/* ============================================================================
 * AzamiOS — Freestanding String & Memory Utility Implementation
 * File: kernel/lib/string.c
 * ============================================================================ */

#include "string.h"
#include "../mm/kmalloc.h"

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    u64 c8 = (u8)c;
    u64 c64 = c8 * 0x0101010101010101UL;

    size_t qwords = n >> 3;
    size_t bytes  = n & 7;

    if (qwords > 0) {
        __asm__ volatile(
            "rep stosq"
            : "+D"(d), "+c"(qwords)
            : "a"(c64)
            : "memory"
        );
    }
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

    size_t qwords = n >> 3;
    size_t bytes  = n & 7;

    if (qwords > 0) {
        __asm__ volatile(
            "rep movsq"
            : "+D"(d), "+S"(s), "+c"(qwords)
            :
            : "memory"
        );
    }
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
    } else {
        d += n - 1;
        s += n - 1;
        __asm__ volatile(
            "std\n\t"
            "rep movsb\n\t"
            "cld"
            : "+D"(d), "+S"(s), "+c"(n)
            :
            : "memory"
        );
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
