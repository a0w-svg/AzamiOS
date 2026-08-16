/* ============================================================================
 * AzamiOS — NT/WDM-Style Driver Object Header
 * File: hal/driver.h
 *
 * A driver object represents a loaded kernel driver.  It contains a
 * dispatch table of major function handlers (indexed by irp_major_t) and
 * an optional add_device callback invoked by the PnP manager when a
 * matching device is found.
 *
 * API summary:
 *   driver_register(drv)       — add driver to the global registry
 *   driver_unregister(drv)     — remove driver from registry
 *   driver_find(name)          → driver_t *
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"

/* Forward declarations */
struct device;
struct irp;

/* ── IRP Major Function Codes ────────────────────────────────────────────── */
typedef enum {
    IRP_MJ_CREATE   = 0,   /* Device handle opened (equivalent to open())  */
    IRP_MJ_CLOSE    = 1,   /* Device handle closed                        */
    IRP_MJ_READ     = 2,   /* Read data from device                       */
    IRP_MJ_WRITE    = 3,   /* Write data to device                        */
    IRP_MJ_IOCTL    = 4,   /* Device-specific I/O control                 */
    IRP_MJ_PNP      = 5,   /* Plug-and-Play notification                  */
    IRP_MJ_POWER    = 6,   /* Power management event                      */
    IRP_MJ_COUNT    = 7    /* Total major function slots                   */
} irp_major_t;

/* ── Driver dispatch function signature ──────────────────────────────────── */
/*
 * Each major function handler receives the target device and the IRP.
 * It should fill irp->status and irp->bytes_transferred, then return
 * the completion status (or AZ_STATUS_PENDING for async completion).
 */
typedef az_status_t (*driver_dispatch_fn)(struct device *dev, struct irp *irp);

/*
 * add_device is called by the PnP manager when a new device matching
 * this driver is discovered.  The driver should create its FDO and
 * attach it to the PDO.
 *
 * Note: uses the typedef name driver_t (declared below) via forward struct tag.
 */
struct driver;  /* ensure the 'struct driver' tag is known at file scope */
typedef az_status_t (*driver_add_device_fn)(struct driver *drv,
                                             struct device *pdo);

/* ── Driver Object ───────────────────────────────────────────────────────── */
typedef struct driver {
    char                  name[32];                  /* Driver name         */
    driver_dispatch_fn    dispatch[IRP_MJ_COUNT];    /* Major fn table      */
    driver_add_device_fn  add_device;                /* PnP add-device      */
    struct driver        *next;                      /* Registry list link   */
} driver_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * driver_init() — Initialise the driver registry.
 * Called once from hal_init().
 */
void driver_init(void);

/**
 * driver_register(drv) — Register a driver in the global driver list.
 *
 * The driver's dispatch table and add_device should be populated before
 * calling this.  Returns 0 on success, negative on error.
 */
az_status_t driver_register(driver_t *drv);

/**
 * driver_unregister(drv) — Remove a driver from the global registry.
 */
void driver_unregister(driver_t *drv);

/**
 * driver_find(name) → driver_t *
 *
 * Look up a registered driver by name.  Returns NULL if not found.
 */
driver_t *driver_find(const char *name);
