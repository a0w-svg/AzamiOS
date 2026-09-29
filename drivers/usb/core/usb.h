/* ============================================================================
 * AzamiOS — USB Core Subsystem Header
 * File: drivers/usb/core/usb.h
 *
 * The split follows Linux's: a host controller driver (xhci.c, …) owns the
 * hardware and knows how to move bytes to one device endpoint; the core owns
 * everything that is the same on every controller — standard descriptors,
 * SET_CONFIGURATION, and handing each interface to the class driver that
 * claims it (usbhid.c for keyboards, mice and tablets).
 *
 * A controller plugs in by filling a usb_hcd_ops_t, pointing its usb_bus_t at
 * it, and calling usb_new_device() once it has a device addressed and able
 * to answer control transfers on endpoint 0.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/defs.h"
#include "../../base/base.h"

/* ── USB Speeds ──────────────────────────────────────────────────────────── */
#define USB_SPEED_LOW    0   /* 1.5 Mbit/s */
#define USB_SPEED_FULL   1   /* 12 Mbit/s  */
#define USB_SPEED_HIGH   2   /* 480 Mbit/s */
#define USB_SPEED_SUPER  3   /* 5 Gbit/s   */

/* ── USB PID Tokens ──────────────────────────────────────────────────────── */
#define USB_PID_OUT      0xE1
#define USB_PID_IN       0x69
#define USB_PID_SOF      0xA5
#define USB_PID_SETUP    0x2D

/* ── bmRequestType ───────────────────────────────────────────────────────── */
#define USB_DIR_OUT               0x00
#define USB_DIR_IN                0x80
#define USB_TYPE_STANDARD         0x00
#define USB_TYPE_CLASS            0x20
#define USB_TYPE_VENDOR           0x40
#define USB_RECIP_DEVICE          0x00
#define USB_RECIP_INTERFACE       0x01
#define USB_RECIP_ENDPOINT        0x02

/* ── Standard Request Types ──────────────────────────────────────────────── */
#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0A
#define USB_REQ_SET_INTERFACE     0x0B
#define USB_REQ_SYNCH_FRAME       0x0C

/* ── Standard Descriptor Types ───────────────────────────────────────────── */
#define USB_DESC_DEVICE           0x01
#define USB_DESC_CONFIG           0x02
#define USB_DESC_STRING           0x03
#define USB_DESC_INTERFACE        0x04
#define USB_DESC_ENDPOINT         0x05
#define USB_DESC_HID              0x21
#define USB_DESC_REPORT           0x22

/* ── Interface classes ───────────────────────────────────────────────────── */
#define USB_CLASS_PER_INTERFACE   0x00
#define USB_CLASS_HID             0x03
#define USB_CLASS_MASS_STORAGE    0x08
#define USB_CLASS_HUB             0x09

/* ── Endpoint attributes ─────────────────────────────────────────────────── */
#define USB_ENDPOINT_DIR_IN       0x80
#define USB_ENDPOINT_NUM_MASK     0x0F
#define USB_ENDPOINT_XFER_MASK    0x03
#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC    1
#define USB_ENDPOINT_XFER_BULK    2
#define USB_ENDPOINT_XFER_INT     3

/* ── USB Setup Packet (8 bytes) ──────────────────────────────────────────── */
typedef struct __attribute__((packed)) usb_setup_packet {
    u8  bmRequestType;
    u8  bRequest;
    u16 wValue;
    u16 wIndex;
    u16 wLength;
} usb_setup_packet_t;

/* ── Standard Device Descriptor (18 bytes) ───────────────────────────────── */
typedef struct __attribute__((packed)) usb_device_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 bcdUSB;
    u8  bDeviceClass;
    u8  bDeviceSubClass;
    u8  bDeviceProtocol;
    u8  bMaxPacketSize0;
    u16 idVendor;
    u16 idProduct;
    u16 bcdDevice;
    u8  iManufacturer;
    u8  iProduct;
    u8  iSerialNumber;
    u8  bNumConfigurations;
} usb_device_descriptor_t;

/* ── Standard Configuration Descriptor (9 bytes) ─────────────────────────── */
typedef struct __attribute__((packed)) usb_config_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u16 wTotalLength;
    u8  bNumInterfaces;
    u8  bConfigurationValue;
    u8  iConfiguration;
    u8  bmAttributes;
    u8  bMaxPower;
} usb_config_descriptor_t;

/* ── Standard Interface Descriptor (9 bytes) ─────────────────────────────── */
typedef struct __attribute__((packed)) usb_interface_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u8  bInterfaceNumber;
    u8  bAlternateSetting;
    u8  bNumEndpoints;
    u8  bInterfaceClass;
    u8  bInterfaceSubClass;
    u8  bInterfaceProtocol;
    u8  iInterface;
} usb_interface_descriptor_t;

/* ── Standard Endpoint Descriptor (7 bytes) ──────────────────────────────── */
typedef struct __attribute__((packed)) usb_endpoint_descriptor {
    u8  bLength;
    u8  bDescriptorType;
    u8  bEndpointAddress;
    u8  bmAttributes;
    u16 wMaxPacketSize;
    u8  bInterval;
} usb_endpoint_descriptor_t;

