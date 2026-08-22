/* ============================================================================
 * AzamiOS — VirtIO PCI Transport Implementation
 * File: hal/virtio_pci.c
 *
 * Scans PCI capabilities to locate VirtIO 1.0 configuration structures,
 * maps them into memory, and provides accessors.
 * ============================================================================ */

#include "virtio_pci.h"
#include "../arch/x86_64/mm/vmm.h"
#include <azami/debug.h>

#define PCI_CAPABILITY_LIST 0x34
#define PCI_STATUS_CAP_LIST 0x10

static void *map_virtio_cap(device_t *dev, u8 bar, u32 offset, u32 length)
{
    phys_addr_t bar_phys = pci_get_bar(dev, bar);
    if (!bar_phys) return NULL;
    
    /* Just map one page for now, assuming structures are small and within a page */
    /* Ideally we'd calculate exactly how many pages are needed. */
    void *virt_base = vmm_map_io(bar_phys, (offset + length + 0xFFF) & ~0xFFF);
    if (!virt_base) return NULL;
    
    return (void *)((u8 *)virt_base + offset);
}

int virtio_pci_init_device(device_t *dev, virtio_pci_device_t *virtio_dev)
{
    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info) return -1;
    
    virtio_dev->pci_dev = dev;
    
    /* Check if capabilities list is available */
    u16 status = pci_config_read16(info->bus, info->slot, info->func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        pr_debug("[VIRTIO] Device has no capabilities list.\n");
        return -1;
    }
    
    u8 cap_ptr = pci_config_read8(info->bus, info->slot, info->func, PCI_CAPABILITY_LIST);
    cap_ptr &= ~3; /* align */
    
    while (cap_ptr != 0) {
        u8 cap_id = pci_config_read8(info->bus, info->slot, info->func, cap_ptr);
        if (cap_id == 0x09) { /* PCI_CAP_ID_VNDR */
            u8 cfg_type = pci_config_read8(info->bus, info->slot, info->func, cap_ptr + 3);
            u8 bar      = pci_config_read8(info->bus, info->slot, info->func, cap_ptr + 4);
            u32 offset  = pci_config_read32(info->bus, info->slot, info->func, cap_ptr + 8);
            u32 length  = pci_config_read32(info->bus, info->slot, info->func, cap_ptr + 12);
            
            switch (cfg_type) {
                case VIRTIO_PCI_CAP_COMMON_CFG:
                    virtio_dev->common_cfg = map_virtio_cap(dev, bar, offset, length);
                    break;
                case VIRTIO_PCI_CAP_NOTIFY_CFG:
                    virtio_dev->notify_base = map_virtio_cap(dev, bar, offset, length);
                    virtio_dev->notify_multiplier = pci_config_read32(info->bus, info->slot, info->func, cap_ptr + 16);
                    break;
                case VIRTIO_PCI_CAP_ISR_CFG:
                    virtio_dev->isr_cfg = map_virtio_cap(dev, bar, offset, length);
                    break;
                case VIRTIO_PCI_CAP_DEVICE_CFG:
                    virtio_dev->device_cfg = map_virtio_cap(dev, bar, offset, length);
                    break;
            }
        }
        cap_ptr = pci_config_read8(info->bus, info->slot, info->func, cap_ptr + 1);
        cap_ptr &= ~3;
    }
    
    if (!virtio_dev->common_cfg || !virtio_dev->notify_base || !virtio_dev->isr_cfg) {
        pr_debug("[VIRTIO] Failed to map required VirtIO capabilities.\n");
        return -1;
    }
    
    /* Enable Bus Mastering so VirtIO can DMA */
    pci_enable_bus_mastering(dev);
    
    return 0;
}

void virtio_pci_set_status(virtio_pci_device_t *virtio_dev, u8 status)
{
    virtio_dev->common_cfg->device_status = status;
}

u8 virtio_pci_get_status(virtio_pci_device_t *virtio_dev)
{
    return virtio_dev->common_cfg->device_status;
}

bool virtio_pci_negotiate_features(virtio_pci_device_t *virtio_dev, u64 requested_features)
{
    /* Always request VERSION_1 */
    requested_features |= VIRTIO_F_VERSION_1;
    
    /* Write feature bits 0-31 */
    virtio_dev->common_cfg->driver_feature_select = 0;
    virtio_dev->common_cfg->driver_feature = (u32)requested_features;
    
    /* Write feature bits 32-63 */
    virtio_dev->common_cfg->driver_feature_select = 1;
    virtio_dev->common_cfg->driver_feature = (u32)(requested_features >> 32);
    
    /* Set FEATURES_OK */
    u8 status = virtio_pci_get_status(virtio_dev);
    virtio_pci_set_status(virtio_dev, status | VIRTIO_CONFIG_S_FEATURES_OK);
    
    /* Read back to ensure device accepted them */
    status = virtio_pci_get_status(virtio_dev);
    return (status & VIRTIO_CONFIG_S_FEATURES_OK) != 0;
}

virtqueue_t *virtio_pci_setup_queue(virtio_pci_device_t *virtio_dev, u16 queue_index)
{
    virtio_dev->common_cfg->queue_select = queue_index;
    u16 queue_size = virtio_dev->common_cfg->queue_size;
    
    if (queue_size == 0) {
        return NULL; /* Queue not available */
    }
    
    virtqueue_t *vq = virtqueue_create(queue_size);
    if (!vq) return NULL;
    
    /* Provide the queue addresses to the device */
    virtio_dev->common_cfg->queue_desc  = vq->queue_phys;
    virtio_dev->common_cfg->queue_avail = vq->queue_phys + (queue_size * sizeof(struct vring_desc));
    virtio_dev->common_cfg->queue_used  = vq->queue_phys + ALIGN_UP(queue_size * sizeof(struct vring_desc) + sizeof(u16) * (3 + queue_size), 4096);
    
    /* Enable the queue */
    virtio_dev->common_cfg->queue_enable = 1;
    
    return vq;
}

void virtio_pci_notify(virtio_pci_device_t *virtio_dev, u16 queue_index, virtqueue_t *vq)
{
    virtio_dev->common_cfg->queue_select = queue_index;
    u16 notify_off = virtio_dev->common_cfg->queue_notify_off;
    
    /* The address to write to is notify_base + (notify_off * notify_multiplier) */
    volatile u16 *notify_addr = (volatile u16 *)(virtio_dev->notify_base + (notify_off * virtio_dev->notify_multiplier));
    
    /* Write the queue index to notify the device */
    *notify_addr = queue_index;
}
