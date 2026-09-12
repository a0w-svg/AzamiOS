/* ============================================================================
 * AzamiOS Userspace — String Implementation (POSIX-compatible)
 * File: userland/libc/string.c
 * ============================================================================ */

#include "include/string.h"
#include "include/stdlib.h"
#include "include/ctype.h"
#include "include/stdio.h"

/* ── Lengths ─────────────────────────────────────────────────────────────── */

/* strlen() and strnlen() are the SSE2/AVX2 versions in string_simd.c. */

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

/* strcmp() and strncmp() are the SSE2/AVX2 versions in string_simd.c. */

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

/* strchr() and strrchr() are the SSE2/AVX2 versions in string_simd.c. */

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

/* memset(), memcpy(), memmove(), memcmp(), and memchr() are the SSE2/AVX2 versions in string_simd.c. */

/* memcmp() and memchr() are the SSE2/AVX2 versions in string_simd.c. */

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

/* ── POSIX strings.h & libgen.h ─────────────────────────────────────────── */

char *strsignal(int sig)
{
    switch (sig) {
        case 1:  return "Hangup";
        case 2:  return "Interrupt";
        case 3:  return "Quit";
        case 4:  return "Illegal instruction";
        case 5:  return "Trace/breakpoint trap";
        case 6:  return "Aborted";
        case 7:  return "Bus error";
        case 8:  return "Floating point exception";
        case 9:  return "Killed";
        case 10: return "User defined signal 1";
        case 11: return "Segmentation fault";
        case 12: return "User defined signal 2";
        case 13: return "Broken pipe";
        case 14: return "Alarm clock";
        case 15: return "Terminated";
        case 17: return "Child exited";
        case 18: return "Continued";
        case 19: return "Stopped (signal)";
        case 20: return "Stopped";
        case 23: return "Urgent I/O condition";
        case 24: return "CPU time limit exceeded";
        case 25: return "File size limit exceeded";
        case 28: return "Window changed";
        case 29: return "I/O possible";
        default: return "Unknown signal";
    }
}

void psignal(int sig, const char *s)
{
    if (s && *s) {
        sys_write(2, s, strlen(s));
        sys_write(2, ": ", 2);
    }
    const char *sig_str = strsignal(sig);
    sys_write(2, sig_str, strlen(sig_str));
    sys_write(2, "\n", 1);
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    /* strlcat must not read past size bytes of dst — use strnlen to bound
     * the scan in case dst is not null-terminated within the buffer. */
    size_t dstlen = strnlen(dst, size);
    size_t srclen = strlen(src);
    if (dstlen >= size) return size + srclen;
    /* Guard against size_t overflow when computing total length needed. */
    size_t avail = size - dstlen - 1;
    size_t copylen = (srclen > avail) ? avail : srclen;
    memcpy(dst + dstlen, src, copylen);
    dst[dstlen + copylen] = '\0';
    return dstlen + srclen;
}

int strcoll(const char *s1, const char *s2)
{
    return strcmp(s1, s2);
}

size_t strxfrm(char *dest, const char *src, size_t n)
{
    size_t len = strlen(src);
    if (n > 0) {
        size_t c = (len >= n) ? n - 1 : len;
        memcpy(dest, src, c);
        dest[c] = '\0';
    }
    return len;
}

void *memccpy(void *dest, const void *src, int c, size_t n)
{
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dest;
    unsigned char uc = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if (s[i] == uc) return d + i + 1;
    }
    return NULL;
}

int ffsl(long i)
{
    if (i == 0) return 0;
    return __builtin_ffsl(i);
}

int ffsll(long long i)
{
    if (i == 0) return 0;
    return __builtin_ffsll(i);
}

int ffs(int i)
{
    if (i == 0) return 0;
    return __builtin_ffs(i);
}

void bzero(void *s, size_t n)
{
    memset(s, 0, n);
}

void bcopy(const void *src, void *dest, size_t n)
{
    memmove(dest, src, n);
}

/* bcmp() is defined alongside memcmp() in string_simd.c. */

char *index(const char *s, int c)
{
    return strchr(s, c);
}

char *rindex(const char *s, int c)
{
    return strrchr(s, c);
}

char *basename(char *path)
{
    static char s_dot[] = ".";
    static char s_slash[] = "/";
    if (!path || !*path) return s_dot;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    if (len == 1 && path[0] == '/') return s_slash;
    char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

char *dirname(char *path)
{
    static char s_dot[] = ".";
    static char s_slash[] = "/";
    if (!path || !*path) return s_dot;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
    }
    if (len == 1 && path[0] == '/') return s_slash;
    char *slash = strrchr(path, '/');
    if (!slash) return s_dot;
    if (slash == path) return s_slash;
    *slash = '\0';
    return path;
}

