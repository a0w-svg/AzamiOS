/* ============================================================================
 * AzamiOS — Bochs / QEMU Debug Console (debugcon) Driver
 * File: drivers/char/debugcon.c
 *
 * Implements the standard x86 hypervisor fast debug output port (0xE9).
 * Writing characters to port 0xE9 directly passes them to the host QEMU / Bochs
 * monitor, console, or log file with zero delay. Exposed to userspace as
 * /dev/debugcon and used by the kernel during early boot and panic logging.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "debugcon.h"
#include "../base/platform.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static bool       g_debugcon_active = false;
static spinlock_t g_debugcon_lock = SPINLOCK_INIT;

bool debugcon_is_present(void)
{
    return g_debugcon_active;
}

void debugcon_putc(char c)
{
    if (!g_debugcon_active) return;
    outb(DEBUGCON_PORT, (u8)c);
}

void debugcon_write(const char *buf, size_t len)
{
    if (!g_debugcon_active || !buf || len == 0) return;
    irqflags_t flags = spinlock_lock_irqsave(&g_debugcon_lock);
    for (size_t i = 0; i < len; i++) {
        outb(DEBUGCON_PORT, (u8)buf[i]);
    }
    spinlock_unlock_irqrestore(&g_debugcon_lock, flags);
}

void debugcon_puts(const char *s)
{
    if (!s) return;
    size_t len = 0;
    while (s[len]) len++;
    debugcon_write(s, len);
}

/* ── /dev/debugcon character device ──────────────────────────────────────── */

static s64 dev_debugcon_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_debugcon_active || !buf || len == 0) return 0;
    debugcon_write((const char *)buf, len);
    return (s64)len;
}

static file_operations_t g_debugcon_fops = {
    .read    = NULL,
    .write   = dev_debugcon_write,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

static int debugcon_probe(platform_device_t *pdev)
{
    (void)pdev;
    g_debugcon_active = true;
    devfs_register_device("debugcon", &g_debugcon_fops, NULL);
    pr_debug("[DEBUGCON] Bochs/QEMU debug console active at port 0x%04X (/dev/debugcon)\n",
             DEBUGCON_PORT);

    return 0;
}

static platform_driver_t g_debugcon_pdrv = {
    .drv   = { .name = "debugcon" },
    .probe = debugcon_probe,
};

void debugcon_init(void)
{
    platform_driver_register(&g_debugcon_pdrv);
}
