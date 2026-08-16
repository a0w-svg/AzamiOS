/* ============================================================================
 * AzamiOS — I/O Request Packet (IRP) Implementation
 * File: hal/irp.c
 *
 * Provides IRP allocation, dispatch, and the default unimplemented handler.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "irp.h"
#include "../kernel/mm/kmalloc.h"
#include "../drivers/char/console.h"


/* ── Major function name table (for diagnostics) ─────────────────────────── */
static const char *irp_major_name(irp_major_t mj)
{
    switch (mj) {
    case IRP_MJ_CREATE:  return "CREATE";
    case IRP_MJ_CLOSE:   return "CLOSE";
    case IRP_MJ_READ:    return "READ";
    case IRP_MJ_WRITE:   return "WRITE";
    case IRP_MJ_IOCTL:   return "IOCTL";
    case IRP_MJ_PNP:     return "PNP";
    case IRP_MJ_POWER:   return "POWER";
    default:             return "UNKNOWN";
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

irp_t *irp_create(irp_major_t major)
{
    if ((u32)major >= IRP_MJ_COUNT) return NULL;

    irp_t *irp = (irp_t *)kzalloc(sizeof(irp_t));
    if (!irp) return NULL;

    irp->major  = major;
    irp->status = AZ_STATUS_SUCCESS;

    return irp;
}

void irp_free(irp_t *irp)
{
    if (irp) kfree(irp);
}

az_status_t irp_default_handler(device_t *dev, irp_t *irp)
{
    (void)dev;
    irp->status = AZ_ERROR_NOTSUP;
    irp->bytes_transferred = 0;
    return AZ_ERROR_NOTSUP;
}

az_status_t irp_dispatch(device_t *dev, irp_t *irp)
{
    if (!dev || !irp) return AZ_ERROR_INVAL;

    irp->target_device = dev;

    /* Walk up the device stack to find the topmost device with a driver.
     * In the NT model, IRPs are sent to the top of the device stack and
     * flow downward.  For Phase 1 (no filter drivers), we just use the
     * device's own driver or its stack_top. */
    device_t *target = dev->stack_top ? dev->stack_top : dev;

    if (!target->driver) {
        pr_debug("[IRP] No driver attached to device '%s' for IRP_MJ_%s\n",
                dev->name, irp_major_name(irp->major));
        irp->status = AZ_ERROR_NOTSUP;
        return AZ_ERROR_NOTSUP;
    }

    /* Bounds check the major function index */
    if ((u32)irp->major >= IRP_MJ_COUNT) {
        irp->status = AZ_ERROR_INVAL;
        return AZ_ERROR_INVAL;
    }

    /* Dispatch to the driver's major function handler.
     * If the driver hasn't registered a handler for this function,
     * use the default handler. */
    driver_dispatch_fn handler = target->driver->dispatch[irp->major];
    if (!handler) handler = irp_default_handler;

    az_status_t result = handler(target, irp);
    irp->status = result;

    return result;
}
