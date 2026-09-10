/* ============================================================================
 * AzamiOS — Linux-Style Unified Driver Model ("driver core")
 * File: drivers/base/base.h
 *
 * The HAL device tree (hal/device.h) describes *topology*: who is plugged
 * into what.  The driver model layered on top describes *binding*: which
 * driver drives which device, on which bus, in which class.  It mirrors the
 * Linux `drivers/base` core:
 *
 *   dm_bus_t     ≈ struct bus_type      — a namespace of devices + drivers
 *                                         plus the match() rule that pairs
 *                                         them ("pci", "platform", "virtio")
 *   dm_driver_t  ≈ struct device_driver — probe/remove callbacks + id table
 *   dm_device_t  ≈ struct device        — a bindable device on a bus
 *   dm_class_t   ≈ struct class         — a *functional* grouping exported
 *                                         as /sys/class/<name> ("drm", "net")
 *
 * Binding is symmetric and order-independent, exactly as in Linux: register a
 * device and the core walks the bus's drivers looking for a match; register a
 * driver and the core walks the bus's already-present devices.  Whichever
 * happens second triggers probe(), so drivers and buses can initialise in any
 * order.
 *
 * Everything registered here is visible through sysfs:
 *   /sys/bus/<bus>/devices/<dev>      device attributes (uevent, modalias, …)
 *   /sys/bus/<bus>/drivers/<drv>      one directory per registered driver
 *   /sys/class/<class>/<dev>          functional view (drm/, net/, block/, …)
 *   /sys/devices/<bus>/<dev>          topological view
 *
 * …and every add/remove emits a uevent readable from /dev/kevent, which is
 * this kernel's stand-in for Linux's netlink hotplug socket.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../../hal/device.h"

struct dm_bus;
struct dm_driver;
struct dm_class;

#define DM_NAME_MAX      48
#define DM_MODALIAS_MAX  72

/* ── Device: one bindable node on a bus ──────────────────────────────────── */
typedef struct dm_device {
    char                name[DM_NAME_MAX];      /* bus-unique, "0000:00:02.0" */
    char                modalias[DM_MODALIAS_MAX];
    struct dm_bus      *bus;                    /* owning bus (never NULL)     */
    struct dm_driver   *driver;                 /* bound driver, NULL if none  */
    struct dm_class    *cls;                    /* /sys/class membership       */
    struct dm_device   *parent;                 /* logical parent device       */
    device_t           *hal;                    /* HAL topology node, may be 0 */

    void               *bus_data;               /* bus-private descriptor      */
    void               *drvdata;                /* driver-private state        */
    u32                 devt;                   /* MAJOR<<20 | MINOR, 0 = none */

    struct dm_device   *bus_next;               /* link on bus->devices        */
    struct dm_device   *class_next;             /* link on cls->devices        */
    struct dm_device   *all_next;               /* link on the global list     */
} dm_device_t;

/* ── Driver: probe/remove callbacks bound to devices by the bus match() ──── */
typedef struct dm_driver {
    const char         *name;
    struct dm_bus      *bus;
    const void         *id_table;   /* bus-specific, consumed by bus->match() */
    int               (*probe)(dm_device_t *dev);
    void              (*remove)(dm_device_t *dev);
    void              (*shutdown)(dm_device_t *dev);
    u32                 nbound;     /* devices currently bound to this driver */
    struct dm_driver   *bus_next;
} dm_driver_t;

/* ── Bus: a device/driver namespace plus its matching rule ───────────────── */
typedef struct dm_bus {
    const char         *name;
    /* Does @drv claim @dev?  Consults drv->id_table and dev->bus_data. */
    bool              (*match)(dm_device_t *dev, dm_driver_t *drv);
    /* Optional bus wrapper run instead of drv->probe() (enable bus mastering,
     * map BARs, …).  When NULL the core calls drv->probe() directly. */
    int               (*probe)(dm_device_t *dev);
    /* Optional: append bus-specific KEY=VALUE lines to a uevent/attribute. */
    int               (*uevent)(dm_device_t *dev, char *buf, size_t len);
    /* Optional: render one sysfs attribute.  Return bytes written, or -1 if
     * the attribute is not one of this bus's. */
    int               (*attr_show)(dm_device_t *dev, const char *attr,
                                   char *buf, size_t len);
    /* Optional: NULL-terminated list of bus-specific attribute names to list
     * in the device's sysfs directory, on top of the generic ones. */
    const char *const  *dev_attrs;

    dm_device_t        *devices;
    dm_driver_t        *drivers;
    struct dm_bus      *next;
} dm_bus_t;

