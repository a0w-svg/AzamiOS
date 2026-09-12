/* ============================================================================
 * AzamiOS — Realtek RTL8169 / RTL8168 Gigabit Ethernet Driver
 * File: drivers/net/rtl8169.c
 *
 * Implements a modern 64-bit descriptor ring driver for Realtek PCI/PCIe
 * Gigabit network adapters (RTL8169, RTL8168, RTL8111, RTL8167).
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "rtl8169.h"
#include "../../hal/pci.h"
#include "../../hal/irq.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* Register Offsets */
#define REG_IDR0      0x00   /* MAC bytes 0-5 */
#define REG_MAR0      0x08   /* Multicast filter */
#define REG_TNPDS     0x20   /* Tx Normal Priority Descriptor Start address */
#define REG_COMMAND   0x37   /* Command register */
#define REG_TPPOLL    0x38   /* Transmit priority poll */
#define REG_IMR       0x3C   /* Interrupt Mask Register */
#define REG_ISR       0x3E   /* Interrupt Status Register */
#define REG_TCR       0x40   /* Transmit Configuration */
#define REG_RCR       0x44   /* Receive Configuration */
#define REG_9346CR    0x50   /* 93C46 command register (unlock/lock) */
#define REG_RMS       0xDA   /* Rx Max packet size (16-bit) */
#define REG_CPCR      0xE0   /* C+ Command register */
#define REG_RDSAR     0xE4   /* Rx Descriptor Start Address */
#define REG_MTPS      0xEC   /* Max Tx packet size (8-bit) */

/* Command register bits */
#define CMD_RST       0x10
#define CMD_RE        0x08
#define CMD_TE        0x04

/* Interrupt bits */
#define INT_ROK       (1 << 0)
#define INT_RER       (1 << 1)
#define INT_TOK       (1 << 2)
#define INT_TER       (1 << 3)
#define INT_RXOVW     (1 << 4)

static u16        g_r8169_io_base = 0;
static u8         g_r8169_mac[6] = {0};
static u8         g_r8169_irq = 0;
static bool       g_r8169_ready = false;

static rtl8169_desc_t *g_rx_ring = NULL;
static phys_addr_t     g_rx_ring_phys = 0;
static u8             *g_rx_buffers[RTL8169_NUM_RX_DESC];
static phys_addr_t     g_rx_bufs_phys[RTL8169_NUM_RX_DESC];
static u32             g_rx_cur = 0;

static rtl8169_desc_t *g_tx_ring = NULL;
static phys_addr_t     g_tx_ring_phys = 0;
static u8             *g_tx_buffers[RTL8169_NUM_TX_DESC];
static phys_addr_t     g_tx_bufs_phys[RTL8169_NUM_TX_DESC];
static u32             g_tx_cur = 0;

static spinlock_t g_r8169_rx_lock = SPINLOCK_INIT;
static spinlock_t g_r8169_tx_lock = SPINLOCK_INIT;

void rtl8169_get_mac(u8 mac_out[6])
{
    if (mac_out) {
        memcpy(mac_out, g_r8169_mac, 6);
    }
}

s64 rtl8169_send_packet(const void *data, size_t len)
{
    if (!g_r8169_ready || !data || len == 0 || len > RTL8169_PKT_BUF_SIZE) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_r8169_tx_lock);
    u32 cur = g_tx_cur;

    /* Wait if descriptor is currently owned by hardware */
    int timeout = 100000;
    while ((g_tx_ring[cur].opts1 & RTL8169_DESC_OWN) && --timeout > 0) {
        cpu_pause();
    }
    if (timeout <= 0) {
        spinlock_unlock_irqrestore(&g_r8169_tx_lock, flags);
        return -(s64)EBUSY;
    }

    memcpy(g_tx_buffers[cur], data, len);
    if (len < 60) {
        memset(g_tx_buffers[cur] + len, 0, 60 - len);
        len = 60;
    }

    u32 opts = RTL8169_DESC_OWN | RTL8169_DESC_FS | RTL8169_DESC_LS | ((u32)len & 0x3FFF);
    if (cur == RTL8169_NUM_TX_DESC - 1) {
        opts |= RTL8169_DESC_EOR;
    }
    g_tx_ring[cur].opts1 = opts;

    /* Poll normal priority transmit */
    outb(g_r8169_io_base + REG_TPPOLL, 0x40);

    g_tx_cur = (cur + 1) % RTL8169_NUM_TX_DESC;
    spinlock_unlock_irqrestore(&g_r8169_tx_lock, flags);
    return (s64)len;
}

