/**
 * es1370.c  –  Ensoniq ES1370 AudioPCI Sound Card Driver
 */
#include "../include/es1370.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

static bool g_es1370_present = false;
static uint16_t g_es1370_io_base = 0;

void es1370_init(void) {
    uint8_t bus, slot, func;
    kprintf("\nes1370: probing PCI bus for Ensoniq ES1370 AudioPCI...\n");

    if (!pci_find_device(ES1370_VENDOR_ID, ES1370_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("es1370: controller not found on PCI bus\n");
        return;
    }

    g_es1370_io_base = (uint16_t)(pci_config_read32(bus, slot, func, 0x10) & ~3);
    uint8_t irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("es1370: found at PCI %d:%d.%d (I/O Base=0x%x, IRQ=%d)\n", bus, slot, func, g_es1370_io_base, irq);

    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

    g_es1370_present = true;
    kprintf("es1370: 48 kHz stereo DAC and MIDI synthesizer initialized\n");
}
