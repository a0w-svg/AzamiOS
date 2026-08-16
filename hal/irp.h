/* ============================================================================
 * AzamiOS — I/O Request Packet (IRP) Header
 * File: hal/irp.h
 *
 * An IRP is the fundamental unit of I/O work in the NT/WDM driver model.
 * When a process or the kernel needs to perform I/O on a device, it
 * allocates an IRP, fills in the operation parameters, and dispatches it
 * to the target device's driver.
 *
 * Current implementation: synchronous dispatch only.
 * Future: async dispatch with AZ_STATUS_PENDING + completion callback.
 *
 * API summary:
 *   irp_create(major)             → irp_t *  (kmalloc-backed)
 *   irp_free(irp)                 — return to kernel heap
 *   irp_dispatch(dev, irp)        → az_status_t
 *   irp_default_handler(dev,irp)  → AZ_ERROR_NOTSUP  (unimplemented fn)
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "driver.h"   /* irp_major_t */
#include "device.h"   /* device_t    */

/* ── PnP Minor Function Codes (used with IRP_MJ_PNP) ────────────────────── */
#define IRP_MN_START_DEVICE       0x00
#define IRP_MN_STOP_DEVICE        0x01
#define IRP_MN_REMOVE_DEVICE      0x02
#define IRP_MN_QUERY_DEVICE_ID    0x03
#define IRP_MN_QUERY_RESOURCES    0x04

/* ── I/O Request Packet ──────────────────────────────────────────────────── */
typedef struct irp {
    /* Operation descriptor */
    irp_major_t   major;              /* Major function code                */
    u32           minor;              /* Minor function (PnP, IOCTL sub)   */

    /* Completion status */
    az_status_t   status;             /* Set by the driver on completion    */

    /* I/O buffers */
    void         *system_buffer;      /* Kernel-side buffered I/O pointer   */
    size_t        buffer_length;      /* Total buffer capacity in bytes     */
    size_t        bytes_transferred;  /* Actual bytes moved by the driver   */

    /* Operation parameters */
    u64           offset;             /* File/device byte offset            */
    u32           ioctl_code;         /* IOCTL control code (IRP_MJ_IOCTL) */

    /* Target device */
    device_t     *target_device;      /* Device this IRP is dispatched to   */

    /* Caller context (opaque — used by the I/O manager) */
    void         *context;
} irp_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * irp_create(major) → irp_t *
 *
 * Allocate and zero-initialise a new IRP with the given major function.
 * Returns NULL on allocation failure.
 */
irp_t *irp_create(irp_major_t major);

/**
 * irp_free(irp) — Free an IRP previously allocated with irp_create().
 */
void irp_free(irp_t *irp);

/**
 * irp_dispatch(dev, irp) → az_status_t
 *
 * Dispatch the IRP to @dev's attached driver.  Calls the driver's
 * dispatch[irp->major] handler.  If the driver has no handler for this
 * major function, the default handler returns AZ_ERROR_NOTSUP.
 *
 * This function sets irp->target_device to @dev before dispatching.
 */
az_status_t irp_dispatch(device_t *dev, irp_t *irp);

/**
 * irp_default_handler(dev, irp) → AZ_ERROR_NOTSUP
 *
 * Default handler used for unimplemented major functions.
 */
az_status_t irp_default_handler(device_t *dev, irp_t *irp);
