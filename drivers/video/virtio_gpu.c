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
#include "../../arch/x86_64/cpu/hwaccel.h"
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

    /* Plain lock, not _irqsave: nothing ever touches g_gpu_lock from
     * interrupt context (this driver is polled end to end — see
     * virtio_gpu_init, which never unmasks the device's INTx line), so a
     * ticket lock alone is enough to serialize concurrent callers. Disabling
     * interrupts for the whole submit+poll used to mean a page flip — or,
     * via the cursor queue, every pointer move — stalled this core's timer
     * tick and IPI delivery for the entire host round trip. Under TCG
     * (no KVM) that round trip is not free, and it was happening on the
     * compositor's hot path. */
    spinlock_lock(&g_gpu_lock);

    int cookie = 1;
    if (virtqueue_add_chain(vq, addrs, lens, is_write, ndesc, (void *)(uintptr_t)cookie) < 0) {
        spinlock_unlock(&g_gpu_lock);
        pr_debug("[VIRTIO-GPU] Failed to add command to virtqueue %u\n", queue_index);
        return -1;
    }

    virtqueue_kick(vq);
    virtio_pci_notify(&gpu->vpci, queue_index, vq);

    /*
     * Poll for completion. Bounded on purpose: this runs on the compositor's
     * path, so a device that never retires the descriptor must fail the
     * operation, not wedge the caller forever. The cap is far longer than any
     * real round trip (microseconds under QEMU); hitting it means the queue
     * is genuinely stuck.
     */
    void *returned_cookie = NULL;
    u64 spins = 0;
    const u64 SPIN_LIMIT = 200000000ULL;
    while (!returned_cookie) {
        returned_cookie = virtqueue_get_used(vq, NULL);
        if (returned_cookie) break;
        if (++spins >= SPIN_LIMIT) {
            spinlock_unlock(&g_gpu_lock);
            pr_debug("[VIRTIO-GPU] queue %u timed out waiting for completion\n",
                     queue_index);
            return -1;
        }
        hw_spin_wait((u32)spins);
    }

    spinlock_unlock(&g_gpu_lock);

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

bool virtio_gpu_edid_supported(void)
{
    return (g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_EDID)) != 0;
}

/* Not its own pci_driver_t: virtgpu_drm.c already registers the sole PCI
 * driver for 1AF4:1050/1010 (its KMS probe), and this device model only lets
 * one driver bind a given device — a second registration with the same
 * id_table here would just lose that race and never run. So this stays a
 * plain bring-up function, called from virtgpu_pci_probe() once it already
 * knows a real virtio-gpu device exists; that is what makes this dynamic
 * rather than the old unconditional call from kernel/main.c. */
int virtio_gpu_init(device_t *hal_dev)
{
    /* g_gpu is a single global instance — this OS drives one display
     * adapter — so a second call is refused rather than silently
     * reinitializing the transport out from under the first. */
    if (g_gpu.controlq) return -EBUSY;

    pci_device_info_t *info = pci_get_device_info(hal_dev);
    if (!info) return -ENODEV;

    pr_debug("[VIRTIO-GPU] Found VirtIO GPU at PCI %02x:%02x.%x\n", info->bus, info->slot, info->func);

    if (virtio_pci_init_device(hal_dev, &g_gpu.vpci) < 0) {
        pr_debug("[VIRTIO-GPU] Failed to initialize VirtIO PCI transport\n");
        return -1;
    }

    /* 1. Reset device */
    virtio_pci_set_status(&g_gpu.vpci, 0);

    /* 2. Set ACKNOWLEDGE and DRIVER */
    virtio_pci_set_status(&g_gpu.vpci, virtio_pci_get_status(&g_gpu.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* 3. Negotiate features. We don't request 3D (VIRTIO_GPU_F_VIRGL), but
     * do ask for EDID: negotiation only grants bits the device actually
     * offers, so requesting it is free on a host that predates the feature
     * — negotiated_features simply won't have the bit set and
     * virtio_gpu_edid_supported() reports that below. */
    if (!virtio_pci_negotiate_features(&g_gpu.vpci, 1ULL << VIRTIO_GPU_F_EDID)) {
        pr_debug("[VIRTIO-GPU] Failed to negotiate features\n");
        virtio_pci_set_status(&g_gpu.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }
    if (g_gpu.vpci.negotiated_features & (1ULL << VIRTIO_GPU_F_EDID)) {
        pr_debug("[VIRTIO-GPU] Device offers EDID\n");
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
