/**
 * kernel/filesystem/vfs_dev_bridge.c — Kernel VFS ↔ Driver Model Bridge
 *
 * Registers character-device driver instances (device_t + cdev_ops_t)
 * into the VFS tree as FS_CHARDEVICE nodes under /dev/.
 *
 * Each device_t with DEV_CLASS_CHAR gets a vfs_cdev_node_t allocated from
 * a static pool (no heap dependency) and inserted into the tarfs initrd
 * tree via initrd_create_file().
 *
 * Integration path at boot:
 *   secure_uart_init()          — registers driver + device
 *   vfs_register_cdev(&secure_uart_device) — creates /dev/ttyS0 node
 */
#include "../drivers/char/secure_uart.h"
#include "../../lib/fs/vfs_dev.h"
#include "../../lib/fs/vfs.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

/* ── Static pool of VFS char-device nodes ───────────────────────────── */
#define VFS_CDEV_POOL_SIZE 16

static vfs_cdev_node_t g_cdev_pool[VFS_CDEV_POOL_SIZE];
static int             g_cdev_pool_used = 0;

/* ── Adapter: translate cdev_ops_t ↔ vfs_cdev_ops ───────────────────── */
/*
 * device_t.drv.cdev_ops uses (device_t*) as context.
 * vfs_cdev_ops uses (void*) — we store device_t* there.
 */

static ssize_t _vfs_adapt_read(void *ctx, uint8_t *buf,
                                size_t max_len, uint64_t offset) {
    device_t *dev = (device_t*)ctx;
    if (!dev || !dev->drv || !dev->drv->cdev_ops ||
        !dev->drv->cdev_ops->read) return -1;
    /* Explicit max_len forwarded — driver MUST NOT exceed it            */
    return dev->drv->cdev_ops->read(dev, buf, max_len, offset);
}

static ssize_t _vfs_adapt_write(void *ctx, const uint8_t *buf,
                                 size_t len, uint64_t offset) {
    device_t *dev = (device_t*)ctx;
    if (!dev || !dev->drv || !dev->drv->cdev_ops ||
        !dev->drv->cdev_ops->write) return -1;
    /* len forwarded unchanged; driver reads AT MOST len bytes            */
    return dev->drv->cdev_ops->write(dev, buf, len, offset);
}

static int _vfs_adapt_open(void *ctx, uint32_t flags) {
    device_t *dev = (device_t*)ctx;
    if (!dev || !dev->drv || !dev->drv->cdev_ops ||
        !dev->drv->cdev_ops->open) return 0;
    return dev->drv->cdev_ops->open(dev, flags);
}

static int _vfs_adapt_close(void *ctx) {
    device_t *dev = (device_t*)ctx;
    if (!dev || !dev->drv || !dev->drv->cdev_ops ||
        !dev->drv->cdev_ops->close) return 0;
    return dev->drv->cdev_ops->close(dev);
}

static int _vfs_adapt_ioctl(void *ctx, uint32_t cmd,
                              void *buf, size_t buf_len) {
    device_t *dev = (device_t*)ctx;
    if (!dev || !dev->drv || !dev->drv->cdev_ops ||
        !dev->drv->cdev_ops->ioctl) return -1;
    /* buf_len forwarded — prevents driver from reading beyond caller buf */
    return dev->drv->cdev_ops->ioctl(dev, cmd, buf, buf_len);
}

static const struct vfs_cdev_ops g_vfs_adapter_ops = {
    .read  = _vfs_adapt_read,
    .write = _vfs_adapt_write,
    .open  = _vfs_adapt_open,
    .close = _vfs_adapt_close,
    .ioctl = _vfs_adapt_ioctl,
};

/* ── Public API ──────────────────────────────────────────────────────── */

/**
 * vfs_register_cdev — create a /dev/<name> VFS node for a char device.
 *
 * @dev : initialized device_t with DEV_CLASS_CHAR and a valid cdev_ops
 *
 * Returns 0 on success, -1 if the pool is exhausted or dev is invalid.
 */
int vfs_register_cdev(device_t *dev) {
    if (!dev || dev->cls != DEV_CLASS_CHAR) return -1;
    if (!dev->drv || !dev->drv->cdev_ops)   return -1;

    if (g_cdev_pool_used >= VFS_CDEV_POOL_SIZE) {
        kprintf("vfs_bridge: cdev pool exhausted\n");
        return -1;
    }

    vfs_cdev_node_t *cn = &g_cdev_pool[g_cdev_pool_used++];

    /* Build the /dev/<name> path string (e.g. "dev/ttyS0")              */
    char devpath[DEVICE_NAME_MAX + 8];
    int  len = 0;
    devpath[len++] = 'd';
    devpath[len++] = 'e';
    devpath[len++] = 'v';
    devpath[len++] = '/';
    for (int i = 0; dev->name[i] && len < (int)(sizeof(devpath) - 1); i++)
        devpath[len++] = dev->name[i];
    devpath[len] = '\0';

    /* VFS_PERM_R | VFS_PERM_W | VFS_PERM_X = 0x07                      */
    vfs_cdev_node_init(cn, devpath,
                        VFS_PERM_R | VFS_PERM_W | VFS_PERM_X,
                        &g_vfs_adapter_ops,
                        (void*)dev);

    /* Insert into the initrd VFS tree */
    extern fs_node_t *initrd_create_file(char *name);
    fs_node_t *slot = initrd_create_file(devpath);
    if (!slot) {
        kprintf("vfs_bridge: could not create node for %s\n", devpath);
        g_cdev_pool_used--;
        return -1;
    }

    /* Copy the populated vfs_cdev_node_t base into the initrd slot      */
    *slot = cn->base;

    kprintf("vfs_bridge: registered /dev/%s (maj=%u min=%u)\n",
            dev->name, MAJOR(dev->devno), MINOR(dev->devno));
    return 0;
}

/**
 * vfs_dev_bridge_init — called at kernel boot after secure_uart_init().
 * Registers all known char devices into the VFS tree.
 */
void vfs_dev_bridge_init(void) {
    /* Register COM1 UART as /dev/ttyS0 */
    if (vfs_register_cdev(&secure_uart_device) != 0)
        kprintf("vfs_bridge: WARNING: failed to register /dev/ttyS0\n");

    drv_registry_dump();
}