/* Forward declarations */
typedef struct usb_bus usb_bus_t;
typedef struct usb_device usb_device_t;

#define USB_MAX_INTERFACES 8

/* Called by the host controller, in its own thread, with each completed
 * interrupt-IN transfer. Must not issue transfers itself (it runs inside the
 * controller's event processing); defer that work to the class driver's
 * poll hook, which runs from the same thread outside event processing. */
typedef void (*usb_intr_cb_t)(usb_device_t *dev, void *ctx, const u8 *data, u32 len);

/* ── Host controller operations ──────────────────────────────────────────── */
typedef struct usb_hcd_ops {
    /* Synchronous control transfer on endpoint 0. Returns the number of data
     * bytes transferred, or a negative errno (-EPIPE for a STALL). */
    int (*control)(usb_device_t *dev, const usb_setup_packet_t *setup, void *data);
    /* Endpoint 0's max packet size turned out different from the default the
     * controller assumed for this speed (full-speed devices: 8/16/32/64). */
    int (*set_ep0_mps)(usb_device_t *dev, u16 mps);
    /* Configure an interrupt-IN endpoint and keep a transfer queued on it,
     * calling @cb with every completed one. */
    /* Synchronous bulk transfer. */
    int (*bulk)(usb_device_t *dev, const usb_endpoint_descriptor_t *ep, void *data, u32 len, u32 stream_id);
    /* Setup an isochronous stream. */
    int (*isoc_start)(usb_device_t *dev, const usb_endpoint_descriptor_t *ep);
    /* Allocate USB 3.0 streams for a bulk endpoint. */
    int (*alloc_streams)(usb_device_t *dev, const usb_endpoint_descriptor_t *ep, u32 num_streams);

    int (*intr_in)(usb_device_t *dev, const usb_endpoint_descriptor_t *ep,
                   usb_intr_cb_t cb, void *ctx);
} usb_hcd_ops_t;

/* ── Class drivers ───────────────────────────────────────────────────────── */
typedef struct usb_class_driver {
    const char *name;
    /* Claim @intf (with its endpoint/class descriptors in @cfg) or return a
     * negative errno. On success *priv is handed back to the other hooks. */
    int  (*probe)(usb_device_t *dev, const usb_interface_descriptor_t *intf,
                  const u8 *cfg, u16 cfg_len, void **priv);
    void (*disconnect)(usb_device_t *dev, void *priv);
    /* Periodic work from the controller thread (key autorepeat, LED sync). */
    void (*poll)(usb_device_t *dev, void *priv, u64 now_ns);
} usb_class_driver_t;

/* ── USB Device Structure ────────────────────────────────────────────────── */
struct usb_device {
    u8                      address;
    u8                      speed;
    u16                     vendor_id;
    u16                     product_id;
    u8                      port;
    usb_device_descriptor_t desc;
    usb_bus_t              *bus;
    void                   *hcd_priv;

    char                    product[64];
    u8                     *config;          /* full configuration descriptor */
    u16                     config_len;
    u8                      num_bound;
    const usb_class_driver_t *drivers[USB_MAX_INTERFACES];
    void                   *driver_priv[USB_MAX_INTERFACES];
    usb_device_t           *next;            /* on bus->devices              */
};

/* ── USB Bus / Host Controller Abstraction ───────────────────────────────── */
struct usb_bus {
    const char   *name;
    int           bus_num;
    void         *hcd;
    dm_device_t  *dev;
    const usb_hcd_ops_t *ops;
    usb_device_t *devices;                   /* enumerated devices           */
};

/** usb_core_init() — Initialize the USB core subsystem and sysfs class. */
int usb_core_init(void);

/** usb_device_create() — Allocate and initialize a logical USB device. */
usb_device_t *usb_device_create(usb_bus_t *bus, u8 port, u8 speed);

/** usb_control_msg() — one control transfer; see usb_hcd_ops_t.control. */
int usb_control_msg(usb_device_t *dev, u8 request_type, u8 request,
                    u16 value, u16 index, void *data, u16 len);

/** usb_get_descriptor() — standard GET_DESCRIPTOR(@type, @index). */
int usb_get_descriptor(usb_device_t *dev, u8 type, u8 index, void *buf, u16 len);

/** usb_new_device() — read descriptors, select configuration 1 and bind
 * class drivers. Called by the controller once the device is addressed. */
int usb_new_device(usb_device_t *dev);

/** usb_disconnect() — unbind class drivers and free @dev. The controller
 * releases its own per-device state after this returns. */
void usb_disconnect(usb_device_t *dev);

/** usb_bus_poll() — run class drivers' periodic work for @bus's devices.
 * Called by the controller's thread; @now_ns is CLOCK_MONOTONIC. */
void usb_bus_poll(usb_bus_t *bus, u64 now_ns);

/** usb_speed_name() — "low", "full", "high", "super". */
const char *usb_speed_name(u8 speed);

/* ── Class drivers built into the kernel ─────────────────────────────────── */
extern const usb_class_driver_t usbhid_driver;
