/* ============================================================================
 * AzamiOS — VirtIO Block Device Driver Implementation
 * File: drivers/block/virtio_blk.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "virtio_blk.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

static virtio_blk_dev_t g_vblk_dev;
static bool g_vblk_active = false;
static spinlock_t g_vblk_lock = SPINLOCK_INIT;

static s64 virtio_blk_read_sectors(struct block_dev *dev, u64 lba, u32 count, void *buf)
{
    virtio_blk_dev_t *vdev = (virtio_blk_dev_t *)dev->driver_data;
    if (!vdev || !vdev->vq || !buf || count == 0) return -EINVAL;

    u32 bytes_to_read = count * dev->sector_size;
    virtio_blk_req_hdr_t hdr = {
        .type = VIRTIO_BLK_T_IN,
        .reserved = 0,
        .sector = lba
    };
    volatile u8 status = 0xFF;

    phys_addr_t hdr_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&hdr);
    phys_addr_t buf_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)buf);
    phys_addr_t stat_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&status);

    if (!hdr_phys || !buf_phys || !stat_phys) return -EFAULT;

    phys_addr_t addrs[3] = { hdr_phys, buf_phys, stat_phys };
    u32 lens[3] = { sizeof(hdr), bytes_to_read, 1 };
    bool is_write[3] = { false, true, true }; /* Device reads hdr, writes data & status */

    irqflags_t flags = spinlock_lock_irqsave(&g_vblk_lock);

    if (virtqueue_add_chain(vdev->vq, addrs, lens, is_write, 3, (void *)1) < 0) {
        spinlock_unlock_irqrestore(&g_vblk_lock, flags);
        return -EIO;
    }

    virtqueue_kick(vdev->vq);
    virtio_pci_notify(&vdev->vpci, 0, vdev->vq);

    void *cookie = NULL;
    while (!cookie) {
        cookie = virtqueue_get_used(vdev->vq, NULL);
        __asm__ volatile("pause");
    }

    spinlock_unlock_irqrestore(&g_vblk_lock, flags);

    if (status != VIRTIO_BLK_S_OK) return -EIO;
    return (s64)count;
}

static s64 virtio_blk_write_sectors(struct block_dev *dev, u64 lba, u32 count, const void *buf)
{
    virtio_blk_dev_t *vdev = (virtio_blk_dev_t *)dev->driver_data;
    if (!vdev || !vdev->vq || !buf || count == 0) return -EINVAL;

    u32 bytes_to_write = count * dev->sector_size;
    virtio_blk_req_hdr_t hdr = {
        .type = VIRTIO_BLK_T_OUT,
        .reserved = 0,
        .sector = lba
    };
    volatile u8 status = 0xFF;

    phys_addr_t hdr_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&hdr);
    phys_addr_t buf_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)buf);
    phys_addr_t stat_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&status);

    if (!hdr_phys || !buf_phys || !stat_phys) return -EFAULT;

    phys_addr_t addrs[3] = { hdr_phys, buf_phys, stat_phys };
    u32 lens[3] = { sizeof(hdr), bytes_to_write, 1 };
    bool is_write[3] = { false, false, true }; /* Device reads hdr & data, writes status */

    irqflags_t flags = spinlock_lock_irqsave(&g_vblk_lock);

    if (virtqueue_add_chain(vdev->vq, addrs, lens, is_write, 3, (void *)1) < 0) {
        spinlock_unlock_irqrestore(&g_vblk_lock, flags);
        return -EIO;
    }

    virtqueue_kick(vdev->vq);
    virtio_pci_notify(&vdev->vpci, 0, vdev->vq);

    void *cookie = NULL;
    while (!cookie) {
        cookie = virtqueue_get_used(vdev->vq, NULL);
        __asm__ volatile("pause");
    }

    spinlock_unlock_irqrestore(&g_vblk_lock, flags);

    if (status != VIRTIO_BLK_S_OK) return -EIO;
    return (s64)count;
}

static block_ops_t g_virtio_blk_ops = {
    .read_sectors = virtio_blk_read_sectors,
    .write_sectors = virtio_blk_write_sectors,
};

int virtio_blk_init(device_t *pci_dev)
{
    pci_device_info_t *info = pci_get_device_info(pci_dev);
    if (!info) return -1;

    if (info->vendor_id != 0x1AF4 || (info->device_id != 0x1001 && info->device_id != 0x1042)) {
        return -1;
    }

    pr_debug("[VIRTIO-BLK] Found VirtIO Block Device at PCI %02x:%02x.%x\n",
             info->bus, info->slot, info->func);

    if (virtio_pci_init_device(pci_dev, &g_vblk_dev.vpci) < 0) {
        pr_debug("[VIRTIO-BLK] Failed to initialize VirtIO PCI transport\n");
        return -1;
    }

    virtio_pci_set_status(&g_vblk_dev.vpci, 0); /* Reset */
    virtio_pci_set_status(&g_vblk_dev.vpci,
                          virtio_pci_get_status(&g_vblk_dev.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&g_vblk_dev.vpci, 0)) {
        pr_debug("[VIRTIO-BLK] Failed to negotiate features\n");
        virtio_pci_set_status(&g_vblk_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    g_vblk_dev.vq = virtio_pci_setup_queue(&g_vblk_dev.vpci, 0);
    if (!g_vblk_dev.vq) {
        pr_debug("[VIRTIO-BLK] Failed to setup request queue\n");
        virtio_pci_set_status(&g_vblk_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    virtio_pci_set_status(&g_vblk_dev.vpci,
                          virtio_pci_get_status(&g_vblk_dev.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    /* Read capacity from device config */
    u64 capacity = 0;
    if (g_vblk_dev.vpci.device_cfg) {
        capacity = *(volatile u64 *)(g_vblk_dev.vpci.device_cfg);
    }
    if (capacity == 0) capacity = 2097152; /* Default 1GB */

    g_vblk_dev.capacity_sectors = capacity;
    strncpy(g_vblk_dev.bdev.name, "vda", sizeof(g_vblk_dev.bdev.name) - 1);
    g_vblk_dev.bdev.sector_size = 512;
    g_vblk_dev.bdev.sector_count = capacity;
    g_vblk_dev.bdev.ops = &g_virtio_blk_ops;
    g_vblk_dev.bdev.driver_data = &g_vblk_dev;

    block_dev_register(&g_vblk_dev.bdev);
    g_vblk_active = true;

    pr_debug("[VIRTIO-BLK] Registered block device 'vda' (%llu sectors, %llu MB)\n",
             (unsigned long long)capacity,
             (unsigned long long)((capacity * 512) / (1024 * 1024)));

    return 0;
}

void virtio_blk_probe_all(void)
{
    /* Handled through PCI enumeration callback in HAL */
}
