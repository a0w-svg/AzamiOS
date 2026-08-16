/* ============================================================================
 * AzamiOS — Kernel Panic
 * File: kernel/panic.c
 * ============================================================================ */

#include "panic.h"
#include "../drivers/char/console.h"
#include "../include/azami/defs.h"
#include <stdarg.h>

/* Minimal unsigned-integer → decimal string helper (no libc dependency). */
static void panic_print_uint(unsigned long long v)
{
    char buf[24];
    int  i = 23;
    buf[i] = '\0';
    if (v == 0) { kputc('0'); return; }
    while (v && i > 0) { buf[--i] = '0' + (int)(v % 10); v /= 10; }
    kprintf(buf + i);
}

/* Minimal unsigned-integer → hex string helper. */
static void panic_print_hex(unsigned long long v, int min_digits)
{
    static const char hex[] = "0123456789abcdef";
    char buf[20];
    int  i = 19;
    buf[i] = '\0';
    if (v == 0 && min_digits == 0) { kputc('0'); return; }
    while ((v || min_digits > 0) && i > 0) {
        buf[--i] = hex[v & 0xF];
        v >>= 4;
        if (min_digits > 0) min_digits--;
    }
    kprintf(buf + i);
}

__noreturn void kernel_panic(const char *fmt, ...)
{
    cpu_cli();

    kprintf("\n\n");
    kprintf("=====================================\n");
    kprintf("       AzamiOS  KERNEL  PANIC        \n");
    kprintf("=====================================\n");
    kprintf("  ");

    va_list ap;
    va_start(ap, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { kputc(*p); continue; }
        p++;
        if (!*p) break;

        /* Skip flags: '-', '+', ' ', '#', '0' */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') p++;

        /* Skip width digits */
        while (*p >= '1' && *p <= '9') p++;

        /* Skip precision: '.' followed by optional digits */
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }

        /* Handle optional 'll' / 'l' length modifier */
        int is_ll = 0, is_l = 0;
        if (*p == 'l') { p++; is_l = 1; if (*p == 'l') { p++; is_ll = 1; } }

        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            kprintf(s ? s : "(null)");
            break;
        }
        case 'u':
        case 'd': {
            unsigned long long v = is_ll ? va_arg(ap, unsigned long long)
                                 : is_l  ? (unsigned long long)va_arg(ap, unsigned long)
                                         : (unsigned long long)va_arg(ap, unsigned int);
            panic_print_uint(v);
            break;
        }
        case 'x':
        case 'X':
        case 'p': {
            unsigned long long v = (*p == 'p')
                ? (unsigned long long)(uintptr_t)va_arg(ap, void *)
                : is_ll ? va_arg(ap, unsigned long long)
                : is_l  ? (unsigned long long)va_arg(ap, unsigned long)
                        : (unsigned long long)va_arg(ap, unsigned int);
            panic_print_hex(v, 0);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            kputc(c);
            break;
        }
        case '%':
            kputc('%');
            break;
        default:
            kputc('%');
            if (is_ll) { kputc('l'); kputc('l'); }
            else if (is_l) kputc('l');
            kputc(*p);
            break;
        }
    }
    va_end(ap);

    kprintf("\n\n  System halted.\n");

    /* This is the guaranteed terminal point — the __noreturn is satisfied. */
    cpu_halt_loop();
    __builtin_unreachable();
}
