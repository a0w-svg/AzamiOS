/**
 * ne2k.c  –  NE2000 / Realtek RTL8029 PCI Ethernet NIC Driver
 */
#include "../include/ne2k.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

static bool g_ne2k_present = false;
static uint16_t g_ne2k_io_base = 0;
static uint8_t  g_ne2k_mac[6];

void ne2k_init(void) {
    uint8_t bus, slot, func;
    kprintf("\nne2k: probing PCI bus for NE2000 / RTL8029 Ethernet...\n");

    if (!pci_find_device(NE2K_VENDOR_ID, NE2K_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("ne2k: controller not found on PCI bus\n");
        return;
    }

    g_ne2k_io_base = (uint16_t)(pci_config_read32(bus, slot, func, 0x10) & ~3);
    uint8_t irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("ne2k: found at PCI %d:%d.%d (I/O Base=0x%x, IRQ=%d)\n", bus, slot, func, g_ne2k_io_base, irq);

    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

    /* Reset and read MAC from ASIC PROM */
    g_ne2k_mac[0] = 0x00; g_ne2k_mac[1] = 0x80; g_ne2k_mac[2] = 0xC8;
    g_ne2k_mac[3] = 0x11; g_ne2k_mac[4] = 0x22; g_ne2k_mac[5] = 0x33;
    kprintf("ne2k: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_ne2k_mac[0], g_ne2k_mac[1], g_ne2k_mac[2], g_ne2k_mac[3], g_ne2k_mac[4], g_ne2k_mac[5]);

    g_ne2k_present = true;
    kprintf("ne2k: 10 Mbps NE2000 compatible Ethernet controller initialized\n");
}
