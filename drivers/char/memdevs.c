/* ============================================================================
 * AzamiOS — Standard Memory Devices Driver (/dev/null, /dev/zero, /dev/random, /dev/kmsg, /dev/console)
 * File: drivers/char/memdevs.c
 * ============================================================================ */

#include "memdevs.h"
#include "../../fs/vfs.h"
#include "../../kernel/lib/string.h"
#include "../../include/azami/defs.h"
#include "console.h"
#include "uart.h"

/* ── /dev/null ───────────────────────────────────────────────────────────── */

static s64 null_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    return 0; /* EOF */
}

static s64 null_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)offset;
    return (s64)len; /* Discard everything */
}

static file_operations_t g_null_fops = {
    .read = null_read,
    .write = null_write,
};

/* ── /dev/zero ───────────────────────────────────────────────────────────── */

static s64 zero_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (buf && len > 0) {
        memset(buf, 0, len);
    }
    return (s64)len;
}

static s64 zero_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)offset;
    return (s64)len;
}

static file_operations_t g_zero_fops = {
    .read = zero_read,
    .write = zero_write,
};

/* ── /dev/full ───────────────────────────────────────────────────────────── */

static s64 full_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (buf && len > 0) {
        memset(buf, 0, len);
    }
    return (s64)len;
}

static s64 full_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    return -(s64)ENOSPC; /* Always full */
}

static file_operations_t g_full_fops = {
    .read = full_read,
    .write = full_write,
};

/* ── /dev/random & /dev/urandom ──────────────────────────────────────────── */

static u64 g_rng_state = 0x853c49e6748fea9bULL;

static u64 get_random_u64(void)
{
    u64 val = 0;
    unsigned char ok = 0;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(val), "=qm"(ok));
    if (ok && val != 0) {
        return val;
    }

    /* Fallback: xorshift64star */
    g_rng_state ^= g_rng_state >> 12;
    g_rng_state ^= g_rng_state << 25;
    g_rng_state ^= g_rng_state >> 27;
    return g_rng_state * 0x2545F4914F6CDD1DULL;
}

static s64 random_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0) return 0;

    u8 *dst = (u8 *)buf;
    size_t i = 0;
    while (i < len) {
        u64 r = get_random_u64();
        size_t chunk = (len - i > 8) ? 8 : (len - i);
        memcpy(dst + i, &r, chunk);
        i += chunk;
    }
    return (s64)len;
}

static s64 random_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (buf && len >= 8) {
        u64 seed = *(const u64 *)buf;
        g_rng_state ^= seed;
    }
    return (s64)len;
}

static file_operations_t g_random_fops = {
    .read = random_read,
    .write = random_write,
};

/* ── /dev/kmsg & /dev/console ────────────────────────────────────────────── */

static s64 kmsg_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    return 0; /* EOF for now */
}

static s64 kmsg_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0) return 0;
    const char *str = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        kputc(str[i]);
    }
    return (s64)len;
}

static file_operations_t g_kmsg_fops = {
    .read = kmsg_read,
    .write = kmsg_write,
};

static s64 console_dev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0) return 0;
    char *dst = (char *)buf;
    size_t count = 0;
    while (count < len) {
        int c = uart_getc(UART_COM1);
        if (c == -1) {
            if (count > 0) break;
            return -(s64)EAGAIN;
        }
        dst[count++] = (char)c;
    }
    return (s64)count;
}

static s64 console_dev_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0) return 0;
    const char *str = (const char *)buf;
    for (size_t i = 0; i < len; i++) {
        kputc(str[i]);
    }
    return (s64)len;
}

static file_operations_t g_console_dev_fops = {
    .read = console_dev_read,
    .write = console_dev_write,
};

/* ── Initialization ──────────────────────────────────────────────────────── */

void memdevs_init(void)
{
    devfs_register_device("null", &g_null_fops, NULL);
    devfs_register_device("zero", &g_zero_fops, NULL);
    devfs_register_device("full", &g_full_fops, NULL);
    devfs_register_device("random", &g_random_fops, NULL);
    devfs_register_device("urandom", &g_random_fops, NULL);
    devfs_register_device("kmsg", &g_kmsg_fops, NULL);
    devfs_register_device("console", &g_console_dev_fops, NULL);
}
