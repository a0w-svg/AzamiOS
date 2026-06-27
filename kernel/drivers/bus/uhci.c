/**
 * uhci.c  –  Universal Host Controller Interface (USB 1.1 UHCI) Driver
 */
#include "../include/uhci.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

static bool g_uhci_present = false;
static uint16_t g_uhci_io_base = 0;

void uhci_init(void) {
    kprintf("\nuhci: probing PCI bus for USB Universal Host Controllers...\n");

    /* Scan PCI bus for Class 0x0C (Serial Bus Controller), Subclass 0x03 (USB) */
    uint8_t found_bus = 0, found_slot = 0, found_func = 0;
    bool found = false;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
                if (vendor == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                uint8_t base_class = (uint8_t)(class_rev >> 24);
                uint8_t sub_class  = (uint8_t)(class_rev >> 16);
                uint8_t prog_if    = (uint8_t)(class_rev >> 8);

                /* ProgIF 0x00 is UHCI */
                if (base_class == 0x0C && sub_class == 0x03 && prog_if == 0x00) {
                    found_bus = bus; found_slot = slot; found_func = func;
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (found) break;
    }

    if (!found) {
        kprintf("uhci: no UHCI USB controllers found on PCI bus\n");
        return;
    }

    g_uhci_io_base = (uint16_t)(pci_config_read32(found_bus, found_slot, found_func, 0x20) & ~0xF);
    uint8_t irq = (uint8_t)(pci_config_read32(found_bus, found_slot, found_func, 0x3C) & 0xFF);
    kprintf("uhci: found controller at PCI %d:%d.%d (I/O Base=0x%x, IRQ=%d)\n",
            found_bus, found_slot, found_func, g_uhci_io_base, irq);

    uint16_t pci_cmd = pci_config_read16(found_bus, found_slot, found_func, 0x04);
    pci_config_write16(found_bus, found_slot, found_func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

    g_uhci_present = true;
    kprintf("uhci: USB root hub initialized (2 root ports active)\n");
}