s64 rtl8169_recv_packet(void *buf, size_t max_len)
{
    if (!g_r8169_ready || !buf || max_len == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_r8169_rx_lock);
    u32 cur = g_rx_cur;

    if (g_rx_ring[cur].opts1 & RTL8169_DESC_OWN) {
        spinlock_unlock_irqrestore(&g_r8169_rx_lock, flags);
        return -(s64)EAGAIN;
    }

    u32 opts = g_rx_ring[cur].opts1;
    u32 len = opts & 0x3FFF;
    if (len > 4) len -= 4; /* strip CRC */
    if (len > RTL8169_PKT_BUF_SIZE) len = RTL8169_PKT_BUF_SIZE;

    size_t copy_len = (len < max_len) ? len : max_len;
    memcpy(buf, g_rx_buffers[cur], copy_len);

    /* Return descriptor to hardware ownership */
    u32 new_opts = RTL8169_DESC_OWN | RTL8169_PKT_BUF_SIZE;
    if (cur == RTL8169_NUM_RX_DESC - 1) {
        new_opts |= RTL8169_DESC_EOR;
    }
    g_rx_ring[cur].opts1 = new_opts;

    g_rx_cur = (cur + 1) % RTL8169_NUM_RX_DESC;
    spinlock_unlock_irqrestore(&g_r8169_rx_lock, flags);

    return (s64)copy_len;
}

void rtl8169_poll_rx(void)
{
    if (!g_r8169_ready) return;

    u8 local_buf[RTL8169_PKT_BUF_SIZE];
    while (true) {
        s64 r = rtl8169_recv_packet(local_buf, sizeof(local_buf));
        if (r <= 0) break;

        extern void net_process_incoming(const u8 *pkt, size_t len);
        net_process_incoming(local_buf, (size_t)r);
    }
}

static void rtl8169_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    if (!g_r8169_ready) return;

    u16 status = inw(g_r8169_io_base + REG_ISR);
    if (!status) return;

    /* Acknowledge interrupt */
    outw(g_r8169_io_base + REG_ISR, status);

    if (status & (INT_ROK | INT_RER | INT_RXOVW)) {
        rtl8169_poll_rx();
    }
}

static s64 dev_rtl8169_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return rtl8169_recv_packet(buf, len);
}

static s64 dev_rtl8169_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return rtl8169_send_packet(buf, len);
}

