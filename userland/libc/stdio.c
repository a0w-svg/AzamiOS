/* ============================================================================
 * AzamiOS Userspace — Standard I/O Implementation  (POSIX-compatible)
 * File: user/libc/stdio.c
 * ============================================================================ */

#include "include/stdio.h"
#include "include/string.h"
#include "include/stdlib.h"
#include <stdarg.h>

/* ── Core character I/O ──────────────────────────────────────────────────── */

int putchar(int c)
{
    char ch = (char)c;
    sys_write(1, &ch, 1);
    return c;
}

int getchar(void)
{
    char c;
    ssize_t n = sys_read(0, &c, 1);
    return (n == 1) ? (unsigned char)c : -1; /* EOF = -1 */
}

/* ── FILE helpers ────────────────────────────────────────────────────────── */

/* Map the sentinel stdin/stdout/stderr "pointers" (0, 1, 2) to real fds.
 * For real FILE objects allocated by fopen, filp->fd is used directly. */
static inline int _file_fd(FILE *f)
{
    unsigned long v = (unsigned long)f;
    if (v <= 2) return (int)v;  /* stdin=0, stdout=1, stderr=2 */
    return f->fd;
}

static inline int _file_is_sentinel(FILE *f)
{
    return ((unsigned long)f <= 2);
}

/* ── fputc / fputs / puts ────────────────────────────────────────────────── */

int fputc(int c, FILE *stream)
{
    char ch = (char)c;
    if (sys_write(_file_fd(stream), &ch, 1) == 1) return (unsigned char)c;
    return -1; /* EOF */
}

int fputs(const char *s, FILE *stream)
{
    if (!s) s = "(null)";
    size_t len = strlen(s);
    return (sys_write(_file_fd(stream), s, len) == (ssize_t)len) ? 0 : -1;
}

int puts(const char *s)
{
    if (!s) s = "(null)";
    sys_write(1, s, strlen(s));
    putchar('\n');
    return 0;
}

/* fgets: reads up to n-1 chars from stream, stops at '\n' or EOF */
char *fgets(char *s, int n, FILE *stream)
{
    if (!s || n <= 0) return 0;
    int fd = _file_fd(stream);
    int i = 0;
    while (i < n - 1) {
        char c;
        ssize_t r = sys_read(fd, &c, 1);
        if (r <= 0) { if (i == 0) return 0; break; }
        s[i++] = c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return (i > 0) ? s : 0;
}

/* ── printf engine ───────────────────────────────────────────────────────── */

static void _print_num(unsigned long long n, int base, int sign,
                        char *buf, int *idx, int max_len)
{
    char tmp[64];
    int i = 0;

    if (sign && (long long)n < 0) {
        if (*idx < max_len - 1) buf[(*idx)++] = '-';
        n = (unsigned long long)(0ULL - n);
    }
    if (n == 0) { tmp[i++] = '0'; }
    else { while (n) { int r = (int)(n % (unsigned long long)base);
                       tmp[i++] = r < 10 ? '0' + r : 'a' + r - 10;
                       n /= (unsigned long long)base; } }
    while (i > 0) { if (*idx < max_len - 1) buf[(*idx)++] = tmp[--i]; }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    int idx = 0;
    int max = (int)size;

    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            if (idx < max - 1) buf[idx++] = fmt[i];
            continue;
        }
        i++;
        if (!fmt[i]) break;

        /* Width / zero-pad prefix (minimal) */
        int zero_pad = 0, width = 0;
        if (fmt[i] == '0') { zero_pad = 1; i++; }
        while (fmt[i] >= '1' && fmt[i] <= '9') { width = width * 10 + (fmt[i++] - '0'); }

        switch (fmt[i]) {
        case 's': {
            const char *str = va_arg(args, const char *);
            if (!str) str = "(null)";
            while (*str) { if (idx < max - 1) buf[idx++] = *str; str++; }
            break;
        }
        case 'd': case 'i': {
            int val = va_arg(args, int);
            _print_num((unsigned long long)(long long)val, 10, 1, buf, &idx, max);
            break;
        }
        case 'u': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num(val, 10, 0, buf, &idx, max);
            break;
        }
        case 'x': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num(val, 16, 0, buf, &idx, max);
            break;
        }
        case 'X': {
            /* Upper-case hex */
            unsigned int val = va_arg(args, unsigned int);
            char tmp2[64]; int ti = 0;
            _print_num(val, 16, 0, tmp2, &ti, (int)sizeof(tmp2));
            for (int j = 0; j < ti; j++) {
                char c = tmp2[j];
                if (c >= 'a' && c <= 'f') c -= 32;
                if (idx < max - 1) buf[idx++] = c;
            }
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(args, void *);
            if (idx < max - 1) buf[idx++] = '0';
            if (idx < max - 1) buf[idx++] = 'x';
            _print_num(val, 16, 0, buf, &idx, max);
            break;
        }
        case 'o': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num(val, 8, 0, buf, &idx, max);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(args, int);
            if (idx < max - 1) buf[idx++] = ch;
            break;
        }
        case 'l': {
            if (fmt[i + 1] == 'l') i++;
            if      (fmt[i + 1] == 'u') { i++; unsigned long long v = va_arg(args, unsigned long long); _print_num(v, 10, 0, buf, &idx, max); }
            else if (fmt[i + 1] == 'x' || fmt[i + 1] == 'X') { i++; unsigned long long v = va_arg(args, unsigned long long); _print_num(v, 16, 0, buf, &idx, max); }
            else if (fmt[i + 1] == 'd' || fmt[i + 1] == 'i') { i++; long long v = va_arg(args, long long); _print_num((unsigned long long)v, 10, 1, buf, &idx, max); }
            else { /* BUG-13: consume arg to keep va_list aligned */ if (fmt[i+1]) i++; (void)va_arg(args, unsigned long long); }
            break;
        }
        case 'z': {
            /* %zu / %zd */
            if (fmt[i + 1] == 'u') { i++; size_t v = va_arg(args, size_t); _print_num(v, 10, 0, buf, &idx, max); }
            else if (fmt[i + 1] == 'd') { i++; ssize_t v = va_arg(args, ssize_t); _print_num((unsigned long long)v, 10, 1, buf, &idx, max); }
            else { (void)va_arg(args, size_t); }
            break;
        }
        case '%':
            if (idx < max - 1) buf[idx++] = '%';
            break;
        default:
            if (idx < max - 1) buf[idx++] = fmt[i];
            break;
        }
        (void)zero_pad; (void)width; /* width/zero-pad: noted, not yet implemented */
    }

    if (max > 0) buf[idx] = '\0';
    return idx;
}

