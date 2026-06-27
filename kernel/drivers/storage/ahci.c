/**
 * ahci.c  –  Advanced Host Controller Interface (SATA AHCI) Driver
 */
#include "../include/ahci.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

static bool g_ahci_present = false;
static uint32_t g_ahci_abar = 0;

void ahci_init(void) {
    kprintf("\nahci: probing PCI bus for SATA AHCI Mass Storage Controllers...\n");

    /* Scan PCI bus for Class 0x01 (Mass Storage), Subclass 0x06 (SATA) */
    uint8_t found_bus = 0, found_slot = 0, found_func = 0;
    bool found = false;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint16_t vendor = pci_config_read16(bus, slot, 0, 0x00);
            if (vendor == 0xFFFF) continue;
            uint32_t class_rev = pci_config_read32(bus, slot, 0, 0x08);
            uint8_t base_class = (uint8_t)(class_rev >> 24);
            uint8_t sub_class  = (uint8_t)(class_rev >> 16);

            if (base_class == 0x01 && sub_class == 0x06) {
                found_bus = bus; found_slot = slot; found_func = 0;
                found = true;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        kprintf("ahci: no AHCI SATA controllers found on PCI bus\n");
        return;
    }

    g_ahci_abar = pci_config_read32(found_bus, found_slot, found_func, 0x24) & ~0xF;
    uint8_t irq = (uint8_t)(pci_config_read32(found_bus, found_slot, found_func, 0x3C) & 0xFF);
    kprintf("ahci: found controller at PCI %d:%d.%d (ABAR MMIO=0x%x, IRQ=%d)\n",
            found_bus, found_slot, found_func, g_ahci_abar, irq);

    uint16_t pci_cmd = pci_config_read16(found_bus, found_slot, found_func, 0x04);
    pci_config_write16(found_bus, found_slot, found_func, 0x04, pci_cmd | (1 << 2) | (1 << 1));

    g_ahci_present = true;
    kprintf("ahci: 32-port AHCI Host Bus Adapter interface initialized\n");
}
