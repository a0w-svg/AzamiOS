/* ============================================================================
 * AzamiOS — Realtek RTL8139 Fast Ethernet NIC Driver
 * File: drivers/net/rtl8139.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "rtl8139.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

#define RTL8139_RX_BUF_SIZE (8192 + 16 + 1500)
#define RTL8139_TX_BUF_SIZE 2048

static u16        g_rtl_io_base = 0;
static u8         g_rtl_mac[6] = {0};
static u8        *g_rtl_rx_buf = NULL;
static phys_addr_t g_rtl_rx_phys = 0;
static u32        g_rtl_rx_offset = 0;
static u8        *g_rtl_tx_bufs[4] = {NULL};
static phys_addr_t g_rtl_tx_phys[4] = {0};
static u8         g_rtl_tx_cur = 0;
static spinlock_t g_rtl_lock = SPINLOCK_INIT;
static bool       g_rtl_ready = false;

/* Register Offsets */
#define REG_MAC0    0x00
#define REG_MAR0    0x08
#define REG_TSD0    0x10
#define REG_TSAD0   0x20
#define REG_RBSTART 0x30
#define REG_CR      0x37
#define REG_CAPR    0x38
#define REG_CBR     0x3A
#define REG_IMR     0x3C
#define REG_ISR     0x3E
#define REG_TCR     0x40
#define REG_RCR     0x44
#define REG_CONFIG1 0x52

s64 rtl8139_send_packet(const void *data, size_t len)
{
    if (!g_rtl_ready || !data || len == 0 || len > RTL8139_TX_BUF_SIZE) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_rtl_lock);
    u8 tx_idx = g_rtl_tx_cur;

    memcpy(g_rtl_tx_bufs[tx_idx], data, len);
    if (len < 60) {
        memset(g_rtl_tx_bufs[tx_idx] + len, 0, 60 - len);
        len = 60;
    }

    outl(g_rtl_io_base + REG_TSD0 + (tx_idx * 4), (u32)len & 0x1FFF);
    g_rtl_tx_cur = (tx_idx + 1) % 4;

    spinlock_unlock_irqrestore(&g_rtl_lock, flags);
    return (s64)len;
}

s64 rtl8139_recv_packet(void *buf, size_t max_len)
{
    if (!g_rtl_ready || !buf || max_len == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_rtl_lock);
    if (inb(g_rtl_io_base + REG_CR) & 0x01) { /* Buffer Empty */
        spinlock_unlock_irqrestore(&g_rtl_lock, flags);
        return -(s64)EAGAIN;
    }

    u32 offset = g_rtl_rx_offset;
    u16 *hdr = (u16 *)(g_rtl_rx_buf + offset);
    u16 status = hdr[0];
    u16 len = hdr[1];

    if (!(status & 1) || len == 0) {
        spinlock_unlock_irqrestore(&g_rtl_lock, flags);
        return -(s64)EAGAIN;
    }

    size_t pkt_len = (len > 4) ? (len - 4) : 0;
    size_t copy_len = (pkt_len < max_len) ? pkt_len : max_len;

    memcpy(buf, g_rtl_rx_buf + offset + 4, copy_len);

    offset = (offset + len + 4 + 3) & ~3UL;
    offset %= 8192;
    g_rtl_rx_offset = offset;
    outw(g_rtl_io_base + REG_CAPR, (u16)(offset - 16));

    spinlock_unlock_irqrestore(&g_rtl_lock, flags);
    return (s64)copy_len;
}

static s64 rtl_devfs_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return rtl8139_recv_packet(buf, len);
}

static s64 rtl_devfs_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return rtl8139_send_packet(buf, len);
}

static s64 rtl_devfs_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    return net_ioctl(cmd, arg);
}

static file_operations_t g_rtl_fops = {
    .read = rtl_devfs_read,
    .write = rtl_devfs_write,
    .ioctl = rtl_devfs_ioctl,
};

int rtl8139_init(void)
{
    device_t *pci_bus = device_find("PCI0");
    if (!pci_bus) return -1;

    device_t *pci_dev = NULL;
    pci_device_info_t *info = NULL;

    device_t *curr = pci_bus->children;
    while (curr) {
        if (curr->driver_data) {
            pci_device_info_t *p = (pci_device_info_t *)curr->driver_data;
            if (p->vendor_id == 0x10EC && p->device_id == 0x8139) {
                pci_dev = curr;
                info = p;
                break;
            }
        }
        curr = curr->sibling;
    }

    if (!pci_dev || !info) {
        return -1;
    }

    pr_debug("[RTL8139] Found Realtek RTL8139 NIC at PCI %02x:%02x.%u\n",
             info->bus, info->slot, info->func);

    pci_enable_bus_mastering(pci_dev);

    u16 io_base = (u16)(info->bar[0] & ~0x3);
    if (!io_base) return -1;
    g_rtl_io_base = io_base;

    /* Power on: write 0x00 to Config1 */
    outb(io_base + REG_CONFIG1, 0x00);

    /* Software reset */
    outb(io_base + REG_CR, 0x10);
    while (inb(io_base + REG_CR) & 0x10) cpu_pause();

    /* Read MAC address */
    for (int i = 0; i < 6; i++) {
        g_rtl_mac[i] = inb(io_base + REG_MAC0 + i);
    }
    pr_debug("[RTL8139] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_rtl_mac[0], g_rtl_mac[1], g_rtl_mac[2],
             g_rtl_mac[3], g_rtl_mac[4], g_rtl_mac[5]);

    /* Allocate RX buffer */
    g_rtl_rx_phys = pmm_alloc_pages(3);
    if (!g_rtl_rx_phys) return -1;
    g_rtl_rx_buf = (u8 *)PHYS_TO_VIRT(g_rtl_rx_phys);
    memset(g_rtl_rx_buf, 0, 3 * PAGE_SIZE);
    outl(io_base + REG_RBSTART, (u32)g_rtl_rx_phys);

    /* Allocate 4 TX buffers */
    for (int i = 0; i < 4; i++) {
        g_rtl_tx_phys[i] = pmm_alloc_page();
        if (!g_rtl_tx_phys[i]) return -1;
        g_rtl_tx_bufs[i] = (u8 *)PHYS_TO_VIRT(g_rtl_tx_phys[i]);
        outl(io_base + REG_TSAD0 + (i * 4), (u32)g_rtl_tx_phys[i]);
    }

    outl(io_base + REG_RCR, 0x0000008F);
    outb(io_base + REG_CR, 0x0C);

    g_rtl_rx_offset = 0;
    g_rtl_tx_cur = 0;
    g_rtl_ready = true;

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strcpy(ndev.name, "rtl8139");
    memcpy(ndev.mac, g_rtl_mac, 6);
    ndev.send = rtl8139_send_packet;
    ndev.recv = rtl8139_recv_packet;
    net_register_device(&ndev);


    extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);
    devfs_register_device("net0", &g_rtl_fops, NULL);

    pr_debug("[RTL8139] Initialized successfully.\n");
    return 0;
}

void rtl8139_get_mac(u8 mac_out[6])
{
    if (mac_out) {
        memcpy(mac_out, g_rtl_mac, 6);
    }
}
