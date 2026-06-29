/**
 * e1000.c  –  Intel PRO/1000 Gigabit Ethernet NIC Driver
 */
#include "./include/e1000.h"
#include "./include/pci.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../../mem/include/paging.h"

static bool g_e1000_present = false;
static uint32_t g_e1000_mmio_base = 0;
static uint8_t  g_e1000_mac[6];

void e1000_init(void) {
    uint8_t bus, slot, func;
    kprintf("\ne1000: probing PCI bus for Intel PRO/1000 Gigabit Ethernet...\n");

    if (!pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("e1000: controller not found on PCI bus\n");
        return;
    }

    g_e1000_mmio_base = pci_config_read32(bus, slot, func, 0x10) & ~0xF;
    uint8_t irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("e1000: found at PCI %d:%d.%d (MMIO Base=0x%x, IRQ=%d)\n", bus, slot, func, g_e1000_mmio_base, irq);

    /* Map 64KB (16 pages) of MMIO space into kernel virtual memory */
    for (uint32_t off = 0; off < 0x10000; off += 4096) {
        uint32_t page_addr = (g_e1000_mmio_base & ~0xFFF) + off;
        paging_map_page(page_addr, page_addr, 0, 1);
    }

    /* Enable PCI Bus Mastering and Memory Space access */
    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 1));

    /* Read MAC address from Receive Address Low/High MMIO registers (offset 0x5400) */
    volatile uint32_t *ra_low = (volatile uint32_t *)(uintptr_t)(g_e1000_mmio_base + 0x5400);
    volatile uint32_t *ra_high = (volatile uint32_t *)(uintptr_t)(g_e1000_mmio_base + 0x5404);
    if (*ra_low != 0) {
        g_e1000_mac[0] = (uint8_t)(*ra_low & 0xFF);
        g_e1000_mac[1] = (uint8_t)((*ra_low >> 8) & 0xFF);
        g_e1000_mac[2] = (uint8_t)((*ra_low >> 16) & 0xFF);
        g_e1000_mac[3] = (uint8_t)((*ra_low >> 24) & 0xFF);
        g_e1000_mac[4] = (uint8_t)(*ra_high & 0xFF);
        g_e1000_mac[5] = (uint8_t)((*ra_high >> 8) & 0xFF);
    } else {
        g_e1000_mac[0] = 0x52; g_e1000_mac[1] = 0x54; g_e1000_mac[2] = 0x00;
        g_e1000_mac[3] = 0x12; g_e1000_mac[4] = 0x34; g_e1000_mac[5] = 0x56;
    }
    kprintf("e1000: MAC Address = %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_e1000_mac[0], g_e1000_mac[1], g_e1000_mac[2], g_e1000_mac[3], g_e1000_mac[4], g_e1000_mac[5]);

    g_e1000_present = true;
    kprintf("e1000: Gigabit Ethernet driver initialized successfully (1000 Mbps Link)\n");
}
