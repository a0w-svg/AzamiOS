/* ============================================================================
 * AzamiOS Userspace — String Implementation (POSIX-compatible)
 * File: userland/libc/string.c
 * ============================================================================ */

#include "include/string.h"
#include "include/stdlib.h"
#include "include/ctype.h"
#include "include/stdio.h"

/* ── Lengths ─────────────────────────────────────────────────────────────── */

size_t strlen(const char *s)
{
    size_t len = 0;
    while (s && *s++) len++;
    return len;
}

size_t strnlen(const char *s, size_t maxlen)
{
    size_t len = 0;
    while (len < maxlen && s && s[len]) len++;
    return len;
}

/* ── Copy ────────────────────────────────────────────────────────────────── */

char *strcpy(char *dest, const char *src)
{
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    for ( ; i < n; i++) dest[i] = '\0';
    return dest;
}

char *stpcpy(char *dest, const char *src)
{
    while ((*dest++ = *src++) != '\0');
    return dest - 1;
}

char *stpncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) dest[i] = src[i];
    char *ret = dest + i;
    for ( ; i < n; i++) dest[i] = '\0';
    return ret;
}

/* ── Concatenation ───────────────────────────────────────────────────────── */

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
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

/* ── Comparison ──────────────────────────────────────────────────────────── */

int strcmp(const char *s1, const char *s2)
{
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) return 0;
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    while (n > 1 && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strcasecmp(const char *s1, const char *s2)
{
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    while (*s1 && (tolower((unsigned char)*s1) == tolower((unsigned char)*s2)))
        { s1++; s2++; }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

int strncasecmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) return 0;
    if (!s1 || !s2) { if (s1 == s2) return 0; return s1 ? 1 : -1; }
    while (n > 1 && *s1 &&
           (tolower((unsigned char)*s1) == tolower((unsigned char)*s2)))
        { s1++; s2++; n--; }
    return tolower((unsigned char)*s1) - tolower((unsigned char)*s2);
}

/* ── Search ──────────────────────────────────────────────────────────────── */

char *strchr(const char *s, int c)
{
    if (!s) return 0;
    char ch = (char)c;
    while (*s) { if (*s == ch) return (char *)s; s++; }
    return (ch == '\0') ? (char *)s : 0;
}

char *strrchr(const char *s, int c)
{
    if (!s) return 0;
    char ch = (char)c;
    const char *last = 0;
    while (*s) { if (*s == ch) last = s; s++; }
    return (ch == '\0') ? (char *)s : (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (*h == *n)) { h++; n++; }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

char *strcasestr(const char *haystack, const char *needle)
{
    if (!haystack || !needle) return 0;
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
    }
    return 0;
}

size_t strspn(const char *s, const char *accept)
{
    const char *p = s;
    while (*p) {
        const char *a = accept;
        while (*a && *a != *p) a++;
        if (!*a) break;
        p++;
    }
    return (size_t)(p - s);
}

size_t strcspn(const char *s, const char *reject)
{
    const char *p = s;
    while (*p) {
        const char *r = reject;
        while (*r) { if (*r == *p) return (size_t)(p - s); r++; }
        p++;
    }
    return (size_t)(p - s);
}

char *strpbrk(const char *s, const char *accept)
{
    while (*s) {
        const char *a = accept;
        while (*a) { if (*a == *s) return (char *)s; a++; }
        s++;
    }
    return 0;
}

/* ── Tokenise ────────────────────────────────────────────────────────────── */

char *strtok_r(char *str, const char *delim, char **saveptr)
{
    if (!str) str = *saveptr;
    if (!str) return 0;

    /* Skip leading delimiters */
    str += strspn(str, delim);
    if (!*str) { *saveptr = str; return 0; }

    char *token = str;
    str += strcspn(str, delim);
    if (*str) { *str++ = '\0'; }
    *saveptr = str;
    return token;
}

static char *g_strtok_save = 0;

char *strtok(char *str, const char *delim)
{
    return strtok_r(str, delim, &g_strtok_save);
}

