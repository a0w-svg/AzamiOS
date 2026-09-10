/* ============================================================================
 * AzamiOS — PCI Bus Type for the Driver Model
 * File: drivers/base/pci_bus.h
 *
 * hal/pci.c walks configuration space and builds the topology.  This layer
 * turns each discovered function into a bindable driver-model device on a
 * "pci" bus, matched against driver ID tables exactly as Linux does:
 *
 *   static const pci_device_id_t e1000_ids[] = {
 *       { PCI_DEVICE(0x8086, 0x100E) },
 *       { 0 }
 *   };
 *
 * A driver registered here binds to every matching function the HAL found,
 * whenever it is registered.
 * ============================================================================ */
#pragma once

#include "base.h"
#include "../../hal/pci.h"

#define PCI_ANY_ID ((u16)0xFFFF)

typedef struct pci_device_id {
    u16 vendor;
    u16 device;
    u16 subvendor;
    u16 subdevice;
    u32 dev_class;        /* (class << 16) | (subclass << 8) | prog_if */
    u32 dev_class_mask;   /* 0 = ignore class entirely                 */
} pci_device_id_t;

/* Match one vendor:device pair, any subsystem, any class. */
#define PCI_DEVICE(vend, dev) \
    .vendor = (vend), .device = (dev), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

/* Match a whole device class, any vendor (e.g. every AHCI controller). */
#define PCI_DEVICE_CLASS(cls, mask) \
    .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
    .dev_class = (cls), .dev_class_mask = (mask)

typedef struct pci_driver {
    dm_driver_t drv;                       /* .name identifies the driver     */
    const pci_device_id_t *id_table;       /* zero-terminated match table     */
    int  (*probe)(dm_device_t *dev, const pci_device_id_t *id);
    void (*remove)(dm_device_t *dev);
} pci_driver_t;

/** pci_driver_register(pdrv) — publish a PCI driver and bind matching devices. */
int pci_driver_register(pci_driver_t *pdrv);

/** to_pci_info(dev) → the HAL's configuration-space snapshot for @dev. */
static inline pci_device_info_t *to_pci_info(dm_device_t *dev)
{
    return (pci_device_info_t *)dev->bus_data;
}

/** pci_dm_device_for(hal_dev) → driver-model device wrapping a HAL PCI node. */
dm_device_t *pci_dm_device_for(device_t *hal_dev);
