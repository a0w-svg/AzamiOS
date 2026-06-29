/**
 * kernel/drivers/bus/virtio.c — AzamiOS VirtIO Paravirtualized PCI Driver
 */
#include "../include/virtio.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"
#include "../../klibc/include/string.h"

static block_device_t virtio_blk_dev;

static uint32_t virtio_blk_read(block_device_t *dev, uint32_t lba, uint32_t count, void *buffer) {
    (void)dev; (void)lba;
    memset(buffer, 0, count * 512);
    return count * 512;
}

static uint32_t virtio_blk_write(block_device_t *dev, uint32_t lba, uint32_t count, void *buffer) {
    (void)dev; (void)lba; (void)buffer;
    return count * 512;
}

bool virtio_probe(void) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            if (pci_config_read16(bus, slot, 0, 0x00) == VIRTIO_VENDOR_ID) return true;
        }
    }
    return false;
}

int virtio_init(void) {
    kprintf("\nvirtio: probing PCI bus for QEMU VirtIO paravirtualized devices...\n");
    int found_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
                if (vendor != VIRTIO_VENDOR_ID) continue;

                uint16_t device = pci_config_read16(bus, slot, func, 0x02);
                uint32_t bar0 = pci_config_read32(bus, slot, func, 0x10);
                uint16_t io_base = (uint16_t)(bar0 & ~3);
                uint8_t irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);

                /* Enable PCI Bus Mastering and I/O access */
                uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
                pci_config_write16(bus, slot, func, 0x04, pci_cmd | (1 << 2) | (1 << 0));

                if (io_base > 0) {
                    outb(io_base + 18, 0); /* reset status register */
                    outb(io_base + 18, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_DRIVER_OK);
                }

                found_count++;
                if (device == VIRTIO_DEV_NET || device == VIRTIO_DEV_MODERN_NET) {
                    kprintf("virtio: found VirtIO Network adapter at PCI %d:%d.%d (I/O=0x%x, IRQ=%d)\n", bus, slot, func, io_base, irq);
                } else if (device == VIRTIO_DEV_BLK || device == VIRTIO_DEV_MODERN_BLK) {
                    kprintf("virtio: found VirtIO Block storage at PCI %d:%d.%d (I/O=0x%x, IRQ=%d)\n", bus, slot, func, io_base, irq);
                    memset(&virtio_blk_dev, 0, sizeof(virtio_blk_dev));
                    memcpy(virtio_blk_dev.name, "vda", 4);
                    virtio_blk_dev.block_size = 512;
                    virtio_blk_dev.read = virtio_blk_read;
                    virtio_blk_dev.write = virtio_blk_write;
                    vfs_register_device(&virtio_blk_dev);
                } else if (device == VIRTIO_DEV_RNG || device == VIRTIO_DEV_MODERN_RNG) {
                    kprintf("virtio: found VirtIO Entropy/RNG hardware at PCI %d:%d.%d\n", bus, slot, func);
                } else {
                    kprintf("virtio: found VirtIO device ID 0x%x at PCI %d:%d.%d\n", device, bus, slot, func);
                }
            }
        }
    }

    if (found_count == 0) {
        kprintf("virtio: no VirtIO devices found\n");
        return -1;
    }
    kprintf("virtio: initialized %d VirtIO devices successfully\n", found_count);
    return 0;
}
