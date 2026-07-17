#ifndef PCI_H
#define PCI_H
#include <stdint.h>
#include <stdbool.h>

#define PCI_MAX_DEVICES 256

typedef struct pci_device {
    uint8_t  bus, slot, func;
    uint16_t vendor_id, device_id;
    uint8_t  class_code, subclass, prog_if, revision;
    uint8_t  irq, pin;
    uint8_t  header_type;
    uint32_t bar[6];
    bool     valid;
} pci_device_t;

extern pci_device_t g_pci_table[PCI_MAX_DEVICES];
extern uint32_t     g_pci_count;

void         pci_enumerate(void);
pci_device_t *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog_if);
pci_device_t *pci_find_vendor(uint16_t vendor, uint16_t device);

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
bool     pci_find_device(uint16_t vendor_id, uint16_t device_id,
                         uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func);

static inline void pci_enable_busmaster(pci_device_t *dev) {
    if (!dev) return;
    uint16_t cmd = pci_config_read16(dev->bus, dev->slot, dev->func, 0x04);
    pci_config_write16(dev->bus, dev->slot, dev->func, 0x04,
                       (uint16_t)(cmd | (1u << 2) | (1u << 1)));
}

#endif
