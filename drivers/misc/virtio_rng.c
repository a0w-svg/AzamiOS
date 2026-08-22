/* ============================================================================
 * AzamiOS — VirtIO Hardware Random Number Generator Driver
 * File: drivers/misc/virtio_rng.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "virtio_rng.h"
#include "../../hal/device.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../fs/vfs.h"

static virtio_rng_dev_t g_vrng_dev;
static spinlock_t g_vrng_lock = SPINLOCK_INIT;

int virtio_rng_get_bytes(void *buf, size_t len)
{
    if (!g_vrng_dev.active || !g_vrng_dev.vq || !buf || len == 0) {
        return -EINVAL;
    }

    if (len > 4096) len = 4096;

    phys_addr_t buf_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)buf);
    if (!buf_phys) return -EFAULT;

    irqflags_t flags = spinlock_lock_irqsave(&g_vrng_lock);

    /* Device writes entropy into the buffer */
    u32 len32 = (u32)len;
    bool is_write = true;
    if (virtqueue_add_chain(g_vrng_dev.vq, &buf_phys, &len32, &is_write, 1, (void *)1) < 0) {
        spinlock_unlock_irqrestore(&g_vrng_lock, flags);
        return -EIO;
    }

    virtqueue_kick(g_vrng_dev.vq);
    virtio_pci_notify(&g_vrng_dev.vpci, 0, g_vrng_dev.vq);

    u32 len_received = 0;
    void *cookie = NULL;
    int timeout = 100000;
    while (!cookie && timeout-- > 0) {
        cookie = virtqueue_get_used(g_vrng_dev.vq, &len_received);
        __asm__ volatile("pause");
    }

    spinlock_unlock_irqrestore(&g_vrng_lock, flags);

    if (!cookie) return -ETIMEDOUT;
    return (int)len_received;
}

/* File operations for /dev/hwrng */
static s64 dev_hwrng_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf) return -EINVAL;
    if (len == 0) return 0;

    int res = virtio_rng_get_bytes(buf, len);
    if (res < 0) return res;
    return (s64)res;
}

static file_operations_t g_hwrng_fops = {
    .read = dev_hwrng_read,
    .write = NULL,
    .open = NULL,
    .release = NULL,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL
};

int virtio_rng_init(device_t *dev)
{
    if (!dev) return -EINVAL;

    pci_device_info_t *info = pci_get_device_info(dev);
    if (!info) return -ENODEV;

    if (info->vendor_id != 0x1AF4) return -ENODEV;
    if (info->device_id != 0x1004 && info->device_id != 0x1044 && info->device_id != 0x1005) {
        return -ENODEV;
    }

    pr_debug("[RNG] Probing VirtIO-RNG device at PCI %02x:%02x.%x...\n",
             info->bus, info->slot, info->func);

    if (virtio_pci_init_device(dev, &g_vrng_dev.vpci) < 0) {
        return -ENODEV;
    }

    virtio_pci_set_status(&g_vrng_dev.vpci, 0); /* Reset */
    virtio_pci_set_status(&g_vrng_dev.vpci,
                          virtio_pci_get_status(&g_vrng_dev.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    if (!virtio_pci_negotiate_features(&g_vrng_dev.vpci, 0)) {
        pr_debug("[RNG] Failed to negotiate features\n");
        virtio_pci_set_status(&g_vrng_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* Allocate request VirtQueue (Queue 0) */
    g_vrng_dev.vq = virtio_pci_setup_queue(&g_vrng_dev.vpci, 0);
    if (!g_vrng_dev.vq) {
        pr_debug("[RNG] Failed to setup request virtqueue\n");
        virtio_pci_set_status(&g_vrng_dev.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENOMEM;
    }

    virtio_pci_set_status(&g_vrng_dev.vpci,
                          virtio_pci_get_status(&g_vrng_dev.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    g_vrng_dev.active = true;

    devfs_register_device("hwrng", &g_hwrng_fops, NULL);
    pr_debug("[RNG] VirtIO-RNG entropy driver ready (/dev/hwrng)\n");

    return 0;
}
