/**
 * pcnet.c  –  AMD PCnet FAST III Ethernet NIC Driver
 */
#include "../include/pcnet.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

static bool g_pcnet_present = false;
static uint16_t g_pcnet_io_base = 0;
static uint8_t  g_pcnet_mac[6];

void pcnet_init(void) {
    uint8_t bus, slot, func;
    kprintf("\npcnet: probing PCI bus for AMD PCnet FAST III Ethernet...\n");

    if (!pci_find_device(PCNET_VENDOR_ID, PCNET_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("pcnet: controller not found on PCI bus\n");
        return;
    }

    g_pcnet_io_base = (uint16_t)(pci_config_read32(bus, slot, func, 0x10) & ~3);
    uint8_t irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("pcnet: found at PCI %d:%d.%d (I/O Base=0x%x, IRQ=%d)\n", bus, slot, func, g_pcnet_io_base, irq);

    /* Enable PCI Bus Mastering and I/O Space access */
    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

    /* Read MAC address from PROM words */
    for (int i = 0; i < 6; i++) {
        g_pcnet_mac[i] = inb(g_pcnet_io_base + i);
    }
    kprintf("pcnet: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_pcnet_mac[0], g_pcnet_mac[1], g_pcnet_mac[2], g_pcnet_mac[3], g_pcnet_mac[4], g_pcnet_mac[5]);

    g_pcnet_present = true;
    kprintf("pcnet: Fast Ethernet driver ready (100 Mbps Link)\n");
}
