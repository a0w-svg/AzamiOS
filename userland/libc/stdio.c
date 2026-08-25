/* ============================================================================
 * AzamiOS Userspace — Standard I/O Implementation  (POSIX-compatible)
 * File: user/libc/stdio.c
 * ============================================================================ */

#include "include/stdio.h"
#include "include/string.h"
#include "include/stdlib.h"
#include "include/errno.h"
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

static void _print_num_formatted(unsigned long long n, int base, int sign,
                                   char *buf, int *idx, int max_len,
                                   int width, int zero_pad, int left_align, int uppercase)
{
    char tmp[64];
    int len = 0;
    int is_neg = 0;

    if (sign && (long long)n < 0) {
        is_neg = 1;
        n = (unsigned long long)(0ULL - n);
    }

    if (n == 0) {
        tmp[len++] = '0';
    } else {
        const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
        while (n) {
            tmp[len++] = digits[n % (unsigned long long)base];
            n /= (unsigned long long)base;
        }
    }

    int total_len = len + (is_neg ? 1 : 0);
    int pad_count = (width > total_len) ? (width - total_len) : 0;

    if (zero_pad && !left_align) {
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = '0';
        }
    } else if (!left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
    } else {
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
    }

    for (int i = len - 1; i >= 0; i--) {
        if (*idx < max_len - 1) buf[(*idx)++] = tmp[i];
    }

    if (left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
    }
}

static void _print_str_formatted(const char *str, char *buf, int *idx, int max_len,
                                  int width, int left_align, int prec)
{
    if (!str) str = "(null)";
    int slen = 0;
    while (str[slen]) slen++;
    if (prec >= 0 && prec < slen) slen = prec;

    int pad_count = (width > slen) ? (width - slen) : 0;

    if (!left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
    }

    for (int i = 0; i < slen; i++) {
        if (*idx < max_len - 1) buf[(*idx)++] = str[i];
    }

    if (left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
    }
}

