/**
 * lib/fs/vfs_dev.c — Secure VFS Character Device Node Implementation
 *
 * Enforces on every I/O path:
 *   1. NULL pointer guards for node, ops vtable, and buf
 *   2. Explicit size bound (max_len / len) — never exceeded
 *   3. Permission mask check before dispatch
 *   4. FS_CHARDEVICE flag verification so a plain file cannot be
 *      accidentally cast to vfs_cdev_node_t and abused
 *
 * Compiles as kernel-independent C11 (no kprintf, no arch includes).
 */
#include "vfs_dev.h"
#include "../string/string.h"

/* ── Internal: bridge functions wired into fs_node_t vtable ─────────── */
/*
 * These adapters match the legacy fs_node_t function-pointer signatures
 * and forward to the secure vfs_cdev_* API.  They are set in
 * vfs_cdev_node_init() so callers using the legacy read_fs/write_fs
 * path transparently get bounds-enforced I/O.
 */

static uint32_t _bridge_read(block_device_t *dev __attribute__((unused)),
                              struct fs_node *raw,
                              uint32_t offset, uint32_t size,
                              uint8_t *buf) {
    /* Cast: valid because vfs_cdev_node_t has fs_node_t as first member */
    vfs_cdev_node_t *cn = (vfs_cdev_node_t*)raw;
    /* Reject if type flag wrong — catches accidental casts               */
    if ((cn->base.flags & 0x07u) != FS_CHARDEVICE) return 0;
    ssize_t r = vfs_cdev_read(cn, buf, (size_t)size, (uint64_t)offset);
    return (r < 0) ? 0u : (uint32_t)r;
}

static uint32_t _bridge_write(block_device_t *dev __attribute__((unused)),
                               struct fs_node *raw,
                               uint32_t offset, uint32_t size,
                               uint8_t *buf) {
    vfs_cdev_node_t *cn = (vfs_cdev_node_t*)raw;
    if ((cn->base.flags & 0x07u) != FS_CHARDEVICE) return 0;
    ssize_t r = vfs_cdev_write(cn, buf, (size_t)size, (uint64_t)offset);
    return (r < 0) ? 0u : (uint32_t)r;
}

static int _bridge_open(struct fs_node *raw) {
    vfs_cdev_node_t *cn = (vfs_cdev_node_t*)raw;
    if ((cn->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    return vfs_cdev_open(cn, cn->open_flags);
}

static int _bridge_close(struct fs_node *raw) {
    vfs_cdev_node_t *cn = (vfs_cdev_node_t*)raw;
    if ((cn->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    return vfs_cdev_close(cn);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void vfs_cdev_node_init(vfs_cdev_node_t *node,
                         const char *name,
                         uint32_t mask,
                         const struct vfs_cdev_ops *ops,
                         void *drv_ctx) {
    if (!node) return;

    /* Zero entire struct first — no stale pointers                       */
    memset(node, 0, sizeof(*node));

    /* Populate base fs_node_t */
    strncpy(node->base.name, name ? name : "cdev", 127);
    node->base.name[127] = '\0';        /* explicit NUL terminator       */
    node->base.mask  = mask;
    node->base.flags = FS_CHARDEVICE;   /* type tag                      */

    /* Wire secure bridge into legacy vtable                              */
    node->base.read  = _bridge_read;
    node->base.write = _bridge_write;
    node->base.open  = _bridge_open;
    node->base.close = _bridge_close;

    node->ops        = ops;
    node->drv_ctx    = drv_ctx;
    node->open_flags = 0;
}

ssize_t vfs_cdev_read(vfs_cdev_node_t *node, uint8_t *buf,
                       size_t max_len, uint64_t offset) {
    /* Guard 1: NULL checks */
    if (!node || !buf) return -1;
    /* Guard 2: zero-length read is a no-op, not an error */
    if (max_len == 0) return 0;
    /* Guard 3: type safety */
    if ((node->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    /* Guard 4: permission — must have read bit */
    if (!(node->base.mask & VFS_PERM_R)) return -1;
    /* Guard 5: vtable */
    if (!node->ops || !node->ops->read) return -1;

    /* Dispatch — driver MUST NOT write more than max_len bytes           */
    return node->ops->read(node->drv_ctx, buf, max_len, offset);
}

ssize_t vfs_cdev_write(vfs_cdev_node_t *node, const uint8_t *buf,
                        size_t len, uint64_t offset) {
    if (!node || !buf)  return -1;
    if (len == 0)       return 0;       /* zero-len write: no-op         */
    if ((node->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    if (!(node->base.mask & VFS_PERM_W)) return -1;
    if (!node->ops || !node->ops->write) return -1;

    /* Dispatch — driver MUST NOT read beyond buf+len                     */
    return node->ops->write(node->drv_ctx, buf, len, offset);
}

int vfs_cdev_open(vfs_cdev_node_t *node, uint32_t flags) {
    if (!node) return -1;
    if ((node->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    if (!node->ops || !node->ops->open) return 0; /* open is optional    */
    node->open_flags = flags;
    return node->ops->open(node->drv_ctx, flags);
}

int vfs_cdev_close(vfs_cdev_node_t *node) {
    if (!node) return -1;
    if ((node->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    if (!node->ops || !node->ops->close) return 0; /* close is optional  */
    return node->ops->close(node->drv_ctx);
}

int vfs_cdev_ioctl(vfs_cdev_node_t *node, uint32_t cmd,
                    void *buf, size_t buf_len) {
    if (!node) return -1;
    if ((node->base.flags & 0x07u) != FS_CHARDEVICE) return -1;
    /* X bit guards ioctl access */
    if (!(node->base.mask & VFS_PERM_X)) return -1;
    if (!node->ops || !node->ops->ioctl) return -1;

    /* buf may be NULL for commands that take no argument — driver checks  */
    return node->ops->ioctl(node->drv_ctx, cmd, buf, buf_len);
}
