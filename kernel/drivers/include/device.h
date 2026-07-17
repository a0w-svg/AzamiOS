/**
 * kernel/drivers/include/device.h — AzamiOS Unified Driver Model
 *
 * Provides:
 *   - Monotonic major/minor device numbering via MKDEV/MAJOR/MINOR
 *   - driver_t: registration descriptor with probe/remove lifecycle and cdev_ops vtable
 *   - device_t: live instance bound to a driver
 *   - cdev_ops_t: character-device vtable with explicit size-bounded I/O
 */
#ifndef AZAMI_DEVICE_H
#define AZAMI_DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ssize_t: not in freestanding <stddef.h>; define once here             */
#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

/* ── Device numbering ────────────────────────────────────────────────── */
typedef uint32_t dev_t;
#define MKDEV(major, minor) (((dev_t)(major) << 16) | ((dev_t)(minor) & 0xFFFFu))
#define MAJOR(dev)          ((uint16_t)((dev) >> 16))
#define MINOR(dev)          ((uint16_t)((dev) & 0xFFFFu))

/* Device class tags */
typedef enum {
    DEV_CLASS_CHAR   = 0,
    DEV_CLASS_BLOCK  = 1,
    DEV_CLASS_NET    = 2,
    DEV_CLASS_PSEUDO = 3,
} dev_class_t;

struct device;
struct driver;

/* ── Character device operations vtable ─────────────────────────────── */
/* All I/O sizes are explicit; no unbounded raw-pointer operations.      */
typedef struct cdev_ops {
    /* read:  writes at most max_len bytes into buf; returns bytes read  */
    ssize_t (*read )(struct device *dev, uint8_t *buf,
                     size_t max_len, uint64_t offset);
    /* write: reads at most len bytes from buf; returns bytes written    */
    ssize_t (*write)(struct device *dev, const uint8_t *buf,
                     size_t len,     uint64_t offset);
    /* open/close: return 0 on success, -1 on error                     */
    int     (*open )(struct device *dev, uint32_t flags);
    int     (*close)(struct device *dev);
    /* ioctl: buf_len explicitly bounds the buf pointer                  */
    int     (*ioctl)(struct device *dev, uint32_t cmd,
                     void *buf, size_t buf_len);
} cdev_ops_t;

/* ── Driver descriptor ───────────────────────────────────────────────── */
#define DRIVER_NAME_MAX 32

typedef struct driver {
    char        name[DRIVER_NAME_MAX]; /* "secure_uart", "null", ...  */
    dev_class_t cls;
    uint16_t    major;                 /* assigned at drv_register()  */
    int (*probe )(struct driver *drv, struct device *dev);
    int (*remove)(struct driver *drv, struct device *dev);
    const cdev_ops_t *cdev_ops;        /* NULL for non-char drivers   */
    struct driver *next;
} driver_t;

/* ── Live device instance ────────────────────────────────────────────── */
#define DEVICE_NAME_MAX 32

typedef struct device {
    char       name[DEVICE_NAME_MAX];  /* "ttyS0", "null", ...        */
    dev_t      devno;                  /* MKDEV(major, minor)         */
    dev_class_t cls;
    driver_t  *drv;
    void      *private_data;
    bool       initialized;
    struct device *next;
} device_t;

/* ── Registry API ────────────────────────────────────────────────────── */
int       drv_register         (driver_t *drv);
void      drv_unregister       (driver_t *drv);
int       dev_add              (device_t *dev, driver_t *drv);
void      dev_remove           (device_t *dev);
device_t *dev_lookup_by_name   (const char *name);
device_t *dev_lookup_by_number (dev_t devno);
void      drv_registry_dump    (void);

#endif /* AZAMI_DEVICE_H */
