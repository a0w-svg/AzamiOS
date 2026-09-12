/* ============================================================================
 * AzamiOS — virtio-console: host/guest character channel (/dev/hvc0)
 * File: drivers/char/virtio_console.c
 *
 * QEMU's `-device virtio-serial-pci -device virtconsole,chardev=…` exposes a
 * byte pipe between the guest and a host chardev.  Port 0 always owns the
 * first two virtqueues — 0 receives from the host, 1 transmits to it — and a
 * driver that does not negotiate VIRTIO_CONSOLE_F_MULTIPORT gets that port
 * marked connected automatically, which is exactly what a single console
 * wants and keeps the control queues out of the picture.
 *
 * Receive buffers are posted up front and drained into a byte ring on read(),
 * so a reader sees a continuous stream rather than the descriptor boundaries
 * the host happened to write in.  Transmits are synchronous: the frame is
 * handed to the device and the descriptor reclaimed before returning, so the
 * single bounce buffer is never in flight twice.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "../base/pci_bus.h"
#include "../../hal/virtio_pci.h"
#include "../../hal/virtqueue.h"
#include "../../fs/vfs.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

#ifndef POLLIN
#define POLLIN      0x0001
#define POLLOUT     0x0004
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLWRNORM  0x0100
#endif

#define VIRTIO_CONSOLE_RECEIVEQ   0
#define VIRTIO_CONSOLE_TRANSMITQ  1

#define VCON_RX_SLOTS      8
#define VCON_RX_SLOT_SIZE  512
#define VCON_TX_SIZE       2048
#define VCON_RING_SIZE     4096   /* must be a power of two */

typedef struct vcon_device {
    virtio_pci_device_t vpci;
    virtqueue_t *rx_vq;
    virtqueue_t *tx_vq;

    u8   *rx_slots;                /* VCON_RX_SLOTS * VCON_RX_SLOT_SIZE */
    u8   *tx_buf;

    /* Byte ring decoupling descriptor boundaries from read() sizes. */
    u8    ring[VCON_RING_SIZE];
    u32   head, tail;

    spinlock_t lock;
    bool  ready;
} vcon_device_t;

static vcon_device_t g_vcon;

/* ── Byte ring ───────────────────────────────────────────────────────────── */

static u32 vcon_ring_used(void)
{
    return (g_vcon.head - g_vcon.tail) & (VCON_RING_SIZE - 1);
}

static void vcon_ring_put(const u8 *src, u32 len)
{
    for (u32 i = 0; i < len; i++) {
        u32 next = (g_vcon.head + 1) & (VCON_RING_SIZE - 1);
        /* Full ring: drop the oldest byte, as a tty would overrun. */
        if (next == g_vcon.tail) {
            g_vcon.tail = (g_vcon.tail + 1) & (VCON_RING_SIZE - 1);
        }
        g_vcon.ring[g_vcon.head] = src[i];
        g_vcon.head = next;
    }
}

/* ── Queue handling ──────────────────────────────────────────────────────── */

static void vcon_post_rx_slot(u32 slot)
{
    u8 *buf = g_vcon.rx_slots + (size_t)slot * VCON_RX_SLOT_SIZE;
    phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)buf);
    virtqueue_add_buf(g_vcon.rx_vq, phys, VCON_RX_SLOT_SIZE, true,
                      (void *)(uintptr_t)(slot + 1));
}

/* Move everything the host has written into the byte ring. */
static void vcon_drain_rx(void)
{
    if (!g_vcon.ready) return;

    u32 len = 0;
    void *cookie;
    bool posted = false;

    while ((cookie = virtqueue_get_used(g_vcon.rx_vq, &len)) != NULL) {
        u32 slot = (u32)(uintptr_t)cookie - 1;
        if (slot < VCON_RX_SLOTS && len > 0) {
            if (len > VCON_RX_SLOT_SIZE) len = VCON_RX_SLOT_SIZE;
            vcon_ring_put(g_vcon.rx_slots + (size_t)slot * VCON_RX_SLOT_SIZE, len);
        }
        if (slot < VCON_RX_SLOTS) {
            vcon_post_rx_slot(slot);
            posted = true;
        }
    }

    if (posted) {
        virtqueue_kick(g_vcon.rx_vq);
        virtio_pci_notify(&g_vcon.vpci, VIRTIO_CONSOLE_RECEIVEQ, g_vcon.rx_vq);
    }
}

/* ── Character device ────────────────────────────────────────────────────── */

static s64 vcon_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_vcon.ready || !buf) return -(s64)EINVAL;

    /* Plain lock: this device is deliberately polled with its INTx line
     * masked (see vcon_probe), so nothing ever touches g_vcon.lock from
     * interrupt context and there is no need to disable interrupts here. */
    spinlock_lock(&g_vcon.lock);
    vcon_drain_rx();

    u32 avail = vcon_ring_used();
    if (avail == 0) {
        spinlock_unlock(&g_vcon.lock);
        return 0;
    }
    if (avail > len) avail = (u32)len;

    /* The ring may wrap inside this read; copy out in at most two runs. */
    u8 staging[VCON_RING_SIZE];
    for (u32 i = 0; i < avail; i++) {
        staging[i] = g_vcon.ring[(g_vcon.tail + i) & (VCON_RING_SIZE - 1)];
    }
    g_vcon.tail = (g_vcon.tail + avail) & (VCON_RING_SIZE - 1);
    spinlock_unlock(&g_vcon.lock);

    /* Kernel buffer: see the note on the VFS read/write contract in fs/vfs.h.
     * copy_to_user() rejected this destination, so vcon reads always failed. */
    memcpy(buf, staging, avail);
    return (s64)avail;
}

