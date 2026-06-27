#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include <stdbool.h>

/*
    Read 32-bit value from PCI configuration space;
*/
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/*
    Write 32-bit value to PCI configuration space;
*/
void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/*
    Read 16-bit value from PCI configuration space;
*/
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/*
    Write 16-bit value to PCI configuration space;
*/
void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);

/*
    Scan PCI bus 0 for a device matching vendor_id and device_id;
    Returns true if found, writing bus/slot/func into out parameters;
*/
bool pci_find_device(uint16_t vendor_id, uint16_t device_id, uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func);

#endif
