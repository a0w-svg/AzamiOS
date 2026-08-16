/* ============================================================================
 * AzamiOS — VirtIO Network Device Driver Implementation
 * File: drivers/net/virtio_net.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/net.h"
#include "virtio_net.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

extern void net_process_incoming(const u8 *pkt, size_t len);
extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static virtio_net_dev_t g_vnet;

#define RX_BUFFER_COUNT 32
#define RX_BUFFER_SIZE  2048

static u8 *g_rx_buffers[RX_BUFFER_COUNT];

static void fill_rx_buffers(void)
{
    for (int i = 0; i < RX_BUFFER_COUNT; i++) {
        if (!g_rx_buffers[i]) {
            g_rx_buffers[i] = (u8 *)kmalloc(RX_BUFFER_SIZE);
        }
        if (g_rx_buffers[i]) {
            phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_rx_buffers[i]);
            virtqueue_add_buf(g_vnet.rx_vq, phys, RX_BUFFER_SIZE, true, (void *)(uintptr_t)(i + 1));
        }
    }
    virtqueue_kick(g_vnet.rx_vq);
    virtio_pci_notify(&g_vnet.vpci, 0, g_vnet.rx_vq);
}

s64 virtio_net_send_packet(const void *data, size_t len)
{
    if (!g_vnet.active || !g_vnet.tx_vq || !data || len == 0 || len > 1514) return -EINVAL;

    struct virtio_net_hdr hdr;
    __builtin_memset(&hdr, 0, sizeof(hdr));

    phys_addr_t hdr_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)&hdr);
    phys_addr_t data_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)data);

    if (!hdr_phys || !data_phys) return -EFAULT;

    phys_addr_t addrs[2] = { hdr_phys, data_phys };
    u32 lens[2] = { sizeof(hdr), (u32)len };
    bool is_write[2] = { false, false };

    if (virtqueue_add_chain(g_vnet.tx_vq, addrs, lens, is_write, 2, (void *)1) < 0) {
        return -EIO;
    }

    virtqueue_kick(g_vnet.tx_vq);
    virtio_pci_notify(&g_vnet.vpci, 1, g_vnet.tx_vq);

    void *cookie = NULL;
    while (!cookie) {
        cookie = virtqueue_get_used(g_vnet.tx_vq, NULL);
        __asm__ volatile("pause");
    }

    return (s64)len;
}

void virtio_net_get_mac(u8 mac_out[6])
{
    if (mac_out) memcpy(mac_out, g_vnet.mac, 6);
}

void virtio_net_poll(void)
{
    if (!g_vnet.active || !g_vnet.rx_vq) return;

    u32 len = 0;
    void *cookie = virtqueue_get_used(g_vnet.rx_vq, &len);
    while (cookie) {
        int idx = (int)(uintptr_t)cookie - 1;
        if (idx >= 0 && idx < RX_BUFFER_COUNT && len > sizeof(struct virtio_net_hdr)) {
            u8 *pkt = g_rx_buffers[idx] + sizeof(struct virtio_net_hdr);
            size_t pkt_len = len - sizeof(struct virtio_net_hdr);
            net_process_incoming(pkt, pkt_len);

            phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_rx_buffers[idx]);
            virtqueue_add_buf(g_vnet.rx_vq, phys, RX_BUFFER_SIZE, true, cookie);
        }
        cookie = virtqueue_get_used(g_vnet.rx_vq, &len);
    }
    virtqueue_kick(g_vnet.rx_vq);
}

static s64 net_fops_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)buf; (void)len; (void)offset;
    virtio_net_poll();
    return 0;
}

static s64 net_fops_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return virtio_net_send_packet(buf, len);
}

static s64 net_fops_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    return net_ioctl(cmd, arg);
}

static file_operations_t g_vnet_fops = {
    .read = net_fops_read,
    .write = net_fops_write,
    .ioctl = net_fops_ioctl,
};

int virtio_net_init(device_t *pci_dev)
{
    pci_device_info_t *info = pci_get_device_info(pci_dev);
    if (!info) return -1;

    if (info->vendor_id != 0x1AF4 || (info->device_id != 0x1000 && info->device_id != 0x1041)) {
        return -1;
    }

    pr_debug("[VIRTIO-NET] Found VirtIO Network Controller at PCI %02x:%02x.%x\n",
             info->bus, info->slot, info->func);

    if (virtio_pci_init_device(pci_dev, &g_vnet.vpci) < 0) {
        pr_debug("[VIRTIO-NET] Failed to initialize VirtIO PCI transport\n");
        return -1;
    }

    virtio_pci_set_status(&g_vnet.vpci, 0);
    virtio_pci_set_status(&g_vnet.vpci,
                          virtio_pci_get_status(&g_vnet.vpci) | VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER);

    u64 features = VIRTIO_NET_F_MAC;
    if (!virtio_pci_negotiate_features(&g_vnet.vpci, features)) {
        pr_debug("[VIRTIO-NET] Failed to negotiate features\n");
        virtio_pci_set_status(&g_vnet.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    g_vnet.rx_vq = virtio_pci_setup_queue(&g_vnet.vpci, 0);
    g_vnet.tx_vq = virtio_pci_setup_queue(&g_vnet.vpci, 1);

    if (!g_vnet.rx_vq || !g_vnet.tx_vq) {
        pr_debug("[VIRTIO-NET] Failed to setup queues\n");
        virtio_pci_set_status(&g_vnet.vpci, VIRTIO_CONFIG_S_FAILED);
        return -1;
    }

    /* Read MAC address from device configuration */
    if (g_vnet.vpci.device_cfg) {
        for (int i = 0; i < 6; i++) {
            g_vnet.mac[i] = g_vnet.vpci.device_cfg[i];
        }
    } else {
        g_vnet.mac[0] = 0x52; g_vnet.mac[1] = 0x54; g_vnet.mac[2] = 0x00;
        g_vnet.mac[3] = 0x12; g_vnet.mac[4] = 0x34; g_vnet.mac[5] = 0x58;
    }

    fill_rx_buffers();

    virtio_pci_set_status(&g_vnet.vpci,
                          virtio_pci_get_status(&g_vnet.vpci) | VIRTIO_CONFIG_S_DRIVER_OK);

    g_vnet.active = true;

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strcpy(ndev.name, "virtio-net");
    memcpy(ndev.mac, g_vnet.mac, 6);
    ndev.send = virtio_net_send_packet;
    extern int net_register_device(const net_device_t *dev);
    net_register_device(&ndev);

    devfs_register_device("net0", &g_vnet_fops, &g_vnet);

    pr_debug("[VIRTIO-NET] MAC: %02x:%02x:%02x:%02x:%02x:%02x initialized successfully\n",
             g_vnet.mac[0], g_vnet.mac[1], g_vnet.mac[2],
             g_vnet.mac[3], g_vnet.mac[4], g_vnet.mac[5]);

    return 0;
}