static s64 vcon_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_vcon.ready || !buf || len == 0) return -(s64)EINVAL;
    if (len > VCON_TX_SIZE) len = VCON_TX_SIZE;

    spinlock_lock(&g_vcon.lock);

    /* Kernel buffer — see fs/vfs.h. */
    memcpy(g_vcon.tx_buf, buf, len);

    phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_vcon.tx_buf);
    if (virtqueue_add_buf(g_vcon.tx_vq, phys, (u32)len, false, (void *)1) != 0) {
        spinlock_unlock(&g_vcon.lock);
        return -(s64)EAGAIN;
    }
    virtqueue_kick(g_vcon.tx_vq);
    virtio_pci_notify(&g_vcon.vpci, VIRTIO_CONSOLE_TRANSMITQ, g_vcon.tx_vq);

    /* Reclaim the descriptor before releasing the bounce buffer.  Bounded, so
     * a stalled host cannot wedge a writer forever. Interrupts stay enabled
     * across the wait now — this no longer needs to be an _irqsave section
     * (see vcon_read). */
    for (u32 spins = 0; spins < 1000000; spins++) {
        u32 used_len = 0;
        if (virtqueue_get_used(g_vcon.tx_vq, &used_len) != NULL) break;
        cpu_pause();
    }

    spinlock_unlock(&g_vcon.lock);
    return (s64)len;
}

static int vcon_poll(file_t *filp)
{
    (void)filp;
    if (!g_vcon.ready) return POLLNVAL;

    spinlock_lock(&g_vcon.lock);
    vcon_drain_rx();
    int mask = POLLOUT | POLLWRNORM;
    if (vcon_ring_used() > 0) mask |= POLLIN | POLLRDNORM;
    spinlock_unlock(&g_vcon.lock);
    return mask;
}

static file_operations_t g_vcon_fops = {
    .read  = vcon_read,
    .write = vcon_write,
    .poll  = vcon_poll,
};

/* ── PCI binding ─────────────────────────────────────────────────────────── */

static int vcon_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_vcon.ready) return -EBUSY;   /* one console port is exposed */

    memset(&g_vcon, 0, sizeof(g_vcon));
    spinlock_init(&g_vcon.lock);

    if (virtio_pci_init_device(dm->hal, &g_vcon.vpci) < 0) return -ENODEV;

    virtio_pci_set_status(&g_vcon.vpci, 0);
    virtio_pci_set_status(&g_vcon.vpci,
                          VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    /* Deliberately no MULTIPORT: the device then treats port 0 as connected
     * without a control-queue handshake. */
    if (!virtio_pci_negotiate_features(&g_vcon.vpci, 0)) {
        virtio_pci_set_status(&g_vcon.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }

    g_vcon.rx_vq = virtio_pci_setup_queue(&g_vcon.vpci, VIRTIO_CONSOLE_RECEIVEQ);
    g_vcon.tx_vq = virtio_pci_setup_queue(&g_vcon.vpci, VIRTIO_CONSOLE_TRANSMITQ);
    if (!g_vcon.rx_vq || !g_vcon.tx_vq) {
        virtio_pci_set_status(&g_vcon.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENODEV;
    }

    /* Polled driver: the device must not raise its shared INTx line. */
    g_vcon.rx_vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    g_vcon.tx_vq->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;

    g_vcon.rx_slots = (u8 *)kzalloc(VCON_RX_SLOTS * VCON_RX_SLOT_SIZE);
    g_vcon.tx_buf   = (u8 *)kzalloc(VCON_TX_SIZE);
    if (!g_vcon.rx_slots || !g_vcon.tx_buf) {
        if (g_vcon.rx_slots) kfree(g_vcon.rx_slots);
        if (g_vcon.tx_buf)   kfree(g_vcon.tx_buf);
        virtio_pci_set_status(&g_vcon.vpci, VIRTIO_CONFIG_S_FAILED);
        return -ENOMEM;
    }

    for (u32 i = 0; i < VCON_RX_SLOTS; i++) vcon_post_rx_slot(i);
    virtqueue_kick(g_vcon.rx_vq);

    virtio_pci_set_status(&g_vcon.vpci,
                          virtio_pci_get_status(&g_vcon.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);
    virtio_pci_notify(&g_vcon.vpci, VIRTIO_CONSOLE_RECEIVEQ, g_vcon.rx_vq);

    pci_device_info_t *info = to_pci_info(dm);
    if (info) {
        u16 cmd = pci_config_read16(info->bus, info->slot, info->func, PCI_COMMAND);
        pci_config_write16(info->bus, info->slot, info->func, PCI_COMMAND,
                           (u16)(cmd | PCI_CMD_INTERRUPT_DIS));
    }

    g_vcon.ready = true;
    devfs_register_device("hvc0", &g_vcon_fops, &g_vcon);
    dm_set_drvdata(dm, &g_vcon);

    pr_debug("[VIRTIO-CONSOLE] port 0 at /dev/hvc0 (%u receive slots)\n", VCON_RX_SLOTS);
    return 0;
}

static void vcon_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_vcon.ready) return;
    g_vcon.ready = false;
    virtio_pci_set_status(&g_vcon.vpci, 0);
}

static const pci_device_id_t vcon_pci_ids[] = {
    { PCI_DEVICE(0x1AF4, 0x1043) },   /* virtio-console (modern) */
    { PCI_DEVICE(0x1AF4, 0x1003) },   /* virtio-console (legacy) */
    { 0 }
};

static pci_driver_t vcon_pci_driver = {
    .drv      = { .name = "virtio_console" },
    .id_table = vcon_pci_ids,
    .probe    = vcon_probe,
    .remove   = vcon_remove,
};

void virtio_console_init(void)
{
    pci_driver_register(&vcon_pci_driver);
}
