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

/* Submit one command/response pair on a chosen queue and block until the
 * device retires it. queue_index selects both the virtqueue and the notify
 * register: 0 = controlq (2D resource management), 1 = cursorq (cursor). */
static int virtio_gpu_submit(virtio_gpu_state_t *gpu, u16 queue_index,
                             virtqueue_t *vq, void *cmd, u32 cmd_size,
                             void *resp, u32 resp_size)
{
    phys_addr_t addrs[2];
    u32         lens[2];
    bool        is_write[2] = {false, true}; /* Device reads cmd, writes resp */
    u32         ndesc = 1;

    addrs[0] = vmm_translate(vmm_kernel_space(), (virt_addr_t)cmd);
    lens[0]  = cmd_size;

    /* The cursor queue is out-only — a MOVE/UPDATE_CURSOR takes no response.
     * Callers signal that by passing resp == NULL. */
    if (resp) {
        addrs[1] = vmm_translate(vmm_kernel_space(), (virt_addr_t)resp);
        lens[1]  = resp_size;
        ndesc    = 2;
    }

    if (!vq) {
        pr_debug("[VIRTIO-GPU] queue %u not available\n", queue_index);
        return -1;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_gpu_lock);

    int cookie = 1;
    if (virtqueue_add_chain(vq, addrs, lens, is_write, ndesc, (void *)(uintptr_t)cookie) < 0) {
        spinlock_unlock_irqrestore(&g_gpu_lock, flags);
        pr_debug("[VIRTIO-GPU] Failed to add command to virtqueue %u\n", queue_index);
        return -1;
    }

    virtqueue_kick(vq);
    virtio_pci_notify(&gpu->vpci, queue_index, vq);

    /*
     * Poll for completion with interrupts off. Bounded on purpose: this runs
     * on the compositor's path (a page flip, and — via the cursor queue — every
     * pointer move), so a device that never retires the descriptor must fail
     * the operation, not wedge the core forever with IRQs masked. The cap is
     * far longer than any real round trip (microseconds under QEMU); hitting it
     * means the queue is genuinely stuck.
     */
    void *returned_cookie = NULL;
    u64 spins = 0;
    const u64 SPIN_LIMIT = 200000000ULL;
    while (!returned_cookie) {
        returned_cookie = virtqueue_get_used(vq, NULL);
        if (returned_cookie) break;
        if (++spins >= SPIN_LIMIT) {
            spinlock_unlock_irqrestore(&g_gpu_lock, flags);
            pr_debug("[VIRTIO-GPU] queue %u timed out waiting for completion\n",
                     queue_index);
            return -1;
        }
        __asm__ volatile("pause");
    }

    spinlock_unlock_irqrestore(&g_gpu_lock, flags);

    if (resp) {
        struct virtio_gpu_ctrl_hdr *hdr = (struct virtio_gpu_ctrl_hdr *)resp;
        if (hdr->type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
            pr_debug("[VIRTIO-GPU] Command failed with error 0x%x\n", hdr->type);
            return -1;
        }
    }

    return 0;
}

int virtio_gpu_send_command(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size)
{
    return virtio_gpu_submit(gpu, 0, gpu->controlq, cmd, cmd_size, resp, resp_size);
}

int virtio_gpu_send_cursor(virtio_gpu_state_t *gpu, void *cmd, u32 cmd_size, void *resp, u32 resp_size)
{
    return virtio_gpu_submit(gpu, 1, gpu->cursorq, cmd, cmd_size, resp, resp_size);
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
