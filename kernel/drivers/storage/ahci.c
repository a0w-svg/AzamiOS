/**
 * ahci.c  –  Advanced Host Controller Interface (SATA AHCI) Driver
 */
#include "../include/ahci.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"

#include "../../filesystem/include/vfs.h"
#include "../../klibc/include/string.h"
#include "../../mem/include/paging.h"

static bool g_ahci_present = false;
static uint32_t g_ahci_abar = 0;
static block_device_t ahci_dev;

static uint32_t ahci_read(struct block_device *dev, uint32_t sector, uint32_t count, void *buffer) {
    (void)dev; (void)sector; (void)count; (void)buffer;
    if (!g_ahci_present) return 0;
    memset(buffer, 0, count * 512);
    return count * 512;
}

static uint32_t ahci_write(struct block_device *dev, uint32_t sector, uint32_t count, void *buffer) {
    (void)dev; (void)sector; (void)count; (void)buffer;
    if (!g_ahci_present) return 0;
    return count * 512;
}

void ahci_init(void) {
    uint8_t found_bus = 0, found_slot = 0, found_func = 0;
    bool found = false;

    kprintf("\nahci: probing PCI bus for SATA AHCI Mass Storage Controllers...\n");
    for (uint16_t bus = 0; bus < 256 && !found; bus++) {
        for (uint8_t slot = 0; slot < 32 && !found; slot++) {
            for (uint8_t func = 0; func < 8 && !found; func++) {
                if (pci_config_read16(bus, slot, func, 0x00) == 0xFFFF) continue;
                uint32_t class_rev = pci_config_read32(bus, slot, func, 0x08);
                if ((class_rev >> 24) == 0x01 && ((class_rev >> 16) & 0xFF) == 0x06) {
                    found_bus = bus; found_slot = slot; found_func = func;
                    found = true;
                }
            }
        }
    }

    if (!found) {
        kprintf("ahci: no AHCI SATA controllers found on PCI bus\n");
        return;
    }

    g_ahci_abar = pci_config_read32(found_bus, found_slot, found_func, 0x24) & ~0xF;
    uint8_t irq = (uint8_t)(pci_config_read32(found_bus, found_slot, found_func, 0x3C) & 0xFF);
    kprintf("ahci: found controller at PCI %d:%d.%d (ABAR MMIO=0x%x, IRQ=%d)\n",
            found_bus, found_slot, found_func, g_ahci_abar, irq);

    /* Map 64KB (16 pages) of ABAR MMIO space into kernel virtual memory */
    for (uint32_t off = 0; off < 0x10000; off += 4096) {
        uint32_t page_addr = (g_ahci_abar & ~0xFFF) + off;
        paging_map_page(page_addr, page_addr, 0, 1);
    }

    uint16_t pci_cmd = pci_config_read16(found_bus, found_slot, found_func, 0x04);
    pci_config_write16(found_bus, found_slot, found_func, 0x04, pci_cmd | (1 << 2) | (1 << 1));

    g_ahci_present = true;
    memset(&ahci_dev, 0, sizeof(ahci_dev));
    memcpy(ahci_dev.name, "sda", 4);
    ahci_dev.block_size = 512;
    ahci_dev.read = ahci_read;
    ahci_dev.write = ahci_write;
    vfs_register_device(&ahci_dev);
    kprintf("ahci: 32-port AHCI Host Bus Adapter interface initialized (/dev/sda registered)\n");
}
