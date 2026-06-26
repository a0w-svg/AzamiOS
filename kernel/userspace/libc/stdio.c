#include "include/stdio.h"
#include "include/string.h"
#include "include/stdlib.h"
#include <stddef.h>
#include <stdarg.h>
#include <limits.h>

/* ─── raw syscall wrappers ──────────────────────────────────────────────── */

static void syscall_putchar(char c) {
    asm volatile(
        "int $128\n"
        :
        : "a"(1), "b"(c)
    );
}

static void syscall_print_string(const char* str) {
    asm volatile(
        "int $128\n"
        :
        : "a"(0), "b"(str)
    );
}

static void syscall_exit(int code) {
    asm volatile(
        "int $128\n"
        :
        : "a"(2), "b"(code)
    );
}

static int syscall_getchar(void) {
    int ret;
    asm volatile(
        "int $128\n"
        : "=a"(ret)
        : "a"(3)
    );
    return ret;
}

/* ─── public libc functions ─────────────────────────────────────────────── */

int getchar(void) {
    return syscall_getchar();
}

int putchar(char c) {
    syscall_putchar(c);
    return (int)(unsigned char)c;
}

int puts(const char* string) {
    syscall_print_string(string);
    syscall_putchar('\n');
    return 0;
}

void exit(int code) {
    syscall_exit(code);
    for (;;); /* should not be reached */
}

void exec(const char *filename) {
    asm volatile(
        "int $128\n"
        :
        : "a"(10), "b"(filename)
    );
}

/*
 * printf — minimal implementation.
 * Flags: %c %d %s %x %o
 */
void printf(char* format, ...) {
    char *string;
    char buf[50];
    unsigned int i = 0;

    va_list arg;
    va_start(arg, format);

    while (*format != '\0') {
        if (*format == '%') {
            format++;
            switch (*format++) {
            case 'c':
                i = (unsigned int)va_arg(arg, int);
                syscall_putchar((char)i);
                break;
            case 'd':
                i = (unsigned int)va_arg(arg, int);
                itoa((int)i, buf, 10);
                syscall_print_string(buf);
                break;
            case 'o':
                i = (unsigned int)va_arg(arg, int);
                itoa((int)i, buf, 8);
                syscall_print_string(buf);
                break;
            case 's':
                string = va_arg(arg, char*);
                if (string == NULL) {
                    string = "(null)";
                }
                syscall_print_string(string);
                break;
            case 'x':
                i = (unsigned int)va_arg(arg, int);
                itoa((int)i, buf, 16);
                syscall_print_string("0x");
                syscall_print_string(buf);
                break;
            default:
                break;
            }
        } else {
            syscall_putchar(*format);
            format++;
        }
    }
    va_end(arg);
}