static file_operations_t g_rtl8169_fops = {
    .read    = dev_rtl8169_read,
    .write   = dev_rtl8169_write,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

static int rtl8169_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    /* Check BAR0 or other BARs for I/O base */
    u16 io_base = 0;
    for (int b = 0; b < 6; b++) {
        if (info->bar[b] & 1) {
            io_base = (u16)(info->bar[b] & ~0x3);
            if (io_base != 0) break;
        }
    }

    if (!io_base) return -ENODEV;
    g_r8169_io_base = io_base;

    /* Enable PCI Bus Mastering and I/O */
    pci_enable_bus_mastering(dm->hal);

    /* 1. Reset chip */
    outb(io_base + REG_COMMAND, CMD_RST);
    for (int i = 0; i < 10000; i++) {
        if (!(inb(io_base + REG_COMMAND) & CMD_RST)) break;
        cpu_pause();
    }

    /* 2. Read MAC address */
    for (int i = 0; i < 6; i++) {
        g_r8169_mac[i] = inb(io_base + REG_IDR0 + i);
    }

    /* 3. Unlock configuration registers */
    outb(io_base + REG_9346CR, 0xC0);

    /* 4. Allocate RX and TX descriptor rings */
    g_rx_ring_phys = pmm_alloc_page();
    g_tx_ring_phys = pmm_alloc_page();
    if (!g_rx_ring_phys || !g_tx_ring_phys) return -ENOMEM;

    g_rx_ring = (rtl8169_desc_t *)PHYS_TO_VIRT(g_rx_ring_phys);
    g_tx_ring = (rtl8169_desc_t *)PHYS_TO_VIRT(g_tx_ring_phys);
    memset(g_rx_ring, 0, PAGE_SIZE);
    memset(g_tx_ring, 0, PAGE_SIZE);

    /* 5. Allocate buffers and populate RX ring */
    for (u32 i = 0; i < RTL8169_NUM_RX_DESC; i++) {
        g_rx_bufs_phys[i] = pmm_alloc_page();
        if (!g_rx_bufs_phys[i]) return -ENOMEM;
        g_rx_buffers[i] = (u8 *)PHYS_TO_VIRT(g_rx_bufs_phys[i]);

        u32 opts = RTL8169_DESC_OWN | RTL8169_PKT_BUF_SIZE;
        if (i == RTL8169_NUM_RX_DESC - 1) opts |= RTL8169_DESC_EOR;

        g_rx_ring[i].opts1 = opts;
        g_rx_ring[i].opts2 = 0;
        g_rx_ring[i].buf_addr = (u64)g_rx_bufs_phys[i];
    }

    /* 6. Populate TX ring */
    for (u32 i = 0; i < RTL8169_NUM_TX_DESC; i++) {
        g_tx_bufs_phys[i] = pmm_alloc_page();
        if (!g_tx_bufs_phys[i]) return -ENOMEM;
        g_tx_buffers[i] = (u8 *)PHYS_TO_VIRT(g_tx_bufs_phys[i]);

        u32 opts = 0;
        if (i == RTL8169_NUM_TX_DESC - 1) opts |= RTL8169_DESC_EOR;

        g_tx_ring[i].opts1 = opts;
        g_tx_ring[i].opts2 = 0;
        g_tx_ring[i].buf_addr = (u64)g_tx_bufs_phys[i];
    }

    /* 7. Write descriptor base addresses (64-bit) */
    outl(io_base + REG_RDSAR, (u32)g_rx_ring_phys);
    outl(io_base + REG_RDSAR + 4, (u32)(g_rx_ring_phys >> 32));
    outl(io_base + REG_TNPDS, (u32)g_tx_ring_phys);
    outl(io_base + REG_TNPDS + 4, (u32)(g_tx_ring_phys >> 32));

    /* 8. Configure Rx Max Size & Tx Max Size */
    outw(io_base + REG_RMS, 1536);
    outb(io_base + REG_MTPS, 0x3B);

    /* 9. Enable Rx Checksum offloading in C+ Command */
    outw(io_base + REG_CPCR, 0x0020);

    /* 10. Enable Rx & Tx engines */
    outb(io_base + REG_COMMAND, CMD_RE | CMD_TE);

    /* 11. Configure TCR and RCR */
    outl(io_base + REG_TCR, 0x03000700);  /* IFG standard + unlimited DMA burst */
    outl(io_base + REG_RCR, 0x0000E70F);  /* Accept broadcast, multicast, unicast */

    /* 12. Lock configuration registers */
    outb(io_base + REG_9346CR, 0x00);

    /* 13. Enable Interrupts */
    g_r8169_irq = info->interrupt_line;
    outw(io_base + REG_ISR, 0xFFFF);
    if (g_r8169_irq > 0) {
        idt_register_irq(g_r8169_irq + 32, rtl8169_irq_handler, NULL);
        hal_irq_enable(g_r8169_irq, g_r8169_irq + 32);
    }
    outw(io_base + REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RXOVW);

    g_rx_cur = 0;
    g_tx_cur = 0;
    g_r8169_ready = true;

    /* Register with network subsystem */
    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strcpy(ndev.name, "rtl8169");
    memcpy(ndev.mac, g_r8169_mac, 6);
    ndev.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
    ndev.mtu = 1500;
    ndev.send = rtl8169_send_packet;
    ndev.recv = rtl8169_recv_packet;
    net_register_device(&ndev);

    devfs_register_device("net1", &g_rtl8169_fops, NULL);
    dm_set_drvdata(dm, &g_r8169_ready);

    pr_debug("[RTL8169] Gigabit Ethernet online at I/O 0x%04X, IRQ %u, MAC: %02x:%02x:%02x:%02x:%02x:%02x (/dev/net1)\n",
             g_r8169_io_base, g_r8169_irq,
             g_r8169_mac[0], g_r8169_mac[1], g_r8169_mac[2],
             g_r8169_mac[3], g_r8169_mac[4], g_r8169_mac[5]);

    return 0;
}

static const pci_device_id_t rtl8169_pci_ids[] = {
    { PCI_DEVICE(0x10EC, 0x8169) },   /* RTL-8169/8110 Gigabit */
    { PCI_DEVICE(0x10EC, 0x8168) },   /* RTL-8168/8111 PCIe   */
    { PCI_DEVICE(0x10EC, 0x8167) },   /* RTL-8169SC           */
    { PCI_DEVICE(0x10EC, 0x8136) },   /* RTL-8101E            */
    { 0 }
};

static pci_driver_t rtl8169_pci_driver = {
    .drv      = { .name = "rtl8169" },
    .id_table = rtl8169_pci_ids,
    .probe    = rtl8169_probe,
    .remove   = NULL,
};

void rtl8169_init(void)
{
    pci_driver_register(&rtl8169_pci_driver);
}
