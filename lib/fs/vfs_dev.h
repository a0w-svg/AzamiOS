/**
 * lib/fs/vfs_dev.h — Secure VFS Character Device Node
 *
 * Extends the existing vfs_node (fs_node_t) model to attach character-device
 * cdev_ops_t instances.  All I/O through this layer enforces:
 *   - Explicit size_t max_len / len bounds on read/write
 *   - Permission mask check (mode bits) before dispatching
 *   - NULL-vtable guard on every call path
 *
 * This header is kernel-independent (depends only on <stdint.h> and
 * the existing vfs.h), so userspace test harnesses can include it.
 */
#ifndef VFS_DEV_H
#define VFS_DEV_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"

/* ssize_t: not in freestanding <stddef.h>                               */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

/* Permission bits embedded in fs_node_t::mask */
#define VFS_PERM_R  0x04u   /* read permission   */
#define VFS_PERM_W  0x02u   /* write permission  */
#define VFS_PERM_X  0x01u   /* execute/ioctl     */

/* Forward: the driver-model cdev_ops_t lives in device.h (kernel-only).
 * For the lib layer we duplicate just the function-pointer signature so
 * vfs_dev.h stays kernel-independent.  The bridge fills in real pointers. */
struct vfs_cdev_ops {
    /* max_len enforced: at most max_len bytes written to buf             */
    ssize_t (*read )(void *drv_ctx, uint8_t *buf,
                     size_t max_len, uint64_t offset);
    /* len    enforced: at most len bytes read from buf                   */
    ssize_t (*write)(void *drv_ctx, const uint8_t *buf,
                     size_t len,    uint64_t offset);
    int     (*open )(void *drv_ctx, uint32_t flags);
    int     (*close)(void *drv_ctx);
    /* buf_len validated by callee before any dereference                 */
    int     (*ioctl)(void *drv_ctx, uint32_t cmd,
                     void *buf, size_t buf_len);
};

/**
 * vfs_cdev_node — a VFS node backed by a character device.
 *
 * Embed as private_data in an existing fs_node_t or allocate standalone.
 * The VFS bridge functions (below) wire vfs_cdev_*() into the standard
 * fs_node_t read/write/open/close function pointers.
 */
typedef struct vfs_cdev_node {
    fs_node_t               base;       /* MUST be first (cast-compatible) */
    const struct vfs_cdev_ops *ops;     /* pointer to driver vtable        */
    void                   *drv_ctx;    /* passed verbatim to ops->*()     */
    uint32_t                open_flags; /* flags passed to ops->open()     */
} vfs_cdev_node_t;

/* ── Secure VFS character-device API ────────────────────────────────── */

/**
 * vfs_cdev_read — bounds-enforced read dispatcher.
 *
 * @node     : must be a vfs_cdev_node_t (checked by type flag)
 * @buf      : destination; must be non-NULL
 * @max_len  : maximum bytes to write into buf — NEVER exceeded
 * @offset   : passed to driver (ignored for stream devices)
 *
 * Returns bytes read (>= 0) or -1 on error.
 * Rejects: NULL node/buf, zero max_len, missing write permission in mask.
 */
ssize_t vfs_cdev_read (vfs_cdev_node_t *node, uint8_t *buf,
                        size_t max_len, uint64_t offset);

/**
 * vfs_cdev_write — bounds-enforced write dispatcher.
 *
 * @node  : must be a vfs_cdev_node_t
 * @buf   : source; must be non-NULL
 * @len   : exact byte count to write — driver reads AT MOST len bytes
 *
 * Returns bytes written (>= 0) or -1 on error.
 */
ssize_t vfs_cdev_write(vfs_cdev_node_t *node, const uint8_t *buf,
                        size_t len, uint64_t offset);

/**
 * vfs_cdev_open  — open a character device node.
 */
int     vfs_cdev_open (vfs_cdev_node_t *node, uint32_t flags);

/**
 * vfs_cdev_close — close a character device node.
 */
int     vfs_cdev_close(vfs_cdev_node_t *node);

/**
 * vfs_cdev_ioctl — bounded ioctl dispatcher.
 *
 * @buf     : kernel pointer; may be NULL for argument-less commands
 * @buf_len : callee uses this to validate buf before dereferencing
 */
int     vfs_cdev_ioctl(vfs_cdev_node_t *node, uint32_t cmd,
                        void *buf, size_t buf_len);

/**
 * vfs_cdev_node_init — populate a vfs_cdev_node_t in-place.
 *
 * After calling this, the node's base fs_node_t read/write/open/close
 * function pointers dispatch through the secure vfs_cdev_* wrappers.
 */
void vfs_cdev_node_init(vfs_cdev_node_t *node,
                         const char *name,
                         uint32_t mask,          /* permission bits      */
                         const struct vfs_cdev_ops *ops,
                         void *drv_ctx);

#endif /* VFS_DEV_H */