/* ── Class: functional grouping, /sys/class/<name>/<device> ─────────────── */
typedef struct dm_class {
    const char         *name;
    dm_device_t        *devices;
    struct dm_class    *next;
} dm_class_t;

/* ── Core lifecycle ──────────────────────────────────────────────────────── */

/** driver_core_init() — bring up the driver model; call before any bus. */
void driver_core_init(void);

/* ── Buses ───────────────────────────────────────────────────────────────── */
int        dm_bus_register(dm_bus_t *bus);
dm_bus_t  *dm_bus_find(const char *name);
dm_bus_t  *dm_bus_nth(u32 n);
u32        dm_bus_count(void);

/* ── Classes ─────────────────────────────────────────────────────────────── */
int         dm_class_register(dm_class_t *cls);
dm_class_t *dm_class_find(const char *name);
dm_class_t *dm_class_nth(u32 n);
u32         dm_class_count(void);

/* ── Devices ─────────────────────────────────────────────────────────────── */

/**
 * dm_device_alloc(name, bus) → dm_device_t *
 *
 * Allocate an unregistered device.  Fill in bus_data/hal/modalias, then hand
 * it to dm_device_register().  Returns NULL on allocation failure.
 */
dm_device_t *dm_device_alloc(const char *name, dm_bus_t *bus);

/**
 * dm_device_register(dev) — publish the device.
 *
 * Adds it to its bus, emits an "add" uevent, then tries every driver on the
 * bus until one matches and probes successfully.  Returns 0 when the device
 * was published (whether or not a driver bound), negative on error.
 */
int  dm_device_register(dm_device_t *dev);

/** dm_device_unregister(dev) — unbind, emit "remove", unlink and free. */
void dm_device_unregister(dm_device_t *dev);

/** dm_device_add_class(dev, cls, devt) — join /sys/class/<cls>. */
int  dm_device_add_class(dm_device_t *dev, dm_class_t *cls, u32 devt);

/** dm_device_find(bus, name) → device on that bus, or NULL. */
dm_device_t *dm_device_find(dm_bus_t *bus, const char *name);

/** dm_device_nth(bus, n) → n-th device on the bus in registration order. */
dm_device_t *dm_device_nth(dm_bus_t *bus, u32 n);

/** dm_class_device_nth(cls, n) → n-th device in the class. */
dm_device_t *dm_class_device_nth(dm_class_t *cls, u32 n);

static inline void *dm_get_drvdata(dm_device_t *dev) { return dev->drvdata; }
static inline void  dm_set_drvdata(dm_device_t *dev, void *p) { dev->drvdata = p; }

/* ── Drivers ─────────────────────────────────────────────────────────────── */

/**
 * dm_driver_register(drv) — publish a driver and probe unbound devices.
 *
 * Every already-registered device on drv->bus is offered to the driver, so
 * driver and device registration order does not matter.
 */
int  dm_driver_register(dm_driver_t *drv);
void dm_driver_unregister(dm_driver_t *drv);
dm_driver_t *dm_driver_nth(dm_bus_t *bus, u32 n);

/* ── sysfs support ───────────────────────────────────────────────────────── */

/**
 * dm_device_attr_show(dev, attr, buf, len) → bytes written, or -1.
 *
 * Renders a generic device attribute ("uevent", "modalias", "driver",
 * "devt", "name"), falling back to bus->attr_show() for bus-specific ones.
 */
int dm_device_attr_show(dm_device_t *dev, const char *attr, char *buf, size_t len);

/** Generic per-device attribute names, NULL-terminated. */
extern const char *const dm_device_generic_attrs[];

/* ── Hotplug uevents (drivers/base/uevent.c) ─────────────────────────────── */

/** dm_uevent(action, dev) — queue a hotplug event ("add", "remove", …). */
void dm_uevent(const char *action, dm_device_t *dev);

/** dm_uevent_seqnum() — number of events generated since boot. */
u64  dm_uevent_seqnum(void);

/** uevent_init() — register /dev/kevent, the hotplug event stream. */
void uevent_init(void);

/* ── Built-in buses ──────────────────────────────────────────────────────── */
extern dm_bus_t dm_platform_bus;
extern dm_bus_t dm_pci_bus;

/** platform_bus_init() — register the platform bus and its legacy devices. */
void platform_bus_init(void);

/** pci_bus_init() — register the PCI bus and adopt HAL-enumerated devices. */
void pci_bus_init(void);