/* ── Public printf variants ──────────────────────────────────────────────── */

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}

int vsprintf(char *buf, const char *fmt, va_list ap)
{
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int vprintf(const char *fmt, va_list ap)
{
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) sys_write(1, buf, (size_t)len);
    return len;
}

int printf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int vfprintf(FILE *stream, const char *fmt, va_list ap)
{
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) sys_write(_file_fd(stream), buf, (size_t)len);
    return len;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vfprintf(stream, fmt, ap);
    va_end(ap);
    return r;
}

/* ── perror ──────────────────────────────────────────────────────────────── */

void perror(const char *s)
{
    if (s && *s) { sys_write(2, s, strlen(s)); sys_write(2, ": ", 2); }
    /* We don't have errno yet — just say "error" */
    sys_write(2, "error\n", 6);
}

/* ── getline: POSIX.1-2008 ───────────────────────────────────────────────── */

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    if (!lineptr || !n || !stream) return -1;

    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) return -1;
    }

    int fd = _file_fd(stream);
    size_t pos = 0;
    while (1) {
        if (pos + 1 >= *n) {
            size_t new_n = *n * 2;
            char *tmp = (char *)realloc(*lineptr, new_n);
            if (!tmp) return (ssize_t)pos > 0 ? (ssize_t)pos : -1;
            *lineptr = tmp;
            *n = new_n;
        }
        char c;
        ssize_t r = sys_read(fd, &c, 1);
        if (r <= 0) { (*lineptr)[pos] = '\0'; return pos > 0 ? (ssize_t)pos : -1; }
        (*lineptr)[pos++] = c;
        if (c == '\n') break;
    }
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

