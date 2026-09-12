/* ============================================================================
 * AzamiOS — VirtIO Block Device Driver Implementation
 * File: drivers/block/virtio_blk.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "virtio_blk.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../kernel/lib/string.h"

static virtio_blk_dev_t g_vblk_devs[VIRTIO_BLK_MAX_DEVICES];
static u32 g_vblk_count = 0;

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

    /* Plain lock, not _irqsave: this path only ever runs in thread context
     * (no ISR touches this device's lock), so there is no reentrancy hazard
     * to guard against by disabling interrupts. Masking them for the whole round trip
     * used to stall this core's timer tick and IPIs for as long as the host
     * took to service the request — under TCG that is not free. See the same
     * fix in virtio_gpu.c's virtio_gpu_submit(). */
    spinlock_lock(&vdev->lock);

    if (virtqueue_add_chain(vdev->vq, addrs, lens, is_write, 3, (void *)1) < 0) {
        spinlock_unlock(&vdev->lock);
        return -EIO;
    }

    virtqueue_kick(vdev->vq);
    virtio_pci_notify(&vdev->vpci, 0, vdev->vq);

    /* Bounded: a wedged host must fail the request, not hang the caller (and
     * every other thread queued behind this lock) forever. */
    void *cookie = NULL;
    u64 spins = 0;
    const u64 SPIN_LIMIT = 200000000ULL;
    while (!cookie) {
        cookie = virtqueue_get_used(vdev->vq, NULL);
        if (cookie) break;
        if (++spins >= SPIN_LIMIT) {
            spinlock_unlock(&vdev->lock);
            pr_debug("[VIRTIO-BLK] read request timed out\n");
            return -EIO;
        }
        hw_spin_wait((u32)spins);
    }

    spinlock_unlock(&vdev->lock);

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

    spinlock_lock(&vdev->lock);

    if (virtqueue_add_chain(vdev->vq, addrs, lens, is_write, 3, (void *)1) < 0) {
        spinlock_unlock(&vdev->lock);
        return -EIO;
    }

    virtqueue_kick(vdev->vq);
    virtio_pci_notify(&vdev->vpci, 0, vdev->vq);

    void *cookie = NULL;
    u64 spins = 0;
    const u64 SPIN_LIMIT = 200000000ULL;
    while (!cookie) {
        cookie = virtqueue_get_used(vdev->vq, NULL);
        if (cookie) break;
        if (++spins >= SPIN_LIMIT) {
            spinlock_unlock(&vdev->lock);
            pr_debug("[VIRTIO-BLK] write request timed out\n");
            return -EIO;
        }
        hw_spin_wait((u32)spins);
    }

    spinlock_unlock(&vdev->lock);

    if (status != VIRTIO_BLK_S_OK) return -EIO;
    return (s64)count;
}

static block_ops_t g_virtio_blk_ops = {
    .read_sectors = virtio_blk_read_sectors,
    .write_sectors = virtio_blk_write_sectors,
};

static int virtio_blk_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_vblk_count >= VIRTIO_BLK_MAX_DEVICES) {
        pr_debug("[VIRTIO-BLK] %u devices already bound, ignoring another\n",
                 g_vblk_count);
        return -ENOSPC;
    }

    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    virtio_blk_dev_t *vdev = &g_vblk_devs[g_vblk_count];
    memset(vdev, 0, sizeof(*vdev));
    spinlock_init(&vdev->lock);

    pr_debug("[VIRTIO-BLK] Found VirtIO Block Device at PCI %02x:%02x.%x\n",
             info->bus, info->slot, info->func);

    if (virtio_pci_init_device(dm->hal, &vdev->vpci) < 0) {
        pr_debug("[VIRTIO-BLK] Failed to initialize VirtIO PCI transport\n");
        return -1;
    }

    virtio_pci_set_status(&vdev->vpci, 0); /* Reset */
    virtio_pci_set_status(&vdev->vpci,
                          virtio_pci_get_status(&vdev->vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&vdev->vpci, 0)) {
        pr_debug("[VIRTIO-BLK] Failed to negotiate features\n");
        virtio_pci_set_status(&vdev->vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    vdev->vq = virtio_pci_setup_queue(&vdev->vpci, 0);
    if (!vdev->vq) {
        pr_debug("[VIRTIO-BLK] Failed to setup request queue\n");
        virtio_pci_set_status(&vdev->vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    virtio_pci_set_status(&vdev->vpci,
                          virtio_pci_get_status(&vdev->vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    /* Read capacity from device config */
    u64 capacity = 0;
    if (vdev->vpci.device_cfg) {
        capacity = *(volatile u64 *)(vdev->vpci.device_cfg);
    }
    if (capacity == 0) capacity = 2097152; /* Default 1GB */

    vdev->capacity_sectors = capacity;
    /* "vda", "vdb", "vdc", ... one letter per bound device. */
    char name[8];
    name[0] = 'v'; name[1] = 'd'; name[2] = (char)('a' + g_vblk_count);
    name[3] = '\0';
    strncpy(vdev->bdev.name, name, sizeof(vdev->bdev.name) - 1);
    vdev->bdev.sector_size = 512;
    vdev->bdev.sector_count = capacity;
    vdev->bdev.ops = &g_virtio_blk_ops;
    vdev->bdev.driver_data = vdev;

    block_dev_register(&vdev->bdev);
    vdev->active = true;
    dm_set_drvdata(dm, vdev);
    g_vblk_count++;

    pr_debug("[VIRTIO-BLK] Registered block device '%s' (%llu sectors, %llu MB)\n",
             name, (unsigned long long)capacity,
             (unsigned long long)((capacity * 512) / (1024 * 1024)));

    return 0;
}

static void virtio_blk_remove(dm_device_t *dm)
{
    virtio_blk_dev_t *vdev = (virtio_blk_dev_t *)dm_get_drvdata(dm);
    if (!vdev || !vdev->active) return;
    virtio_pci_set_status(&vdev->vpci, 0);
    vdev->active = false;
}

/* 1001/1042: transitional and modern (VIRTIO_F_VERSION_1) device ids. */
static const pci_device_id_t virtio_blk_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1001) },
    { PCI_DEVICE(0x1AF4, 0x1042) },
    { 0 }
};

static pci_driver_t virtio_blk_pci_driver = {
    .drv      = { .name = "virtio_blk" },
    .id_table = virtio_blk_pci_ids,
    .probe    = virtio_blk_probe,
    .remove   = virtio_blk_remove,
};

void virtio_blk_init(void)
{
    pci_driver_register(&virtio_blk_pci_driver);
}
