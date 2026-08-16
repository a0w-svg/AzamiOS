/* ============================================================================
 * AzamiOS — PCI Bus Enumeration Header
 * File: hal/pci.h
 *
 * Implements PCI Configuration Mechanism #1 (I/O ports 0xCF8/0xCFC) for
 * enumerating devices on the PCI bus.  Each discovered PCI function is
 * represented as a device_t in the device tree, parented under the bus
 * device "PCI0".
 *
 * API summary:
 *   pci_init()                          — scan all buses, populate device tree
 *   pci_config_read{8,16,32}(b,s,f,o)  → config space value
 *   pci_config_write{8,16,32}(b,s,f,o,v)
 *   pci_enable_bus_mastering(dev)       — set bus master enable bit
 *   pci_get_bar(dev, index)             → physical base address
 *   pci_get_device_info(dev)            → pci_device_info_t *
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "device.h"

/* ── PCI Configuration Space I/O ports ───────────────────────────────────── */
#define PCI_CONFIG_ADDR   0x0CF8
#define PCI_CONFIG_DATA   0x0CFC

/* ── PCI Configuration Space Offsets ─────────────────────────────────────── */
#define PCI_VENDOR_ID        0x00   /* u16 */
#define PCI_DEVICE_ID        0x02   /* u16 */
#define PCI_COMMAND          0x04   /* u16 */
#define PCI_STATUS           0x06   /* u16 */
#define PCI_REVISION_ID      0x08   /* u8  */
#define PCI_PROG_IF          0x09   /* u8  */
#define PCI_SUBCLASS         0x0A   /* u8  */
#define PCI_CLASS_CODE       0x0B   /* u8  */
#define PCI_CACHE_LINE       0x0C   /* u8  */
#define PCI_LATENCY_TIMER    0x0D   /* u8  */
#define PCI_HEADER_TYPE      0x0E   /* u8  */
#define PCI_BIST             0x0F   /* u8  */
#define PCI_BAR0             0x10   /* u32 */
#define PCI_BAR1             0x14   /* u32 */
#define PCI_BAR2             0x18   /* u32 */
#define PCI_BAR3             0x1C   /* u32 */
#define PCI_BAR4             0x20   /* u32 */
#define PCI_BAR5             0x24   /* u32 */
#define PCI_SUBSYS_VENDOR    0x2C   /* u16 */
#define PCI_SUBSYS_ID        0x2E   /* u16 */
#define PCI_INTERRUPT_LINE   0x3C   /* u8  */
#define PCI_INTERRUPT_PIN    0x3D   /* u8  */
#define PCI_SECONDARY_BUS    0x19   /* u8  (PCI-to-PCI bridge) */

/* ── PCI Command Register bits ───────────────────────────────────────────── */
#define PCI_CMD_IO_SPACE        (1U << 0)
#define PCI_CMD_MEMORY_SPACE    (1U << 1)
#define PCI_CMD_BUS_MASTER      (1U << 2)
#define PCI_CMD_INTERRUPT_DIS   (1U << 10)

/* ── PCI Class Codes (major classes only) ────────────────────────────────── */
#define PCI_CLASS_UNCLASSIFIED   0x00
#define PCI_CLASS_MASS_STORAGE   0x01
#define PCI_CLASS_NETWORK        0x02
#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_MULTIMEDIA     0x04
#define PCI_CLASS_MEMORY         0x05
#define PCI_CLASS_BRIDGE         0x06
#define PCI_CLASS_COMMUNICATION  0x07
#define PCI_CLASS_SYSTEM         0x08
#define PCI_CLASS_INPUT          0x09
#define PCI_CLASS_SERIAL_BUS     0x0C

/* ── PCI Device Information (stored in device_t->driver_data) ────────────── */
typedef struct {
    u16  vendor_id;
    u16  device_id;
    u8   class_code;
    u8   subclass;
    u8   prog_if;
    u8   revision;
    u8   bus;
    u8   slot;
    u8   func;
    u8   header_type;
    u32  bar[6];
    u8   interrupt_line;
    u8   interrupt_pin;
    u16  subsys_vendor;
    u16  subsys_id;
} pci_device_info_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/**
 * pci_init() — Enumerate all PCI buses and devices.
 *
 * Creates the "PCI0" bus device under the device tree root, then scans
 * all 256 buses × 32 slots × 8 functions.  Each valid PCI function
 * becomes a device_t child of the PCI bus device.
 */
void pci_init(void);

/* ── Configuration Space Access ──────────────────────────────────────────── */

u32  pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16  pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
u8   pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset);

void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 val);
void pci_config_write16(u8 bus, u8 slot, u8 func, u8 offset, u16 val);
void pci_config_write8(u8 bus, u8 slot, u8 func, u8 offset, u8 val);

/* ── Device Helpers ──────────────────────────────────────────────────────── */

/**
 * pci_enable_bus_mastering(dev) — Set the Bus Master Enable bit
 * in the PCI Command register for the given device.
 */
void pci_enable_bus_mastering(device_t *dev);

/**
 * pci_get_bar(dev, index) → physical base address of the BAR.
 *
 * Reads the BAR from the device's pci_device_info_t.
 * Masks off type/prefetch bits for MMIO BARs, I/O bits for I/O BARs.
 */
phys_addr_t pci_get_bar(device_t *dev, u32 bar_index);

/**
 * pci_get_device_info(dev) → pci_device_info_t *
 *
 * Returns the PCI device info structure stored in driver_data.
 * Returns NULL if the device is not a PCI device.
 */
pci_device_info_t *pci_get_device_info(device_t *dev);

/**
 * pci_get_device_count() → total number of PCI devices found.
 */
u32 pci_get_device_count(void);
