/* ============================================================================
 * AzamiOS — USB Core Subsystem Header
 * File: drivers/usb/core/usb.h
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
};

/* ── USB Bus / Host Controller Abstraction ───────────────────────────────── */
struct usb_bus {
    const char   *name;
    int           bus_num;
    void         *hcd;
    dm_device_t  *dev;
};

/** usb_core_init() — Initialize the USB core subsystem and sysfs class. */
int usb_core_init(void);

/** usb_device_create() — Allocate and initialize a logical USB device. */
usb_device_t *usb_device_create(usb_bus_t *bus, u8 port, u8 speed);