static void _print_float_formatted(double val, char *buf, int *idx, int max_len,
                                    int width, int zero_pad, int left_align, int prec,
                                    char spec)
{
    char tmp[64];
    int tlen = 0;

    if (__builtin_isnan(val)) {
        const char *s = "nan";
        _print_str_formatted(s, buf, idx, max_len, width, left_align, -1);
        return;
    }
    if (__builtin_isinf(val)) {
        const char *s = (val < 0) ? "-inf" : "inf";
        _print_str_formatted(s, buf, idx, max_len, width, left_align, -1);
        return;
    }

    int is_neg = 0;
    if (val < 0.0) {
        is_neg = 1;
        val = -val;
    }

    if (prec < 0) prec = 6;
    if (prec > 18) prec = 18;

    if (spec == 'e' || spec == 'E') {
        int exp = 0;
        if (val > 0.0) {
            while (val >= 10.0) { val /= 10.0; exp++; }
            while (val < 1.0)   { val *= 10.0; exp--; }
        }
        double round_val = 0.5;
        for (int p = 0; p < prec; p++) round_val *= 0.1;
        val += round_val;
        if (val >= 10.0) { val /= 10.0; exp++; }

        unsigned long long int_part = (unsigned long long)val;
        val -= (double)int_part;

        char num_str[48];
        int n_idx = 0;
        num_str[n_idx++] = '0' + (char)int_part;
        if (prec > 0) {
            num_str[n_idx++] = '.';
            for (int p = 0; p < prec; p++) {
                val *= 10.0;
                int d = (int)val;
                if (d > 9) d = 9;
                num_str[n_idx++] = '0' + d;
                val -= d;
            }
        }
        num_str[n_idx++] = spec;
        num_str[n_idx++] = (exp >= 0) ? '+' : '-';
        int aexp = exp >= 0 ? exp : -exp;
        num_str[n_idx++] = '0' + (aexp / 10);
        num_str[n_idx++] = '0' + (aexp % 10);
        num_str[n_idx] = '\0';

        for (int i = 0; i < n_idx; i++) tmp[tlen++] = num_str[i];
    } else {
        double round_val = 0.5;
        for (int p = 0; p < prec; p++) round_val *= 0.1;
        val += round_val;

        unsigned long long int_part = (unsigned long long)val;
        double frac_part = val - (double)int_part;

        char int_digits[32];
        int id_len = 0;
        if (int_part == 0) {
            int_digits[id_len++] = '0';
        } else {
            unsigned long long temp = int_part;
            while (temp > 0) {
                int_digits[id_len++] = '0' + (char)(temp % 10);
                temp /= 10;
            }
        }

        for (int i = id_len - 1; i >= 0; i--) {
            tmp[tlen++] = int_digits[i];
        }

        if (prec > 0) {
            tmp[tlen++] = '.';
            for (int p = 0; p < prec; p++) {
                frac_part *= 10.0;
                int d = (int)frac_part;
                if (d > 9) d = 9;
                tmp[tlen++] = '0' + d;
                frac_part -= (double)d;
            }
        }

        if ((spec == 'g' || spec == 'G') && prec > 0) {
            while (tlen > 0 && tmp[tlen - 1] == '0') tlen--;
            if (tlen > 0 && tmp[tlen - 1] == '.') tlen--;
        }
    }

    tmp[tlen] = '\0';
    int total_len = tlen + (is_neg ? 1 : 0);
    int pad_count = (width > total_len) ? (width - total_len) : 0;

    if (zero_pad && !left_align) {
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = '0';
        }
    } else if (!left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
    } else {
        if (is_neg && *idx < max_len - 1) buf[(*idx)++] = '-';
    }

    for (int i = 0; i < tlen; i++) {
        if (*idx < max_len - 1) buf[(*idx)++] = tmp[i];
    }

    if (left_align) {
        for (int i = 0; i < pad_count; i++) {
            if (*idx < max_len - 1) buf[(*idx)++] = ' ';
        }
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    int idx = 0;
    int max = (size == (size_t)-1) ? 0x7FFFFFFF : (int)size;

    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') {
            if (idx < max - 1) buf[idx++] = fmt[i];
            continue;
        }
        i++;
        if (!fmt[i]) break;

        int zero_pad = 0, left_align = 0, width = 0, prec = -1;

        while (fmt[i] == '-' || fmt[i] == '+' || fmt[i] == '0' || fmt[i] == ' ') {
            if (fmt[i] == '-') left_align = 1;
            else if (fmt[i] == '0') zero_pad = 1;
            i++;
        }
        if (left_align) zero_pad = 0;

        while (fmt[i] >= '0' && fmt[i] <= '9') {
            width = width * 10 + (fmt[i++] - '0');
        }
        if (fmt[i] == '.') {
            i++;
            prec = 0;
            while (fmt[i] >= '0' && fmt[i] <= '9') {
                prec = prec * 10 + (fmt[i++] - '0');
            }
        }

        switch (fmt[i]) {
        case 's': {
            const char *str = va_arg(args, const char *);
            _print_str_formatted(str, buf, &idx, max, width, left_align, prec);
            break;
        }
        case 'f': case 'F': {
            double val = va_arg(args, double);
            _print_float_formatted(val, buf, &idx, max, width, zero_pad, left_align, prec, 'f');
            break;
        }
        case 'e': case 'E': {
            double val = va_arg(args, double);
            _print_float_formatted(val, buf, &idx, max, width, zero_pad, left_align, prec, fmt[i]);
            break;
        }
        case 'g': case 'G': {
            double val = va_arg(args, double);
            _print_float_formatted(val, buf, &idx, max, width, zero_pad, left_align, prec, fmt[i]);
            break;
        }
        case 'd': case 'i': {
            int val = va_arg(args, int);
            _print_num_formatted((unsigned long long)(long long)val, 10, 1, buf, &idx, max, width, zero_pad, left_align, 0);
            break;
        }
        case 'u': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num_formatted(val, 10, 0, buf, &idx, max, width, zero_pad, left_align, 0);
            break;
        }
        case 'x': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num_formatted(val, 16, 0, buf, &idx, max, width, zero_pad, left_align, 0);
            break;
        }
        case 'X': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num_formatted(val, 16, 0, buf, &idx, max, width, zero_pad, left_align, 1);
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(args, void *);
            if (idx < max - 1) buf[idx++] = '0';
            if (idx < max - 1) buf[idx++] = 'x';
            _print_num_formatted(val, 16, 0, buf, &idx, max, 0, 0, 0, 0);
            break;
        }
        case 'o': {
            unsigned int val = va_arg(args, unsigned int);
            _print_num_formatted(val, 8, 0, buf, &idx, max, width, zero_pad, left_align, 0);
            break;
        }
        case 'c': {
            char ch = (char)va_arg(args, int);
            if (idx < max - 1) buf[idx++] = ch;
            break;
        }
        case 'l': {
            if (fmt[i + 1] == 'l') i++;
            if (fmt[i + 1] == 'u') {
                i++;
                unsigned long long v = va_arg(args, unsigned long long);
                _print_num_formatted(v, 10, 0, buf, &idx, max, width, zero_pad, left_align, 0);
            } else if (fmt[i + 1] == 'x' || fmt[i + 1] == 'X') {
                int upper = (fmt[i + 1] == 'X');
                i++;
                unsigned long long v = va_arg(args, unsigned long long);
                _print_num_formatted(v, 16, 0, buf, &idx, max, width, zero_pad, left_align, upper);
            } else if (fmt[i + 1] == 'd' || fmt[i + 1] == 'i') {
                i++;
                long long v = va_arg(args, long long);
                _print_num_formatted((unsigned long long)v, 10, 1, buf, &idx, max, width, zero_pad, left_align, 0);
            } else if (fmt[i + 1] == 'f' || fmt[i + 1] == 'F' || fmt[i + 1] == 'g' || fmt[i + 1] == 'G' || fmt[i + 1] == 'e' || fmt[i + 1] == 'E') {
                char spec = fmt[i + 1];
                i++;
                double val = va_arg(args, double);
                _print_float_formatted(val, buf, &idx, max, width, zero_pad, left_align, prec, spec);
            } else {
                if (fmt[i + 1]) i++;
                (void)va_arg(args, unsigned long long);
            }
            break;
        }
        case 'z': {
            if (fmt[i + 1] == 'u') {
                i++;
                size_t v = va_arg(args, size_t);
                _print_num_formatted((unsigned long long)v, 10, 0, buf, &idx, max, width, zero_pad, left_align, 0);
            } else if (fmt[i + 1] == 'd') {
                i++;
                ssize_t v = va_arg(args, ssize_t);
                _print_num_formatted((unsigned long long)v, 10, 1, buf, &idx, max, width, zero_pad, left_align, 0);
            } else {
                (void)va_arg(args, size_t);
            }
            break;
        }
        case '%':
            if (idx < max - 1) buf[idx++] = '%';
            break;
        default:
            if (idx < max - 1) buf[idx++] = fmt[i];
            break;
        }
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

int vdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (len > 0) sys_write(fd, buf, (size_t)len);
    return len;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

int vasprintf(char **strp, const char *fmt, va_list ap)
{
    if (!strp) return -1;
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int len = vsnprintf(NULL, 0, fmt, ap_copy);
    va_end(ap_copy);
    if (len < 0) return -1;
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) return -1;
    vsnprintf(buf, (size_t)len + 1, fmt, ap);
    *strp = buf;
    return len;
}

int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;
}

void perror(const char *s)
{
    if (s && *s) {
        sys_write(2, s, strlen(s));
        sys_write(2, ": ", 2);
    }
    const char *err_str = strerror(errno);
    if (!err_str) err_str = "Unknown error";
    sys_write(2, err_str, strlen(err_str));
    sys_write(2, "\n", 1);
}

/* ── Character I/O (fgetc / ungetc) ─────────────────────────────────────── */

int ungetc(int c, FILE *stream)
{
    if (!stream || c == -1) return -1;
    if (!_file_is_sentinel(stream)) {
        stream->unget_char = (unsigned char)c;
        stream->eof = 0;
    }
    return (unsigned char)c;
}

int fgetc(FILE *stream)
{
    if (!stream) return -1;
    if (!_file_is_sentinel(stream) && stream->unget_char != -1) {
        int ch = stream->unget_char;
        stream->unget_char = -1;
        return ch;
    }
    char c;
    ssize_t n = sys_read(_file_fd(stream), &c, 1);
    if (n == 1) return (unsigned char)c;
    if (n == 0 && !_file_is_sentinel(stream)) stream->eof = 1;
    if (n < 0 && !_file_is_sentinel(stream)) stream->err = 1;
    return -1;
}

