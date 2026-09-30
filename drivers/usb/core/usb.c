/* ============================================================================
 * AzamiOS — USB Core Subsystem Implementation
 * File: drivers/usb/core/usb.c
 *
 * Enumeration past the point of addressing (which each host controller does
 * its own way — xHCI in hardware, via Address Device), common to every
 * controller: device descriptor, product string, configuration 1, and class
 * driver binding per interface. See usb.h for the controller/core split.
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

/* Class drivers tried, in order, for every interface of every device. */
static const usb_class_driver_t *const g_class_drivers[] = {
    &usbhid_driver,
    &usb_storage_driver,
};

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

const char *usb_speed_name(u8 speed)
{
    switch (speed) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_FULL:  return "full";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "unknown";
    }
}

/* ── Transfers ────────────────────────────────────────────────────────────── */

int usb_control_msg(usb_device_t *dev, u8 request_type, u8 request,
                    u16 value, u16 index, void *data, u16 len)
{
    if (!dev || !dev->bus || !dev->bus->ops || !dev->bus->ops->control) return -ENODEV;
    usb_setup_packet_t setup = {
        .bmRequestType = request_type,
        .bRequest      = request,
        .wValue        = value,
        .wIndex        = index,
        .wLength       = len,
    };
    return dev->bus->ops->control(dev, &setup, data);
}

int usb_get_descriptor(usb_device_t *dev, u8 type, u8 index, void *buf, u16 len)
{
    /* String descriptors are asked for in US English, which every device
     * with strings at all is required to support; others ignore wIndex. */
    u16 lang = (type == USB_DESC_STRING && index != 0) ? 0x0409 : 0;
    return usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                           USB_REQ_GET_DESCRIPTOR, (u16)((type << 8) | index),
                           lang, buf, len);
}

/* A string descriptor is UTF-16LE; keep the ASCII subset, '?' for the rest. */
static void usb_read_string(usb_device_t *dev, u8 index, char *out, size_t out_len)
{
    out[0] = '\0';
    if (!index || out_len == 0) return;
    u8 buf[128];
    int n = usb_get_descriptor(dev, USB_DESC_STRING, index, buf, sizeof(buf));
    if (n < 2 || buf[1] != USB_DESC_STRING) return;
    int len = buf[0] < n ? buf[0] : n;
    size_t o = 0;
    for (int i = 2; i + 1 < len && o + 1 < out_len; i += 2) {
        u16 ch = (u16)(buf[i] | (buf[i + 1] << 8));
        out[o++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    out[o] = '\0';
}

/* ── Enumeration ──────────────────────────────────────────────────────────── */

/* Offer each interface of the active configuration to the class drivers. */
static void usb_bind_interfaces(usb_device_t *dev)
{
    const u8 *p = dev->config, *end = dev->config + dev->config_len;
    while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
        if (p[1] == USB_DESC_INTERFACE && p[0] >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *intf = (const usb_interface_descriptor_t *)p;
            /* Only the default alternate setting is active after
             * SET_CONFIGURATION; the others describe modes to switch to. */
            if (intf->bAlternateSetting == 0 && dev->num_bound < USB_MAX_INTERFACES) {
                for (size_t i = 0; i < sizeof(g_class_drivers) / sizeof(g_class_drivers[0]); i++) {
                    void *priv = NULL;
                    if (g_class_drivers[i]->probe(dev, intf, dev->config, dev->config_len, &priv) == 0) {
                        dev->drivers[dev->num_bound]     = g_class_drivers[i];
                        dev->driver_priv[dev->num_bound] = priv;
                        dev->num_bound++;
                        break;
                    }
                }
            }
        }
        p += p[0];
    }
}

int usb_new_device(usb_device_t *dev)
{
    /* The first 8 bytes of the device descriptor end with bMaxPacketSize0,
     * which is all endpoint 0 needs to be set up for the rest. */
    int n = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dev->desc, 8);
    if (n < 8) {
        pr_debug("[USB] port %u: device descriptor read failed (%d)\n", dev->port, n);
        return n < 0 ? n : -EIO;
    }
    u16 mps0 = dev->desc.bMaxPacketSize0;
    if (dev->speed == USB_SPEED_SUPER) mps0 = (u16)(1U << (mps0 & 0xF));   /* exponent */
    if (dev->bus->ops->set_ep0_mps && mps0) {
        int rc = dev->bus->ops->set_ep0_mps(dev, mps0);
        if (rc < 0) return rc;
    }

    n = usb_get_descriptor(dev, USB_DESC_DEVICE, 0, &dev->desc, sizeof(dev->desc));
    if (n < (int)sizeof(dev->desc)) return n < 0 ? n : -EIO;
    dev->vendor_id  = dev->desc.idVendor;
    dev->product_id = dev->desc.idProduct;
    usb_read_string(dev, dev->desc.iProduct, dev->product, sizeof(dev->product));

    usb_config_descriptor_t cfg;
    n = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, &cfg, sizeof(cfg));
    if (n < (int)sizeof(cfg) || cfg.wTotalLength < sizeof(cfg)) return n < 0 ? n : -EIO;
    u16 total = cfg.wTotalLength > 4096 ? 4096 : cfg.wTotalLength;
    dev->config = (u8 *)kmalloc(total);
    if (!dev->config) return -ENOMEM;
    n = usb_get_descriptor(dev, USB_DESC_CONFIG, 0, dev->config, total);
    if (n < (int)sizeof(cfg)) return n < 0 ? n : -EIO;
    dev->config_len = (u16)n;

    pr_debug("[USB] %s-speed device %04x:%04x '%s' on %s port %u (%u interface%s)\n",
             usb_speed_name(dev->speed), dev->vendor_id, dev->product_id,
             dev->product[0] ? dev->product : "?", dev->bus->name, dev->port,
             cfg.bNumInterfaces, cfg.bNumInterfaces == 1 ? "" : "s");

    int rc = usb_control_msg(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, cfg.bConfigurationValue, 0, NULL, 0);
    if (rc < 0) {
        pr_debug("[USB] %04x:%04x: SET_CONFIGURATION failed (%d)\n",
                 dev->vendor_id, dev->product_id, rc);
        return rc;
    }

    dev->next = dev->bus->devices;
    dev->bus->devices = dev;

    usb_bind_interfaces(dev);
    if (dev->num_bound == 0)
        pr_debug("[USB] %04x:%04x: no driver for this device\n", dev->vendor_id, dev->product_id);
    return 0;
}

void usb_disconnect(usb_device_t *dev)
{
    if (!dev) return;
    for (u32 i = 0; i < dev->num_bound; i++)
        if (dev->drivers[i]->disconnect) dev->drivers[i]->disconnect(dev, dev->driver_priv[i]);

    for (usb_device_t **pp = &dev->bus->devices; *pp; pp = &(*pp)->next) {
        if (*pp == dev) { *pp = dev->next; break; }
    }
    pr_debug("[USB] %04x:%04x '%s' disconnected from %s port %u\n",
             dev->vendor_id, dev->product_id, dev->product[0] ? dev->product : "?",
             dev->bus->name, dev->port);
    if (dev->config) kfree(dev->config);
    kfree(dev);
}

void usb_bus_poll(usb_bus_t *bus, u64 now_ns)
{
    for (usb_device_t *dev = bus->devices; dev; dev = dev->next)
        for (u32 i = 0; i < dev->num_bound; i++)
            if (dev->drivers[i]->poll) dev->drivers[i]->poll(dev, dev->driver_priv[i], now_ns);
}
