/* ============================================================================
 * AzamiOS — Driver Model Core: devices and classes
 * File: drivers/base/core.c
 *
 * Device registration, the global/class device lists, and the generic sysfs
 * attributes every device carries.  Bus and driver bookkeeping lives in
 * bus.c; the two halves meet in dm_device_register(), which publishes the
 * device and then asks the bus to find it a driver.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "base.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

/* Implemented in bus.c — offers @dev to every driver on its bus. */
int dm_bus_probe_device(dm_device_t *dev);
void dm_bus_add_device(dm_bus_t *bus, dm_device_t *dev);
void dm_bus_del_device(dm_bus_t *bus, dm_device_t *dev);
void dm_device_unbind(dm_device_t *dev);

static dm_device_t *g_all_devices;      /* registration-ordered global list */
static dm_class_t  *g_classes;
static spinlock_t   g_core_lock = SPINLOCK_INIT;
static bool         g_core_ready;

const char *const dm_device_generic_attrs[] = {
    "uevent", "modalias", "driver", "dev", NULL
};

void driver_core_init(void)
{
    if (g_core_ready) return;
    g_all_devices = NULL;
    g_classes     = NULL;
    g_core_ready  = true;
    pr_debug("[DEVCORE] Unified driver model online (bus/class/device binding)\n");
}

/* ── Classes ─────────────────────────────────────────────────────────────── */

int dm_class_register(dm_class_t *cls)
{
    if (!cls || !cls->name) return -EINVAL;
    if (dm_class_find(cls->name)) return -EEXIST;

    spinlock_lock(&g_core_lock);
    cls->devices = NULL;
    cls->next    = g_classes;
    g_classes    = cls;
    spinlock_unlock(&g_core_lock);

    pr_debug("[DEVCORE] class '%s' registered\n", cls->name);
    return 0;
}

dm_class_t *dm_class_find(const char *name)
{
    if (!name) return NULL;
    for (dm_class_t *c = g_classes; c; c = c->next) {
        if (strcmp(c->name, name) == 0) return c;
    }
    return NULL;
}

dm_class_t *dm_class_nth(u32 n)
{
    u32 i = 0;
    for (dm_class_t *c = g_classes; c; c = c->next, i++) {
        if (i == n) return c;
    }
    return NULL;
}

u32 dm_class_count(void)
{
    u32 n = 0;
    for (dm_class_t *c = g_classes; c; c = c->next) n++;
    return n;
}

dm_device_t *dm_class_device_nth(dm_class_t *cls, u32 n)
{
    if (!cls) return NULL;
    u32 i = 0;
    for (dm_device_t *d = cls->devices; d; d = d->class_next, i++) {
        if (i == n) return d;
    }
    return NULL;
}

int dm_device_add_class(dm_device_t *dev, dm_class_t *cls, u32 devt)
{
    if (!dev || !cls) return -EINVAL;
    if (dev->cls) return -EBUSY;

    spinlock_lock(&g_core_lock);
    dev->cls  = cls;
    dev->devt = devt;
    /* Append so /sys/class listings follow registration order. */
    dm_device_t **tail = &cls->devices;
    while (*tail) tail = &(*tail)->class_next;
    dev->class_next = NULL;
    *tail = dev;
    spinlock_unlock(&g_core_lock);
    return 0;
}

static void dm_device_del_class(dm_device_t *dev)
{
    if (!dev->cls) return;
    dm_device_t **pp = &dev->cls->devices;
    while (*pp) {
        if (*pp == dev) { *pp = dev->class_next; break; }
        pp = &(*pp)->class_next;
    }
    dev->cls        = NULL;
    dev->class_next = NULL;
}

/* ── Devices ─────────────────────────────────────────────────────────────── */

/*
 * @bus may be NULL for a class-only device — one that exists purely as a
 * functional endpoint (a DRM card, an input event node) with no bus to
 * enumerate or bind it, exactly as in Linux.
 */
