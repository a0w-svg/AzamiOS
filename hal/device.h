/* ============================================================================
 * AzamiOS — NT/WDM-Style Device Object Header
 * File: hal/device.h
 *
 * The device object represents an instance of a hardware or logical device
 * in the kernel's device tree.  Devices are organized as a tree rooted at
 * a virtual "System" device.  Each physical device discovered by a bus
 * driver (e.g. PCI) becomes a Physical Device Object (PDO) parented to
 * the bus device.  A function driver attaches a Functional Device Object
 * (FDO) to the PDO by stacking on top of it.
 *
 * Naming convention:
 *   \Device\<name>   — registered in the NT-style object manager namespace
 *
 * API summary:
 *   device_create(name, type, parent)   → device_t *
 *   device_destroy(dev)                 — unlink and free
 *   device_set_driver(dev, drv)         — attach a driver
 *   device_find(name)                   → device_t * (by name search)
 *   device_tree_root()                  → root device_t *
 *   device_dump_tree()                  — print device tree to console
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"

/* Forward declarations */
struct driver;
struct az_object;

/* ── Device type classification ──────────────────────────────────────────── */
typedef enum {
    DEVICE_TYPE_ROOT    = 0,   /* Virtual root device (system)            */
    DEVICE_TYPE_BUS     = 1,   /* Bus enumerator (PCI, USB, ISA)          */
    DEVICE_TYPE_BLOCK   = 2,   /* Block storage (disk, ramdisk)           */
    DEVICE_TYPE_CHAR    = 3,   /* Character/serial device                 */
    DEVICE_TYPE_DISPLAY = 4,   /* Display adapter / framebuffer           */
    DEVICE_TYPE_INPUT   = 5,   /* Input device (keyboard, mouse)          */
    DEVICE_TYPE_NETWORK = 6,   /* Network interface controller            */
    DEVICE_TYPE_AUDIO   = 7,   /* Audio device                            */
    DEVICE_TYPE_OTHER   = 255  /* Uncategorised                           */
} device_type_t;

/* ── Device flags ────────────────────────────────────────────────────────── */
#define DEVICE_FLAG_STARTED       (1U << 0)   /* Device has been started       */
#define DEVICE_FLAG_REMOVED       (1U << 1)   /* Device is being removed       */
#define DEVICE_FLAG_BUS_DRIVER    (1U << 2)   /* Device is a bus enumerator    */

/* ── Device Object ───────────────────────────────────────────────────────── */
typedef struct device {
    char             name[32];         /* Short device name (e.g. "PCI0")    */
    device_type_t    type;             /* Device type classification         */
    u32              flags;            /* DEVICE_FLAG_* bitmask              */

    struct driver   *driver;           /* Attached driver (NULL if none)     */
    void            *driver_data;      /* Driver-private extension data      */

    /* Device tree links (parent → children via first-child/sibling list) */
    struct device   *parent;           /* Parent device in tree              */
    struct device   *children;         /* First child device                 */
    struct device   *sibling;          /* Next sibling at same tree level    */

    /* Device stack (filter/function driver layering) */
    struct device   *stack_top;        /* Top of attached device stack       */
    struct device   *stack_next;       /* Next device down in the stack      */

    /* Object manager integration */
    struct az_object *obj;             /* Reference to \Device\<name> object */
} device_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * device_init() — Initialise the device tree subsystem.
 *
 * Creates the root "System" device.  Must be called once from hal_init().
 */
void device_init(void);

/**
 * device_create(name, type, parent) → device_t *
 *
 * Allocate a new device object and attach it as a child of @parent.
 * If @parent is NULL the device becomes a child of the root.
 * The device is registered in the object manager under "\Device\<name>".
 *
 * Returns NULL on allocation failure.
 */
device_t *device_create(const char *name, device_type_t type, device_t *parent);

/**
 * device_destroy(dev) — Unlink device from tree and free it.
 *
 * Also dereferences the object manager entry.  Does NOT destroy children;
 * the caller must walk the tree bottom-up if needed.
 */
void device_destroy(device_t *dev);

/**
 * device_set_driver(dev, drv) — Attach a driver to this device.
 */
void device_set_driver(device_t *dev, struct driver *drv);

/**
 * device_find(name) → device_t *
 *
 * Search the entire device tree for a device with the given name.
 * Returns NULL if not found.
 */
device_t *device_find(const char *name);

/**
 * device_tree_root() → device_t *
 *
 * Returns the root device of the device tree.
 */
device_t *device_tree_root(void);

/**
 * device_dump_tree() — Print the device tree to the kernel console.
 *
 * Useful for debugging — shows the full hierarchy with indentation.
 */
void device_dump_tree(void);