/* ── getdelim & getline: POSIX.1-2008 ────────────────────────────────────── */

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *stream)
{
    if (!lineptr || !n || !stream) { errno = EINVAL; return -1; }

    if (!*lineptr || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (!*lineptr) { errno = ENOMEM; return -1; }
    }

    size_t pos = 0;
    while (1) {
        int c = fgetc(stream);
        if (c == -1) {
            if (pos == 0) return -1;
            break;
        }
        if (pos + 2 >= *n) {
            size_t new_n = *n * 2;
            char *tmp = (char *)realloc(*lineptr, new_n);
            if (!tmp) return (ssize_t)pos > 0 ? (ssize_t)pos : -1;
            *lineptr = tmp;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == delim) break;
    }
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream)
{
    return getdelim(lineptr, n, '\n', stream);
}

/* ── sscanf & vsscanf ────────────────────────────────────────────────────── */

int vsscanf(const char *str, const char *fmt, va_list ap)
{
    int matched = 0;
    const char *s = str;

    for (; *fmt && *s; fmt++) {
        if (*fmt != '%') {
            if (*fmt == *s) s++;
            continue;
        }
        fmt++;
        if (!*fmt) break;
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
    return matched;
}

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsscanf(str, fmt, ap);
    va_end(ap);
    return r;
}

/* ── FILE I/O ────────────────────────────────────────────────────────────── */

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
    else if (mode[0] == 'w') flags = O_CREAT | O_TRUNC | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
    else if (mode[0] == 'a') flags = O_CREAT | O_APPEND | ((mode[1] == '+') ? O_RDWR : O_WRONLY);
    else return 0;

    int fd = sys_open(path, flags, 0644);
    if (fd < 0) return 0;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { sys_close(fd); return 0; }
    f->fd  = fd;
    f->err = 0;
    f->eof = 0;
    f->unget_char = -1;
    f->is_pipe = 0;
    f->pipe_pid = 0;
    return f;
}

FILE *fdopen(int fd, const char *mode)
{
    (void)mode;
    if (fd < 0) return 0;
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) return 0;
    f->fd = fd;
    f->err = 0;
    f->eof = 0;
    f->unget_char = -1;
    f->is_pipe = 0;
    f->pipe_pid = 0;
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *stream)
{
    if (!stream) return 0;
    if (!_file_is_sentinel(stream)) {
        sys_close(stream->fd);
    }
    FILE *new_f = fopen(path, mode);
    if (!new_f) return 0;
    if (!_file_is_sentinel(stream)) {
        stream->fd = new_f->fd;
        stream->err = 0;
        stream->eof = 0;
        stream->unget_char = -1;
        free(new_f);
        return stream;
    }
    return new_f;
}

extern char **environ;

FILE *popen(const char *command, const char *type)
{
    if (!command || !type) return 0;
    int is_read = (type[0] == 'r');
    int pfd[2];
    if (syscall1(SYS_pipe, (long)pfd) < 0) return 0;

    int pid = (int)syscall0(SYS_fork);
    if (pid < 0) {
        sys_close(pfd[0]);
        sys_close(pfd[1]);
        return 0;
    }

    if (pid == 0) {
        if (is_read) {
            sys_close(pfd[0]);
            syscall2(SYS_dup2, pfd[1], 1);
            sys_close(pfd[1]);
        } else {
            sys_close(pfd[1]);
            syscall2(SYS_dup2, pfd[0], 0);
            sys_close(pfd[0]);
        }
        char *const argv[] = { (char *)"sh", (char *)"-c", (char *)command, 0 };
        syscall3(SYS_execve, (long)"/bin/sh", (long)argv, (long)environ);
        sys_exit(127);
    }

    FILE *f = 0;
    if (is_read) {
        sys_close(pfd[1]);
        f = fdopen(pfd[0], "r");
    } else {
        sys_close(pfd[0]);
        f = fdopen(pfd[1], "w");
    }
    if (f) {
        f->is_pipe = 1;
        f->pipe_pid = pid;
    }
    return f;
}