dm_device_t *dm_device_alloc(const char *name, dm_bus_t *bus)
{
    if (!name) return NULL;

    dm_device_t *dev = (dm_device_t *)kzalloc(sizeof(dm_device_t));
    if (!dev) return NULL;

    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->bus = bus;
    return dev;
}

int dm_device_register(dm_device_t *dev)
{
    if (!dev) return -EINVAL;
    if (dev->bus && dm_device_find(dev->bus, dev->name)) return -EEXIST;

    spinlock_lock(&g_core_lock);
    dm_device_t **tail = &g_all_devices;
    while (*tail) tail = &(*tail)->all_next;
    dev->all_next = NULL;
    *tail = dev;
    spinlock_unlock(&g_core_lock);

    if (dev->bus) dm_bus_add_device(dev->bus, dev);
    dm_uevent("add", dev);

    /* Whichever of {device, driver} arrives second drives the binding. */
    if (dev->bus) dm_bus_probe_device(dev);
    return 0;
}

void dm_device_unregister(dm_device_t *dev)
{
    if (!dev) return;

    dm_device_unbind(dev);
    dm_uevent("remove", dev);

    spinlock_lock(&g_core_lock);
    dm_device_del_class(dev);
    dm_device_t **pp = &g_all_devices;
    while (*pp) {
        if (*pp == dev) { *pp = dev->all_next; break; }
        pp = &(*pp)->all_next;
    }
    spinlock_unlock(&g_core_lock);

    if (dev->bus) dm_bus_del_device(dev->bus, dev);
    kfree(dev);
}

dm_device_t *dm_device_find(dm_bus_t *bus, const char *name)
{
    if (!bus || !name) return NULL;
    for (dm_device_t *d = bus->devices; d; d = d->bus_next) {
        if (strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

dm_device_t *dm_device_nth(dm_bus_t *bus, u32 n)
{
    if (!bus) return NULL;
    u32 i = 0;
    for (dm_device_t *d = bus->devices; d; d = d->bus_next, i++) {
        if (i == n) return d;
    }
    return NULL;
}

/* ── Generic sysfs attributes ────────────────────────────────────────────── */

int dm_device_attr_show(dm_device_t *dev, const char *attr, char *buf, size_t len)
{
    if (!dev || !attr || !buf || len == 0) return -1;

    if (strcmp(attr, "modalias") == 0) {
        return scnprintf(buf, len, "%s\n",
                        dev->modalias[0] ? dev->modalias : dev->name);
    }
    if (strcmp(attr, "driver") == 0) {
        return scnprintf(buf, len, "%s\n",
                        dev->driver ? dev->driver->name : "(unbound)");
    }
    if (strcmp(attr, "dev") == 0) {
        return scnprintf(buf, len, "%u:%u\n", dev->devt >> 20, dev->devt & 0xFFFFF);
    }
    if (strcmp(attr, "uevent") == 0) {
        const char *subsys = dev->cls ? dev->cls->name
                           : dev->bus ? dev->bus->name : "device";
        int n = scnprintf(buf, len,
                         "DEVPATH=/devices/%s/%s\nSUBSYSTEM=%s\nDEVNAME=%s\n"
                         "MODALIAS=%s\nDRIVER=%s\n",
                         subsys, dev->name, subsys,
                         dev->name,
                         dev->modalias[0] ? dev->modalias : dev->name,
                         dev->driver ? dev->driver->name : "");
        if (n < 0) return -1;
        if (dev->devt && (size_t)n < len) {
            n += scnprintf(buf + n, len - (size_t)n, "MAJOR=%u\nMINOR=%u\n",
                          dev->devt >> 20, dev->devt & 0xFFFFF);
        }
        if (dev->bus && dev->bus->uevent && (size_t)n < len) {
            int extra = dev->bus->uevent(dev, buf + n, len - (size_t)n);
            if (extra > 0) n += extra;
        }
        return n;
    }

    if (dev->bus && dev->bus->attr_show) {
        return dev->bus->attr_show(dev, attr, buf, len);
    }
    return -1;
}
