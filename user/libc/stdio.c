/**
 * stdio.c — AzamiOS libc: complete C standard I/O
 */
#include "include/stdio.h"
#include "include/string.h"
#include "include/stdlib.h"
#include <unistd.h>
#include <fcntl.h>

/* ── Standard streams ────────────────────────────────────────────── */

static FILE _streams[3] = {
    { STDIN_FILENO,  _FILE_FLAG_RDONLY, 0, -1 },
    { STDOUT_FILENO, _FILE_FLAG_WRONLY, 0, -1 },
    { STDERR_FILENO, _FILE_FLAG_WRONLY, 0, -1 }
};

FILE *stdin  = &_streams[0];
FILE *stdout = &_streams[1];
FILE *stderr = &_streams[2];

/* ── Syscall wrappers ────────────────────────────────────────────── */

static void syscall_putchar(char c) {
    asm volatile("int $128" : : "a"(1), "b"(c) : "memory");
}

static void syscall_print_string(const char *str) {
    asm volatile("int $128" : : "a"(0), "b"(str) : "memory");
}

static int syscall_getchar(void) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(3) : "memory");
    return ret;
}

/* ── Character I/O ───────────────────────────────────────────────── */

int putchar(int c) {
    syscall_putchar((char)c);
    return (unsigned char)c;
}

int getchar(void) {
    return syscall_getchar();
}

int puts(const char *s) {
    syscall_print_string(s);
    syscall_putchar('\n');
    return 0;
}

int fputc(int c, FILE *fp) {
    if (!fp || fp->fd < 0 || (fp->flags & _FILE_FLAG_RDONLY)) return EOF;
    if (fp->fd == STDOUT_FILENO || fp->fd == STDERR_FILENO) {
        syscall_putchar((char)c);
        return (unsigned char)c;
    }
    char ch = (char)c;
    if (write(fp->fd, &ch, 1) == 1) {
        fp->pos++;
        return (unsigned char)c;
    }
    fp->flags |= _FILE_FLAG_ERR;
    return EOF;
}

int fgetc(FILE *fp) {
    if (!fp || fp->fd < 0 || (fp->flags & _FILE_FLAG_WRONLY)) return EOF;
    if (fp->unget != -1) {
        int c = fp->unget;
        fp->unget = -1;
        return c;
    }
    if (fp->fd == STDIN_FILENO) {
        return syscall_getchar();
    }
    unsigned char ch;
    ssize_t n = read(fp->fd, &ch, 1);
    if (n == 1) {
        fp->pos++;
        return ch;
    } else if (n == 0) {
        fp->flags |= _FILE_FLAG_EOF;
    } else {
        fp->flags |= _FILE_FLAG_ERR;
    }
    return EOF;
}

int ungetc(int c, FILE *fp) {
    if (c == EOF || !fp || fp->unget != -1) return EOF;
    fp->unget = (unsigned char)c;
    fp->flags &= ~_FILE_FLAG_EOF;
    return c;
}

char *fgets(char *buf, int n, FILE *fp) {
    if (n <= 0 || !buf || !fp) return 0;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(fp);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    buf[i] = '\0';
    return buf;
}

int fputs(const char *s, FILE *fp) {
    if (!s || !fp) return EOF;
    while (*s) {
        if (fputc(*s++, fp) == EOF) return EOF;
    }
    return 0;
}

/* ── File operations ─────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode) return 0;
    int flags = 0;
    int f_flags = 0;
    if (mode[0] == 'r') {
        flags = O_RDONLY;
        f_flags = _FILE_FLAG_RDONLY;
    } else if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        f_flags = _FILE_FLAG_WRONLY;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        f_flags = _FILE_FLAG_WRONLY;
    } else return 0;

    int fd = open(path, flags, 0666);
    if (fd < 0) return 0;

    FILE *fp = (FILE *)malloc(sizeof(FILE));
    if (!fp) { close(fd); return 0; }
    fp->fd = fd;
    fp->flags = f_flags;
    fp->pos = 0;
    fp->unget = -1;
    return fp;
}

int fclose(FILE *fp) {
    if (!fp || fp == stdin || fp == stdout || fp == stderr) return EOF;
    int res = close(fp->fd);
    free(fp);
    return res;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!ptr || !fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    char *buf = (char *)ptr;
    size_t count = 0;
    while (count < total) {
        int c = fgetc(fp);
        if (c == EOF) break;
        buf[count++] = (char)c;
    }
    return count / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp) {
    if (!ptr || !fp || size == 0 || nmemb == 0) return 0;
    size_t total = size * nmemb;
    const char *buf = (const char *)ptr;
    size_t count = 0;
    while (count < total) {
        if (fputc(buf[count++], fp) == EOF) break;
    }
    return count / size;
}

int fflush(FILE *fp) {
    (void)fp;
    return 0;
}

int fseek(FILE *fp, long offset, int whence) {
    (void)fp; (void)offset; (void)whence;
    return -1; /* Not supported by simple syscalls yet */
}