/* ── sscanf (minimal — %d, %u, %s, %c, %%) ──────────────────────────────── */

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int matched = 0;
    const char *s = str;

    for (; *fmt && *s; fmt++) {
        if (*fmt != '%') {
            if (*fmt == *s) s++;
            continue;
        }
        fmt++;
        if (!*fmt) break;
        /* Skip leading spaces in input for numeric conversions */
        if (*fmt != 'c') while (*s == ' ' || (*s >= '\t' && *s <= '\r')) s++;

        switch (*fmt) {
        case 'd': case 'i': {
            int *p = va_arg(ap, int *);
            int neg = 0;
            if (*s == '-') { neg = 1; s++; }
            else if (*s == '+') s++;
            if (!(*s >= '0' && *s <= '9')) goto done;
            long long v = 0;
            while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
            *p = (int)(neg ? -v : v);
            matched++;
            break;
        }
        case 'u': {
            unsigned int *p = va_arg(ap, unsigned int *);
            if (!(*s >= '0' && *s <= '9')) goto done;
            unsigned long long v = 0;
            while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned long long)(*s++ - '0');
            *p = (unsigned int)v;
            matched++;
            break;
        }
        case 's': {
            char *p = va_arg(ap, char *);
            if (!*s) goto done;
            while (*s && *s != ' ' && !(*s >= '\t' && *s <= '\r')) *p++ = *s++;
            *p = '\0';
            matched++;
            break;
        }
        case 'c': {
            char *p = va_arg(ap, char *);
            *p = *s++;
            matched++;
            break;
        }
        case '%':
            if (*s == '%') s++;
            break;
        default:
            goto done;
        }
    }
done:
    va_end(ap);
    return matched;
}

/* ── FILE I/O ────────────────────────────────────────────────────────────── */

/* Parse fopen mode string into O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC flags.
 * We use the same numeric constants as Linux x86-64. */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000
#define O_APPEND 02000

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode) return 0;
    int flags = 0;
    if      (mode[0] == 'r') flags = (mode[1] == '+') ? O_RDWR : O_RDONLY;
    else if (mode[0] == 'w') flags = O_CREAT | O_TRUNC | ((mode[1] == '+') ? O_RDWR : O_WRONLY); /* M-08: don't OR O_WRONLY with O_RDWR */
    else if (mode[0] == 'a') flags = O_CREAT | O_APPEND | ((mode[1] == '+') ? O_RDWR : O_WRONLY); /* M-08: same fix for append */
    else return 0;

    int fd = sys_open(path, flags, 0644);
    if (fd < 0) return 0;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); return 0; }
    f->fd  = fd;
    f->err = 0;
    f->eof = 0;
    return f;
}

int fclose(FILE *stream)
{
    if (!stream || _file_is_sentinel(stream)) return -1;
    int r = sys_close(stream->fd);
    free(stream);
    return r;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    /* H-09: check for overflow before multiplying */
    if (nmemb > (size_t)-1 / size) return 0;
    size_t total = size * nmemb;
    ssize_t n = sys_read(_file_fd(stream), ptr, total);
    if (n < 0) { if (!_file_is_sentinel(stream)) stream->err = 1; return 0; }
    if (n == 0) { if (!_file_is_sentinel(stream)) stream->eof = 1; }
    return (size_t)n / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    /* H-09: check for overflow before multiplying */
    if (nmemb > (size_t)-1 / size) return 0;
    size_t total = size * nmemb;
    ssize_t n = sys_write(_file_fd(stream), ptr, total);
    if (n < 0) { if (!_file_is_sentinel(stream)) stream->err = 1; return 0; }
    return (size_t)n / size;
}

int fseek(FILE *stream, long offset, int whence)
{
    if (!stream) return -1;
    long r = syscall3(SYS_lseek, (long)_file_fd(stream), offset, (long)whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *stream)
{
    if (!stream) return -1;
    return (long)syscall3(SYS_lseek, (long)_file_fd(stream), 0L, (long)SEEK_CUR);
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0; /* No buffering implemented yet */
}

int feof(FILE *stream)
{
    if (!stream || _file_is_sentinel(stream)) return 0;
    return stream->eof;
}

int ferror(FILE *stream)
{
    if (!stream || _file_is_sentinel(stream)) return 0;
    return stream->err;
}

void clearerr(FILE *stream)
{
    if (stream && !_file_is_sentinel(stream)) { stream->err = 0; stream->eof = 0; }
}

int fileno(FILE *stream)
{
    if (!stream) return -1;
    return _file_fd(stream);
}

void rewind(FILE *stream)
{
    fseek(stream, 0L, SEEK_SET);
    clearerr(stream);
}