/* ── Wide character string helpers ───────────────────────────────────────── */

#include "include/wchar.h"

size_t wcslen(const wchar_t *s)
{
    size_t len = 0;
    while (s && *s++) len++;
    return len;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src)
{
    wchar_t *d = dest;
    while ((*d++ = *src++));
    return dest;
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n)
{
    wchar_t *d = dest;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = 0;
    return dest;
}

int wcscmp(const wchar_t *s1, const wchar_t *s2)
{
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned int *)s1 - *(const unsigned int *)s2;
}

int wcsncmp(const wchar_t *s1, const wchar_t *s2, size_t n)
{
    if (n == 0) return 0;
    while (--n && *s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned int *)s1 - *(const unsigned int *)s2;
}

size_t mbstowcs(wchar_t *dest, const char *src, size_t n)
{
    size_t count = 0;
    while (count < n && *src) {
        if (dest) dest[count] = (wchar_t)(unsigned char)*src;
        src++;
        count++;
    }
    if (count < n && dest) dest[count] = 0;
    return count;
}

size_t wcstombs(char *dest, const wchar_t *src, size_t n)
{
    size_t count = 0;
    while (count < n && *src) {
        if (dest) dest[count] = (char)(*src & 0x7F);
        src++;
        count++;
    }
    if (count < n && dest) dest[count] = '\0';
    return count;
}

size_t mbrtowc(wchar_t *pwc, const char *s, size_t n, mbstate_t *ps)
{
    (void)ps;
    if (!s || n == 0) return 0;
    if (!*s) return 0;
    if (pwc) *pwc = (wchar_t)(unsigned char)*s;
    return 1;
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *ps)
{
    (void)ps;
    if (!s) return 1;
    *s = (char)(wc & 0x7F);
    return 1;
}

int mbtowc(wchar_t *pwc, const char *s, size_t n)
{
    return (int)mbrtowc(pwc, s, n, NULL);
}

int wctomb(char *s, wchar_t wc)
{
    return (int)wcrtomb(s, wc, NULL);
}

/* wcschr/wcsrchr/wcscat/wcsncat/wmemchr/wmemcmp/wmemcpy/wmemmove/wmemset
 * were declared in wchar.h but never defined here — any caller referencing
 * one got an undefined-reference link error. Direct transliterations of
 * their strchr/strrchr/strcat/strncat/memchr/memcmp/memcpy/memmove/memset
 * counterparts above, just over wchar_t instead of char. */

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    if (!s) return 0;
    while (*s) { if (*s == c) return (wchar_t *)s; s++; }
    return (c == 0) ? (wchar_t *)s : 0;
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    if (!s) return 0;
    const wchar_t *last = 0;
    while (*s) { if (*s == c) last = s; s++; }
    return (c == 0) ? (wchar_t *)s : (wchar_t *)last;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src)
{
    wchar_t *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != 0);
    return dest;
}

wchar_t *wcsncat(wchar_t *dest, const wchar_t *src, size_t n)
{
    wchar_t *d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dest;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    while (n--) { if (*s == c) return (wchar_t *)s; s++; }
    return 0;
}

int wmemcmp(const wchar_t *s1, const wchar_t *s2, size_t n)
{
    while (n--) {
        if (*s1 != *s2) return (*s1 < *s2) ? -1 : 1;
        s1++; s2++;
    }
    return 0;
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n)
{
    wchar_t *d = dest;
    while (n--) *d++ = *src++;
    return dest;
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n)
{
    if (dest == src || n == 0) return dest;
    if (dest < src || dest >= src + n) {
        return wmemcpy(dest, src, n);
    }
    wchar_t *d = dest + n;
    const wchar_t *s = src + n;
    while (n--) *--d = *--s;
    return dest;
}

wchar_t *wmemset(wchar_t *s, wchar_t c, size_t n)
{
    wchar_t *p = s;
    while (n--) *p++ = c;
    return s;
}

/* rawmemchr() and strchrnul() are the SSE2/AVX2 versions in string_simd.c. */

int strverscmp(const char *s1, const char *s2)
{
    return strcmp(s1, s2);
}

void explicit_bzero(void *s, size_t n)
{
    volatile unsigned char *p = (volatile unsigned char *)s;
    while (n--) *p++ = 0;
}

int timingsafe_bcmp(const void *b1, const void *b2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)b1;
    const unsigned char *p2 = (const unsigned char *)b2;
    int res = 0;
    for (size_t i = 0; i < n; i++) {
        res |= p1[i] ^ p2[i];
    }
    return res;
}

void *mempcpy(void *dest, const void *src, size_t n)
{
    return (void *)((char *)memcpy(dest, src, n) + n);
}


