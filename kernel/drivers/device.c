/**
 * kernel/drivers/device.c — AzamiOS Driver / Device Registry
 *
 * Thread-safety: all registry mutations/lookups are protected by
 * g_dev_lock (spinlock_acquire_irqsave).  Monotonic major counter
 * ensures every driver gets a unique, never-reused major number.
 */
#include "include/device.h"
#include "../arch/include/spinlock.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

/* ── Registry state ──────────────────────────────────────────────────── */
#define MAX_DRIVERS  32
#define MAX_DEVICES  64

static driver_t  *g_drv_head   = (void*)0;
static device_t  *g_dev_head   = (void*)0;
static volatile int g_dev_lock = 0;

/* Monotonic counters; 0 is reserved ("no device")                       */
static uint16_t g_next_major = 1; /* increments on each drv_register()  */
static uint16_t g_next_minor = 1; /* per-driver minor counter           */

static int g_drv_count = 0;
static int g_dev_count = 0;

/* ── Driver registry ─────────────────────────────────────────────────── */

int drv_register(driver_t *drv) {
    if (!drv) return -1;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    if (g_drv_count >= MAX_DRIVERS) {
        spinlock_release_irqrestore(&g_dev_lock, flags);
        kprintf("drv: registry full, cannot register '%s'\n", drv->name);
        return -1;
    }

    /* Assign monotonic major — never reused even after unregister       */
    drv->major = g_next_major++;
    drv->next  = g_drv_head;
    g_drv_head = drv;
    g_drv_count++;

    spinlock_release_irqrestore(&g_dev_lock, flags);

    kprintf("drv: registered driver '%s' major=%u\n", drv->name, drv->major);
    return 0;
}

void drv_unregister(driver_t *drv) {
    if (!drv) return;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    driver_t **pp = &g_drv_head;
    while (*pp) {
        if (*pp == drv) {
            *pp = drv->next;
            drv->next = (void*)0;
            g_drv_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    spinlock_release_irqrestore(&g_dev_lock, flags);
    kprintf("drv: unregistered driver '%s'\n", drv->name);
}

/* ── Device registry ─────────────────────────────────────────────────── */

int dev_add(device_t *dev, driver_t *drv) {
    if (!dev || !drv) return -1;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    if (g_dev_count >= MAX_DEVICES) {
        spinlock_release_irqrestore(&g_dev_lock, flags);
        kprintf("drv: device table full, cannot add '%s'\n", dev->name);
        return -1;
    }

    /* Assign MKDEV using driver major + monotonic minor                  */
    dev->devno       = MKDEV(drv->major, g_next_minor++);
    dev->drv         = drv;
    dev->cls         = drv->cls;
    dev->initialized = false;
    dev->next        = g_dev_head;
    g_dev_head       = dev;
    g_dev_count++;

    spinlock_release_irqrestore(&g_dev_lock, flags);

    /* Call driver probe outside the lock (may do I/O)                   */
    if (drv->probe) {
        int rc = drv->probe(drv, dev);
        if (rc != 0) {
            kprintf("drv: probe failed for '%s' (rc=%d)\n", dev->name, rc);
            dev_remove(dev);
            return -1;
        }
    }

    dev->initialized = true;
    kprintf("drv: device '%s' added dev=%04x:%04x\n",
            dev->name, MAJOR(dev->devno), MINOR(dev->devno));
    return 0;
}

void dev_remove(device_t *dev) {
    if (!dev) return;

    /* Notify driver first (may flush buffers etc.)                       */
    if (dev->drv && dev->drv->remove)
        dev->drv->remove(dev->drv, dev);

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    device_t **pp = &g_dev_head;
    while (*pp) {
        if (*pp == dev) {
            *pp = dev->next;
            dev->next = (void*)0;
            g_dev_count--;
            break;
        }
        pp = &(*pp)->next;
    }

    dev->devno       = 0;
    dev->initialized = false;
    spinlock_release_irqrestore(&g_dev_lock, flags);

    kprintf("drv: device '%s' removed\n", dev->name);
}

device_t *dev_lookup_by_name(const char *name) {
    if (!name) return (void*)0;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    device_t *d = g_dev_head;
    while (d) {
        if (strncmp(d->name, name, DEVICE_NAME_MAX) == 0) break;
        d = d->next;
    }

    spinlock_release_irqrestore(&g_dev_lock, flags);
    return d;
}

device_t *dev_lookup_by_number(dev_t devno) {
    if (devno == 0) return (void*)0;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    device_t *d = g_dev_head;
    while (d) {
        if (d->devno == devno) break;
        d = d->next;
    }

    spinlock_release_irqrestore(&g_dev_lock, flags);
    return d;
}

void drv_registry_dump(void) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_dev_lock, &flags);

    kprintf("=== Driver Registry (%d drivers, %d devices) ===\n",
            g_drv_count, g_dev_count);

    driver_t *drv = g_drv_head;
    while (drv) {
        kprintf("  DRV %-24s major=%u cls=%d\n",
                drv->name, drv->major, (int)drv->cls);
        drv = drv->next;
    }

    device_t *dev = g_dev_head;
    while (dev) {
        kprintf("  DEV %-24s %04x:%04x init=%d\n",
                dev->name, MAJOR(dev->devno), MINOR(dev->devno),
                (int)dev->initialized);
        dev = dev->next;
    }

    spinlock_release_irqrestore(&g_dev_lock, flags);
}
