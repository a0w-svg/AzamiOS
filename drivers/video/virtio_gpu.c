/* ============================================================================
 * AzamiOS — VirtIO-GPU Driver Implementation
 * File: drivers/video/virtio_gpu.c
 *
 * Implements initialization and command submission for VirtIO-GPU.
 * ============================================================================ */

#include "virtio_gpu.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include <azami/debug.h>

virtio_gpu_state_t g_gpu;
static spinlock_t g_gpu_lock = SPINLOCK_INIT;

int virtio_gpu_send_command(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size)
{
    phys_addr_t cmd_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)cmd);
    phys_addr_t resp_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)resp);

    phys_addr_t addrs[2] = {cmd_phys, resp_phys};
    u32 lens[2] = {cmd_size, resp_size};
    bool is_write[2] = {false, true}; /* Device reads cmd, writes resp */

    irqflags_t flags = spinlock_lock_irqsave(&g_gpu_lock);

    int cookie = 1;
    if (virtqueue_add_chain(gpu->controlq, addrs, lens, is_write, 2, (void *)(uintptr_t)cookie) < 0) {
        spinlock_unlock_irqrestore(&g_gpu_lock, flags);
        pr_debug("[VIRTIO-GPU] Failed to add command to virtqueue\n");
        return -1;
    }

    virtqueue_kick(gpu->controlq);
    virtio_pci_notify(&gpu->vpci, 0, gpu->controlq);

    /* Spin wait for completion (simple polling for now) */
    void *returned_cookie = NULL;
    while (!returned_cookie) {
        returned_cookie = virtqueue_get_used(gpu->controlq, NULL);
        /* In a real OS we'd yield or wait for an interrupt here */
        __asm__ volatile("pause");
    }

    spinlock_unlock_irqrestore(&g_gpu_lock, flags);

    struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)resp;
    if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
        pr_debug("[VIRTIO-GPU] Command failed with error 0x%x\n", hdr->type);
        return -1;
    }

    return 0;
}

int virtio_gpu_init(device_t *pci_dev)
{
    pci_device_info_t *info = pci_get_device_info(pci_dev);
    if (!info) return -1;

    /* Check vendor and device ID for VirtIO-GPU (1AF4:1050) */
    if (info->vendor_id != 0x1AF4 || info->device_id != 0x1050) {
        return -1;
    }

    pr_debug("[VIRTIO-GPU] Found VirtIO GPU at PCI %02x:%02x.%x\n", info->bus, info->slot, info->func);

    if (virtio_pci_init_device(pci_dev, &g_gpu.vpci) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to initialize VirtIO PCI transport\n");
        return -1;
    }

    /* 1. Reset device */
    virtio_pci_set_status(&g_gpu.vpci, 0);

    /* 2. Set ACKNOWLEDGE and DRIVER */
    virtio_pci_set_status(&g_gpu.vpci, virtio_pci_get_status(&g_gpu.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* 3. Negotiate features (we don't request any special 3D features) */
    if (!virtio_pci_negotiate_features(&g_gpu.vpci, 0)) {
        pr_debug("[VIRTIO-GPU] Failed to negotiate features\n");
        virtio_pci_set_status(&g_gpu.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* 4. Setup queues */
    g_gpu.controlq = virtio_pci_setup_queue(&g_gpu.vpci, 0);
    if (!g_gpu.controlq) {
        pr_debug("[VIRTIO-GPU] Failed to setup control queue\n");
        virtio_pci_set_status(&g_gpu.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    g_gpu.cursorq = virtio_pci_setup_queue(&g_gpu.vpci, 1);
    if (!g_gpu.cursorq) {
        pr_debug("[VIRTIO-GPU] Failed to setup cursor queue\n");
        virtio_pci_set_status(&g_gpu.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* 5. Set DRIVER_OK */
    virtio_pci_set_status(&g_gpu.vpci, virtio_pci_get_status(&g_gpu.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);
    pr_debug("[VIRTIO-GPU] Device initialized successfully\n");

    return 0;
}