int pclose(FILE *stream)
{
    if (!stream || !stream->is_pipe) return -1;
    int pid = stream->pipe_pid;
    fclose(stream);
    int status = 0;
    syscall3(SYS_wait4, pid, (long)&status, 0);
    return status;
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
    if (nmemb > (size_t)-1 / size) return 0;
    size_t total = size * nmemb;
    unsigned char *p = (unsigned char *)ptr;
    size_t done = 0;

    if (!_file_is_sentinel(stream) && stream->unget_char != -1) {
        *p++ = (unsigned char)stream->unget_char;
        stream->unget_char = -1;
        done++;
        if (done == total) return 1;
    }

    ssize_t n = sys_read(_file_fd(stream), p, total - done);
    if (n < 0) { if (!_file_is_sentinel(stream)) stream->err = 1; return done / size; }
    if (n == 0) { if (!_file_is_sentinel(stream)) stream->eof = 1; }
    done += (size_t)n;
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || size == 0 || nmemb == 0 || !stream) return 0;
    if (nmemb > (size_t)-1 / size) return 0;
    size_t total = size * nmemb;
    ssize_t n = sys_write(_file_fd(stream), ptr, total);
    if (n < 0) { if (!_file_is_sentinel(stream)) stream->err = 1; return 0; }
    return (size_t)n / size;
}

int fseek(FILE *stream, long offset, int whence)
{
    if (!stream) return -1;
    if (!_file_is_sentinel(stream)) stream->unget_char = -1;
    long r = syscall3(SYS_lseek, (long)_file_fd(stream), offset, (long)whence);
    return (r < 0) ? -1 : 0;
}

long ftell(FILE *stream)
{
    if (!stream) return -1;
    return (long)syscall3(SYS_lseek, (long)_file_fd(stream), 0L, (long)SEEK_CUR);
}

int fseeko(FILE *stream, off_t offset, int whence)
{
    return fseek(stream, (long)offset, whence);
}

off_t ftello(FILE *stream)
{
    return (off_t)ftell(stream);
}

int fflush(FILE *stream)
{
    (void)stream;
    return 0;
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
    if (stream && !_file_is_sentinel(stream)) { stream->err = 0; stream->eof = 0; stream->unget_char = -1; }
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

static unsigned int s_tmp_seq = 1000;

char *tmpnam(char *s)
{
    static char s_tmpnam_buf[64];
    char *buf = s ? s : s_tmpnam_buf;
    snprintf(buf, 64, "/tmp/tmp_%u_%u", (unsigned int)sys_getpid(), s_tmp_seq++);
    return buf;
}

char *tempnam(const char *dir, const char *pfx)
{
    const char *d = dir ? dir : "/tmp";
    const char *p = pfx ? pfx : "tmp";
    char *buf = (char *)malloc(128);
    if (!buf) return 0;
    snprintf(buf, 128, "%s/%s_%u_%u", d, p, (unsigned int)sys_getpid(), s_tmp_seq++);
    return buf;
}

FILE *tmpfile(void)
{
    char path[64];
    tmpnam(path);
    FILE *f = fopen(path, "w+");
    if (f) {
        sys_unlink(path);
    }
    return f;
}

int remove(const char *pathname)
{
    int r = sys_unlink(pathname);
    if (r == 0) return 0;
    return sys_rmdir(pathname);
}

int fgetpos(FILE *stream, fpos_t *pos)
{
    if (!stream || !pos) { errno = EINVAL; return -1; }
    off_t offset = ftello(stream);
    if (offset < 0) return -1;
    *pos = offset;
    return 0;
}

int fsetpos(FILE *stream, const fpos_t *pos)
{
    if (!stream || !pos) { errno = EINVAL; return -1; }
    return fseeko(stream, *pos, SEEK_SET);
}

int setvbuf(FILE *stream, char *buf, int mode, size_t size)
{
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;
}

void setbuf(FILE *stream, char *buf)
{
    (void)stream; (void)buf;
}

void setbuffer(FILE *stream, char *buf, size_t size)
{
    (void)stream; (void)buf; (void)size;
}

void setlinebuf(FILE *stream)
{
    (void)stream;
}

void flockfile(FILE *stream)
{
    (void)stream;
}

int ftrylockfile(FILE *stream)
{
    (void)stream;
    return 0;
}

void funlockfile(FILE *stream)
{
    (void)stream;
}



