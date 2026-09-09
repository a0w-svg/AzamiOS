/* ============================================================================
 * AzamiOS — Phase 5a dynamic linking milestone: test PIE application
 * File: userland/examples/dltest_app.c
 *
 * Loaded via PT_INTERP -> ld-azami.so (userland/ldso/), which resolves and
 * relocates the call into dltest_lib.so before jumping here. No libc: this
 * is a freestanding PIE binary, built and linked the same minimal way
 * ld-azami.so itself is (see userland/ldso/ldso.c's top comment for why).
 * ============================================================================ */

#include <stdint.h>

extern int dltest_add(int a, int b);

static long raw_write(int fd, const void *buf, uint64_t len)
{
    long ret;
    __asm__ volatile(
        "syscall"
        : "=a"(ret)
        : "a"(1L /* SYS_write */), "D"(fd), "S"(buf), "d"(len)
        : "rcx", "r11", "memory"
    );
    return ret;
}

/* Minimal itoa for a small (possibly negative) int — no libc available. */
static uint64_t int_to_str(int v, char *buf)
{
    char tmp[24];
    int n = 0;
    int neg = v < 0;
    unsigned int uv = neg ? (unsigned int)(-v) : (unsigned int)v;
    if (uv == 0) { buf[0] = '0'; return 1; }
    while (uv) { tmp[n++] = (char)('0' + (uv % 10)); uv /= 10; }
    uint64_t i = 0;
    if (neg) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    return i;
}

static uint64_t cstrlen(const char *s) { uint64_t n = 0; while (s[n]) n++; return n; }

int main(void)
{
    int r = dltest_add(19, 23); /* expect 42, resolved through ld-azami.so */

    char msg[80];
    const char *prefix = "dltest_app: dltest_add(19,23) = ";
    uint64_t plen = cstrlen(prefix);
    char *p = msg;
    for (uint64_t i = 0; i < plen; i++) p[i] = prefix[i];
    p += plen;
    p += int_to_str(r, p);
    *p++ = '\n';
    raw_write(1, msg, (uint64_t)(p - msg));

    return (r == 42) ? 0 : 1;
}