char *strsep(char **stringp, const char *delim)
{
    if (!stringp || !*stringp) return 0;
    char *token = *stringp;
    char *p = strpbrk(token, delim);
    if (p) { *p++ = '\0'; *stringp = p; }
    else    { *stringp = 0; }
    return token;
}

/* ── Duplicate ───────────────────────────────────────────────────────────── */

char *strdup(const char *s)
{
    if (!s) return 0;
    size_t len = strlen(s) + 1;
    char *copy = (char *)malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}

char *strndup(const char *s, size_t n)
{
    if (!s) return 0;
    size_t len = strnlen(s, n);
    char *copy = (char *)malloc(len + 1);
    if (copy) { memcpy(copy, s, len); copy[len] = '\0'; }
    return copy;
}

/* ── Error strings ───────────────────────────────────────────────────────── */

char *strerror(int errnum)
{
    switch (errnum) {
        case 0:   return "Success";
        case 1:   return "Operation not permitted";
        case 2:   return "No such file or directory";
        case 3:   return "No such process";
        case 4:   return "Interrupted system call";
        case 5:   return "Input/output error";
        case 6:   return "No such device or address";
        case 7:   return "Argument list too long";
        case 8:   return "Exec format error";
        case 9:   return "Bad file descriptor";
        case 11:  return "Resource temporarily unavailable";
        case 12:  return "Cannot allocate memory";
        case 13:  return "Permission denied";
        case 14:  return "Bad address";
        case 17:  return "File exists";
        case 19:  return "No such device";
        case 20:  return "Not a directory";
        case 21:  return "Is a directory";
        case 22:  return "Invalid argument";
        case 23:  return "Too many open files in system";
        case 24:  return "Too many open files";
        case 25:  return "Inappropriate ioctl for device";
        case 28:  return "No space left on device";
        case 32:  return "Broken pipe";
        case 38:  return "Function not implemented";
        default:  return "Unknown error";
    }
}

int strerror_r(int errnum, char *buf, size_t buflen)
{
    if (!buf || buflen == 0) return -1;
    char *msg = strerror(errnum);
    strncpy(buf, msg, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

/* ── Memory ──────────────────────────────────────────────────────────────── */

void *memset(void *dest, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    unsigned long c64 = (unsigned char)c;
    c64 |= (c64 << 8);
    c64 |= (c64 << 16);
    c64 |= (c64 << 32);

    while (n > 0 && ((unsigned long)d & 7) != 0) { *d++ = (unsigned char)c; n--; }

    unsigned long *d64 = (unsigned long *)d;
    while (n >= 8) { *d64++ = c64; n -= 8; }

    d = (unsigned char *)d64;
    while (n--) *d++ = (unsigned char)c;
    return dest;
}

void *memcpy(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if ((((unsigned long)d | (unsigned long)s) & 7) == 0) {
        unsigned long *d64 = (unsigned long *)d;
        const unsigned long *s64 = (const unsigned long *)s;
        while (n >= 8) { *d64++ = *s64++; n -= 8; }
        d = (unsigned char *)d64;
        s = (const unsigned char *)s64;
    }
    while (n--) *d++ = *s++;
    return dest;
}

void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) return dest;
    if (d < s)       { while (n--) *d++ = *s++; }
    else             { d += n; s += n; while (n--) *--d = *--s; }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) { if (*p1 != *p2) return (int)*p1 - (int)*p2; p1++; p2++; }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char ch = (unsigned char)c;
    while (n--) { if (*p == ch) return (void *)p; p++; }
    return 0;
}

void *memrchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s + n;
    unsigned char ch = (unsigned char)c;
    while (n--) {
        p--;
        if (*p == ch) return (void *)p;
    }
    return 0;
}

void *memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen)
{
    if (!haystack || !needle || needlelen == 0 || haystacklen < needlelen) return NULL;
    const unsigned char *h = (const unsigned char *)haystack;
    const unsigned char *n = (const unsigned char *)needle;
    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (memcmp(h + i, n, needlelen) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}
