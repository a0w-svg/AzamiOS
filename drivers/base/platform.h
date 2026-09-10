/* ============================================================================
 * AzamiOS — Platform Bus (non-discoverable devices)
 * File: drivers/base/platform.h
 *
 * The platform bus is where devices live that no bus can enumerate: the
 * PS/2 controller, the CMOS RTC, the 8250 UARTs, the PC speaker, the HPET,
 * the bootloader's framebuffer.  Firmware or board code declares them with
 * fixed resources, and drivers claim them by name — the same contract as
 * Linux's platform_device / platform_driver.
 * ============================================================================ */
#pragma once

#include "base.h"

/* ── Resource kinds ──────────────────────────────────────────────────────── */
#define PLATFORM_RES_MEM   0    /* MMIO window   */
#define PLATFORM_RES_IO    1    /* port I/O range */
#define PLATFORM_RES_IRQ   2    /* interrupt line */

typedef struct platform_resource {
    u32  type;
    u64  start;
    u64  size;      /* IRQ resources use size == 1 */
    const char *name;
} platform_resource_t;

typedef struct platform_device {
    const char           *name;      /* driver-matching name, e.g. "serial8250" */
    int                   id;        /* instance number, -1 for a singleton     */
    const platform_resource_t *res;
    u32                   nres;
    void                 *pdata;     /* board-specific data handed to probe()   */
    dm_device_t          *dev;       /* driver-model device, set on register    */
} platform_device_t;

typedef struct platform_driver {
    dm_driver_t drv;                 /* .name is the matched device name        */
    int  (*probe)(platform_device_t *pdev);
    void (*remove)(platform_device_t *pdev);
} platform_driver_t;

/**
 * platform_device_register(pdev) — publish a non-discoverable device.
 *
 * Creates "<name>.<id>" on the platform bus and runs the usual probe path.
 */
int platform_device_register(platform_device_t *pdev);

/** platform_driver_register(pdrv) — claim platform devices named pdrv->drv.name. */
int platform_driver_register(platform_driver_t *pdrv);

/** platform_get_resource(pdev, type, index) → resource, or NULL. */
const platform_resource_t *platform_get_resource(platform_device_t *pdev,
                                                 u32 type, u32 index);

/** platform_get_irq(pdev, index) → IRQ line, or -1. */
int platform_get_irq(platform_device_t *pdev, u32 index);

/** to_platform_device(dev) → the platform_device behind a driver-model device. */
static inline platform_device_t *to_platform_device(dm_device_t *dev)
{
    return (platform_device_t *)dev->bus_data;
}
