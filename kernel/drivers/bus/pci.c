#include "./include/pci.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

pci_device_t g_pci_table[PCI_MAX_DEVICES];
uint32_t     g_pci_count = 0;

static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (uint32_t)((1U << 31)
        | ((uint32_t)bus  << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func <<  8)
        | (offset & 0xFC));
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    uint32_t dword = inl(PCI_CONFIG_DATA);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    uint32_t dword = inl(PCI_CONFIG_DATA);
    int shift = (offset & 2) * 8;
    dword &= ~(0xFFFFU << shift);
    dword |= ((uint32_t)value << shift);
    outl(PCI_CONFIG_ADDRESS, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, dword);
}

bool pci_find_device(uint16_t vendor_id, uint16_t device_id,
                     uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    for (uint32_t i = 0; i < g_pci_count; i++) {
        if (g_pci_table[i].vendor_id == vendor_id &&
            g_pci_table[i].device_id == device_id) {
            if (out_bus)  *out_bus  = g_pci_table[i].bus;
            if (out_slot) *out_slot = g_pci_table[i].slot;
            if (out_func) *out_func = g_pci_table[i].func;
            return true;
        }
    }
    return false;
}

pci_device_t *pci_find_class(uint8_t cls, uint8_t sub, uint8_t prog_if) {
    for (uint32_t i = 0; i < g_pci_count; i++) {
        if (g_pci_table[i].class_code == cls &&
            g_pci_table[i].subclass   == sub &&
            (prog_if == 0xFF || g_pci_table[i].prog_if == prog_if)) {
            return &g_pci_table[i];
        }
    }
    return (void*)0;
}

pci_device_t *pci_find_vendor(uint16_t vendor, uint16_t device) {
    for (uint32_t i = 0; i < g_pci_count; i++) {
        if (g_pci_table[i].vendor_id == vendor &&
            g_pci_table[i].device_id == device) {
            return &g_pci_table[i];
        }
    }
    return (void*)0;
}

void pci_enumerate(void) {
    g_pci_count = 0;
    kprintf("pci: enumerating PCI bus...\n");

    for (uint16_t bus = 0; bus < 256 && g_pci_count < PCI_MAX_DEVICES; bus++) {
        for (uint8_t slot = 0; slot < 32 && g_pci_count < PCI_MAX_DEVICES; slot++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            uint8_t header = (uint8_t)(pci_config_read32((uint8_t)bus, slot, 0, 0x0C) >> 16);
            uint8_t max_func = (header & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < max_func && g_pci_count < PCI_MAX_DEVICES; func++) {
                uint16_t v = pci_config_read16((uint8_t)bus, slot, func, 0x00);
                if (v == 0xFFFF) continue;

                pci_device_t *d = &g_pci_table[g_pci_count];
                memset(d, 0, sizeof(*d));

                d->bus       = (uint8_t)bus;
                d->slot      = slot;
                d->func      = func;
                d->vendor_id = v;
                d->device_id = pci_config_read16((uint8_t)bus, slot, func, 0x02);

                uint32_t class_rev = pci_config_read32((uint8_t)bus, slot, func, 0x08);
                d->class_code = (uint8_t)(class_rev >> 24);
                d->subclass   = (uint8_t)(class_rev >> 16);
                d->prog_if    = (uint8_t)(class_rev >>  8);
                d->revision   = (uint8_t)(class_rev);

                uint32_t bist_hdr = pci_config_read32((uint8_t)bus, slot, func, 0x0C);
                d->header_type = (uint8_t)(bist_hdr >> 16);

                uint32_t irq_info = pci_config_read32((uint8_t)bus, slot, func, 0x3C);
                d->irq = (uint8_t)(irq_info & 0xFF);
                d->pin = (uint8_t)((irq_info >> 8) & 0xFF);

                /* Read BARs (only for header type 0) */
                if ((d->header_type & 0x7F) == 0) {
                    for (int b = 0; b < 6; b++) {
                        d->bar[b] = pci_config_read32((uint8_t)bus, slot, func,
                                                       (uint8_t)(0x10 + b * 4));
                    }
                }
                d->valid = true;
                g_pci_count++;
            }
        }
    }
    kprintf("pci: enumerated %u device(s)\n", (unsigned)g_pci_count);
}
