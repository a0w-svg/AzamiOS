/* ============================================================================
 * AzamiOS — USB Core Subsystem Implementation
 * File: drivers/usb/core/usb.c
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "usb.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"

static dm_class_t g_usb_class = {
    .name = "usb",
};

static bool g_usb_core_initialized = false;

int usb_core_init(void)
{
    if (g_usb_core_initialized) return 0;

    int ret = dm_class_register(&g_usb_class);
    if (ret != 0) {
        pr_debug("[USB] Failed to register sysfs class 'usb': %d\n", ret);
        return ret;
    }

    g_usb_core_initialized = true;
    pr_debug("[USB] USB Core subsystem active (sysfs class /sys/class/usb)\n");
    return 0;
}

usb_device_t *usb_device_create(usb_bus_t *bus, u8 port, u8 speed)
{
    if (!bus) return NULL;

    usb_device_t *udev = (usb_device_t *)kmalloc(sizeof(usb_device_t));
    if (!udev) return NULL;

    memset(udev, 0, sizeof(*udev));
    udev->bus = bus;
    udev->port = port;
    udev->speed = speed;
    udev->address = 0; /* Default address until SET_ADDRESS */

    return udev;
}
