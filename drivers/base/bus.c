/* ============================================================================
 * AzamiOS — Driver Model Core: buses, drivers and binding
 * File: drivers/base/bus.c
 *
 * A bus owns two lists — its devices and its drivers — and the match() rule
 * that pairs them.  Binding is symmetric, mirroring Linux's device_attach() /
 * driver_attach() pair:
 *
 *   dm_device_register()  → dm_bus_probe_device()  walk the bus's drivers
 *   dm_driver_register()  → dm_driver_attach()     walk the bus's devices
 *
 * so a driver registered before its hardware is enumerated and a driver
 * registered after both bind at the moment the second half appears.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "base.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

static dm_bus_t  *g_buses;
static spinlock_t g_bus_lock = SPINLOCK_INIT;

/* ── Bus registry ────────────────────────────────────────────────────────── */

int dm_bus_register(dm_bus_t *bus)
{
    if (!bus || !bus->name || !bus->match) return -EINVAL;
    if (dm_bus_find(bus->name)) return -EEXIST;

    spinlock_lock(&g_bus_lock);
    bus->devices = NULL;
    bus->drivers = NULL;
    bus->next    = g_buses;
    g_buses      = bus;
    spinlock_unlock(&g_bus_lock);

    pr_debug("[DEVCORE] bus '%s' registered\n", bus->name);
    return 0;
}

dm_bus_t *dm_bus_find(const char *name)
{
    if (!name) return NULL;
    for (dm_bus_t *b = g_buses; b; b = b->next) {
        if (strcmp(b->name, name) == 0) return b;
    }
    return NULL;
}

dm_bus_t *dm_bus_nth(u32 n)
{
    u32 i = 0;
    for (dm_bus_t *b = g_buses; b; b = b->next, i++) {
        if (i == n) return b;
    }
    return NULL;
}

u32 dm_bus_count(void)
{
    u32 n = 0;
    for (dm_bus_t *b = g_buses; b; b = b->next) n++;
    return n;
}

/* ── Device list maintenance (called from core.c) ────────────────────────── */

void dm_bus_add_device(dm_bus_t *bus, dm_device_t *dev)
{
    spinlock_lock(&g_bus_lock);
    dm_device_t **tail = &bus->devices;
    while (*tail) tail = &(*tail)->bus_next;
    dev->bus_next = NULL;
    *tail = dev;
    spinlock_unlock(&g_bus_lock);
}

void dm_bus_del_device(dm_bus_t *bus, dm_device_t *dev)
{
    spinlock_lock(&g_bus_lock);
    dm_device_t **pp = &bus->devices;
    while (*pp) {
        if (*pp == dev) { *pp = dev->bus_next; break; }
        pp = &(*pp)->bus_next;
    }
    spinlock_unlock(&g_bus_lock);
}

/* ── Binding ─────────────────────────────────────────────────────────────── */

/*
 * Run the probe path for one (device, driver) pair.  The bus gets first
 * refusal via bus->probe() so it can do bus-wide setup (bus mastering, BAR
 * mapping) before the driver's own probe runs.  A failed probe leaves the
 * device unbound and available to the next driver.
 */
static int dm_really_probe(dm_device_t *dev, dm_driver_t *drv)
{
    dev->driver = drv;

    int ret;
    if (dev->bus->probe) {
        ret = dev->bus->probe(dev);
    } else if (drv->probe) {
        ret = drv->probe(dev);
    } else {
        ret = 0;
    }

    if (ret != 0) {
        dev->driver  = NULL;
        dev->drvdata = NULL;
        return ret;
    }

    drv->nbound++;
    pr_debug("[DEVCORE] %s: bound '%s' to %s\n", dev->bus->name, drv->name, dev->name);
    dm_uevent("bind", dev);
    return 0;
}

int dm_bus_probe_device(dm_device_t *dev)
{
    if (!dev || !dev->bus || dev->driver) return 0;

    for (dm_driver_t *drv = dev->bus->drivers; drv; drv = drv->bus_next) {
        if (!dev->bus->match(dev, drv)) continue;
        if (dm_really_probe(dev, drv) == 0) return 1;
    }
    return 0;
}

void dm_device_unbind(dm_device_t *dev)
{
    if (!dev || !dev->driver) return;

    dm_driver_t *drv = dev->driver;
    if (drv->remove) drv->remove(dev);
    if (drv->nbound) drv->nbound--;
    dev->driver  = NULL;
    dev->drvdata = NULL;
    dm_uevent("unbind", dev);
}

/* ── Driver registry ─────────────────────────────────────────────────────── */

int dm_driver_register(dm_driver_t *drv)
{
    if (!drv || !drv->name || !drv->bus) return -EINVAL;

    spinlock_lock(&g_bus_lock);
    dm_driver_t **tail = &drv->bus->drivers;
    while (*tail) {
        if (*tail == drv) { spinlock_unlock(&g_bus_lock); return -EEXIST; }
        tail = &(*tail)->bus_next;
    }
    drv->bus_next = NULL;
    *tail = drv;
    spinlock_unlock(&g_bus_lock);

    /* Adopt devices that were enumerated before this driver existed. */
    u32 bound = 0;
    for (dm_device_t *dev = drv->bus->devices; dev; dev = dev->bus_next) {
        if (dev->driver) continue;
        if (!drv->bus->match(dev, drv)) continue;
        if (dm_really_probe(dev, drv) == 0) bound++;
    }

    pr_debug("[DEVCORE] driver '%s' registered on bus '%s' (%u device%s bound)\n",
             drv->name, drv->bus->name, bound, bound == 1 ? "" : "s");
    return 0;
}

void dm_driver_unregister(dm_driver_t *drv)
{
    if (!drv || !drv->bus) return;

    for (dm_device_t *dev = drv->bus->devices; dev; dev = dev->bus_next) {
        if (dev->driver == drv) dm_device_unbind(dev);
    }

    spinlock_lock(&g_bus_lock);
    dm_driver_t **pp = &drv->bus->drivers;
    while (*pp) {
        if (*pp == drv) { *pp = drv->bus_next; break; }
        pp = &(*pp)->bus_next;
    }
    spinlock_unlock(&g_bus_lock);
}

dm_driver_t *dm_driver_nth(dm_bus_t *bus, u32 n)
{
    if (!bus) return NULL;
    u32 i = 0;
    for (dm_driver_t *d = bus->drivers; d; d = d->bus_next, i++) {
        if (i == n) return d;
    }
    return NULL;
}
