/**
 * string.c — AzamiOS libc: complete string & memory implementation
 *
 * Covers all functions declared in include/string.h.
 * No hardware dependencies; compiles -ffreestanding.
 */
#include "include/string.h"
#include "include/stdlib.h"   /* for malloc (strdup) */
#include <stdint.h>

/* ── Memory operations ──────────────────────────────────────────── */

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    if (d <= s || d >= (s + n))
        return memcpy(dest, src, n);
    d += n; s += n;
    while (n--) *--d = *--s;
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n) {
    const uint8_t *p = (const uint8_t *)s;
    while (n--) {
        if (*p == (uint8_t)c) return (void *)p;
        p++;
    }
    return (void *)0;
}

/* ── Measurement ────────────────────────────────────────────────── */

size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

/* ── Comparison ─────────────────────────────────────────────────── */

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    return n ? ((unsigned char)*s1 - (unsigned char)*s2) : 0;
}

int strcasecmp(const char *s1, const char *s2) {
    unsigned char a, b;
    do {
        a = (unsigned char)(*s1 >= 'A' && *s1 <= 'Z' ? *s1 + 32 : *s1);
        b = (unsigned char)(*s2 >= 'A' && *s2 <= 'Z' ? *s2 + 32 : *s2);
        if (a != b) return a - b;
        s1++; s2++;
    } while (a);
    return 0;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    unsigned char a, b;
    while (n--) {
        a = (unsigned char)(*s1 >= 'A' && *s1 <= 'Z' ? *s1 + 32 : *s1);
        b = (unsigned char)(*s2 >= 'A' && *s2 <= 'Z' ? *s2 + 32 : *s2);
        if (!a || a != b) return a - b;
        s1++; s2++;
    }
    return 0;
}

/* ── Copy & concatenation ───────────────────────────────────────── */

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n > 0 && *src) { *d++ = *src++; n--; }
    while (n-- > 0) *d++ = '\0';
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

/* ── Search ─────────────────────────────────────────────────────── */

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    return ((char)c == '\0') ? (char *)s : (char *)0;
}

char *strrchr(const char *s, int c) {
    const char *last = (char *)0;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    return (char)c == '\0' ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        if (*haystack == *needle) {
            const char *h = haystack, *n = needle;
            while (*h && *n && *h == *n) { h++; n++; }
            if (!*n) return (char *)haystack;
        }
    }
    return (char *)0;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) { if (*s == *a) return (char *)s; a++; }
        s++;
    }
    return (char *)0;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (*s) {
        const char *a = accept;
        int found = 0;
        while (*a) { if (*s == *a++) { found = 1; break; } }
        if (!found) break;
        n++; s++;
    }
    return n;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (*s) {
        const char *r = reject;
        while (*r) { if (*s == *r++) goto done; }
        n++; s++;
    }
done:
    return n;
}

/* ── Tokenising ─────────────────────────────────────────────────── */

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *s = str ? str : *saveptr;
    if (!s) return (char *)0;
    /* Skip leading delimiters */
    s += strspn(s, delim);
    if (!*s) { *saveptr = (char *)0; return (char *)0; }
    /* Find end of token */
    char *end = s + strcspn(s, delim);
    if (*end) { *end = '\0'; *saveptr = end + 1; }
    else       *saveptr = (char *)0;
    return s;
}

char *strtok(char *str, const char *delim) {
    static char *saved = (char *)0;
    return strtok_r(str, delim, &saved);
}

/* ── Numeric conversion ─────────────────────────────────────────── */

long strtol(const char *s, char **endptr, int base) {
    while (*s == ' ' || *s == '\t') s++;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else base = 10;
    } else if (base == 16 && *s == '0' && (*(s+1) == 'x' || *(s+1) == 'X'))
        s += 2;
    long val = 0;
    while (*s) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (*s >= 'a' && *s <= 'z') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z') d = *s - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        s++;
    }
    if (endptr) *endptr = (char *)s;
    return neg ? -val : val;
}

unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtol(s, endptr, base);
}

/* ── Error strings ──────────────────────────────────────────────── */

char *strerror(int errnum) {
    switch (errnum) {
    case 0:  return "Success";
    case 1:  return "Operation not permitted";
    case 2:  return "No such file or directory";
    case 4:  return "Interrupted system call";
    case 5:  return "I/O error";
    case 9:  return "Bad file descriptor";
    case 12: return "Out of memory";
    case 13: return "Permission denied";
    case 17: return "File exists";
    case 22: return "Invalid argument";
    case 28: return "No space left on device";
    case 38: return "Function not implemented";
    default: return "Unknown error";
    }
}