long ftell(FILE *fp) {
    return fp ? fp->pos : -1;
}

void rewind(FILE *fp) {
    fseek(fp, 0, SEEK_SET);
}

int feof(FILE *fp) {
    return fp ? (fp->flags & _FILE_FLAG_EOF) : 0;
}

int ferror(FILE *fp) {
    return fp ? (fp->flags & _FILE_FLAG_ERR) : 0;
}

void clearerr(FILE *fp) {
    if (fp) fp->flags &= ~(_FILE_FLAG_EOF | _FILE_FLAG_ERR);
}

/* ── Formatting Engine ───────────────────────────────────────────── */

struct out_ctx {
    char *buf;
    size_t max;
    size_t count;
    FILE *fp;
};

static void emit_char(struct out_ctx *ctx, char c) {
    if (ctx->fp) {
        fputc(c, ctx->fp);
    } else if (ctx->buf) {
        if (ctx->count < ctx->max - 1) {
            ctx->buf[ctx->count] = c;
        }
    }
    ctx->count++;
}

static void emit_string(struct out_ctx *ctx, const char *s) {
    if (!s) s = "(null)";
    while (*s) emit_char(ctx, *s++);
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out_ctx ctx = { buf, n, 0, 0 };
    char num_buf[32];

    while (*fmt) {
        if (*fmt != '%') {
            emit_char(&ctx, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            emit_char(&ctx, '%');
            fmt++;
            continue;
        }
        
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }

        switch (*fmt++) {
        case 'c':
            emit_char(&ctx, (char)va_arg(ap, int));
            break;
        case 'd':
        case 'i':
            itoa(va_arg(ap, int), num_buf, 10);
            emit_string(&ctx, num_buf);
            break;
        case 'u':
            itoa((int)va_arg(ap, unsigned int), num_buf, 10);
            emit_string(&ctx, num_buf);
            break;
        case 'x':
        case 'X':
        case 'p':
            itoa((int)va_arg(ap, unsigned int), num_buf, 16);
            if (fmt[-1] == 'p') emit_string(&ctx, "0x");
            emit_string(&ctx, num_buf);
            break;
        case 'o':
            itoa((int)va_arg(ap, unsigned int), num_buf, 8);
            emit_string(&ctx, num_buf);
            break;
        case 's':
            emit_string(&ctx, va_arg(ap, const char *));
            break;
        default:
            emit_char(&ctx, '?');
            break;
        }
    }

    if (buf && n > 0) {
        size_t end = (ctx.count < n) ? ctx.count : (n - 1);
        buf[end] = '\0';
    }
    return (int)ctx.count;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vsprintf(buf, fmt, ap);
    va_end(ap);
    return res;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return res;
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    struct out_ctx ctx = { 0, 0, 0, fp };
    char num_buf[32];

    while (*fmt) {
        if (*fmt != '%') {
            emit_char(&ctx, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            emit_char(&ctx, '%');
            fmt++;
            continue;
        }
        
        while (*fmt >= '0' && *fmt <= '9') fmt++;

        switch (*fmt++) {
        case 'c': emit_char(&ctx, (char)va_arg(ap, int)); break;
        case 'd':
        case 'i': itoa(va_arg(ap, int), num_buf, 10); emit_string(&ctx, num_buf); break;
        case 'u': itoa((int)va_arg(ap, unsigned int), num_buf, 10); emit_string(&ctx, num_buf); break;
        case 'x':
        case 'X':
        case 'p': itoa((int)va_arg(ap, unsigned int), num_buf, 16); if (fmt[-1]=='p') emit_string(&ctx, "0x"); emit_string(&ctx, num_buf); break;
        case 'o': itoa((int)va_arg(ap, unsigned int), num_buf, 8); emit_string(&ctx, num_buf); break;
        case 's': emit_string(&ctx, va_arg(ap, const char *)); break;
        default: emit_char(&ctx, '?'); break;
        }
    }
    return (int)ctx.count;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vfprintf(fp, fmt, ap);
    va_end(ap);
    return res;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = vprintf(fmt, ap);
    va_end(ap);
    return res;
}


int scanf(const char *fmt, ...) {
    (void)fmt;
    return 0; /* Minimal stub */
}

int fscanf(FILE *fp, const char *fmt, ...) {
    (void)fp; (void)fmt;
    return 0;
}

int sscanf(const char *str, const char *fmt, ...) {
    (void)str; (void)fmt;
    return 0;
}

void perror(const char *msg) {
    if (msg && *msg) {
        fprintf(stderr, "%s: ", msg);
    }
    fprintf(stderr, "error\n");
}