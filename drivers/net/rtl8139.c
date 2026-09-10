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
#include "../../arch/x86_64/cpu/idt.h"
#include "../../hal/irq.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

/* RCR is programmed for an 8 KiB ring with WRAP set, so the chip may run up to
 * one maximum-size frame past the 8 KiB mark instead of splitting it — the
 * ring allocation has to cover that overshoot, and the ring index still wraps
 * modulo 8 KiB. */
#define RTL8139_RING_SIZE   8192
#define RTL8139_RX_BUF_SIZE (RTL8139_RING_SIZE + 16 + 1500)
#define RTL8139_TX_BUF_SIZE 2048

/* Longest frame the receiver will hand us, CRC included. */
#define RTL8139_MAX_FRAME   1536

/* Bits of the per-packet RX status word that precedes each frame. */
#define RTL_RXS_ROK         (1 << 0)
#define RTL_RXS_FAE         (1 << 1)
#define RTL_RXS_CRC         (1 << 2)
#define RTL_RXS_LONG        (1 << 3)
#define RTL_RXS_RUNT        (1 << 4)
#define RTL_RXS_ISE         (1 << 5)
#define RTL_RXS_ERR_MASK \
    (RTL_RXS_FAE | RTL_RXS_CRC | RTL_RXS_LONG | RTL_RXS_RUNT | RTL_RXS_ISE)

/* Interrupt status / mask bits. */
#define RTL_INT_ROK         (1 << 0)
#define RTL_INT_RER         (1 << 1)
#define RTL_INT_TOK         (1 << 2)
#define RTL_INT_TER         (1 << 3)
#define RTL_INT_RXOVW       (1 << 4)

/* Command register bits. */
#define RTL_CR_BUFE         (1 << 0)
#define RTL_CR_TE           (1 << 2)
#define RTL_CR_RE           (1 << 3)
#define RTL_CR_RST          (1 << 4)

#define RTL_RCR_CONFIG      0x0000008FU  /* WRAP | AB | AM | APM | AAP */

static u16        g_rtl_io_base = 0;
static u8         g_rtl_mac[6] = {0};
static u8        *g_rtl_rx_buf = NULL;
static phys_addr_t g_rtl_rx_phys = 0;
static u32        g_rtl_rx_offset = 0;
static u8        *g_rtl_tx_bufs[4] = {NULL};
static phys_addr_t g_rtl_tx_phys[4] = {0};
static u8         g_rtl_tx_cur = 0;
static u8         g_rtl_irq = 0;
static spinlock_t g_rtl_lock = SPINLOCK_INIT;
static bool       g_rtl_ready = false;
static bool       g_rtl_rx_draining = false;

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

/*
 * Stop the receiver, point it back at the start of the ring and restart it.
 *
 * The RTL8139 ring has no way to resynchronise once the read index stops
 * lining up with a packet header: every subsequent header is read out of the
 * middle of a frame, so a single corrupt length would have kept producing
 * garbage — and out-of-range reads — for the life of the boot.  A reset costs
 * whatever is still queued, which is the right trade for regaining sync.
 */
static void rtl8139_rx_reset(void)
{
    outb(g_rtl_io_base + REG_CR, RTL_CR_TE);   /* receiver off, TX untouched */
    outl(g_rtl_io_base + REG_RBSTART, (u32)g_rtl_rx_phys);
    outl(g_rtl_io_base + REG_RCR, RTL_RCR_CONFIG);
    g_rtl_rx_offset = 0;
    outw(g_rtl_io_base + REG_CAPR, (u16)(0 - 16));
    outb(g_rtl_io_base + REG_CR, RTL_CR_TE | RTL_CR_RE);
}

/*
 * Pop one frame off the RX ring into `dst`.  Returns the frame length with the
 * trailing CRC removed, 0 for a frame that was consumed but not worth keeping
 * (the chip flagged it bad), -EAGAIN when the ring is empty or the chip is
 * still writing, and -EIO when the ring had to be reset.  A caller draining the
 * ring keeps going on 0 and stops on any negative.  Caller holds g_rtl_lock.
 *
 * Everything read here — the status word, the length, the implied position of
 * the next header — is written by the device into memory we handed it, so all
 * of it is checked before it is used as a bound.  The previous version trusted
 * the length outright: it advanced the read index by it without ever comparing
 * it against the ring, and copied from `offset + 4` for as many bytes as the
 * caller's buffer would hold.
 */
static s64 rtl8139_ring_pop(void *dst, size_t max_len)
{
    if (inb(g_rtl_io_base + REG_CR) & RTL_CR_BUFE) return -(s64)EAGAIN;

    u32 offset = g_rtl_rx_offset;
    if (offset + 4 > RTL8139_RX_BUF_SIZE) {
        rtl8139_rx_reset();
        return -(s64)EIO;
    }

    u16 status, len;
    memcpy(&status, g_rtl_rx_buf + offset, sizeof(status));
    memcpy(&len, g_rtl_rx_buf + offset + 2, sizeof(len));

    /* 0xFFF0 is the chip's "still DMAing this frame" marker. */
    if (len == 0xFFF0) return -(s64)EAGAIN;

    if (len < 4 + sizeof(eth_hdr_t) || len > RTL8139_MAX_FRAME ||
        (u32)offset + 4 + len > RTL8139_RX_BUF_SIZE) {
        pr_debug("[RTL8139] Bogus RX header (status=0x%04x len=%u off=%u) — resetting ring\n",
                 status, len, offset);
        rtl8139_rx_reset();
        return -(s64)EIO;
    }

    size_t pkt_len = (size_t)len - 4;          /* drop the trailing CRC */
    size_t copy_len = 0;

    if ((status & RTL_RXS_ROK) && !(status & RTL_RXS_ERR_MASK)) {
        copy_len = (pkt_len < max_len) ? pkt_len : max_len;
        memcpy(dst, g_rtl_rx_buf + offset + 4, copy_len);
    }

    /* Headers are 4-byte aligned; the index wraps at the ring size, not at
     * the allocation size, because WRAP lets the chip overshoot. */
    offset = (offset + len + 4 + 3) & ~3U;
    offset %= RTL8139_RING_SIZE;
    g_rtl_rx_offset = offset;
    outw(g_rtl_io_base + REG_CAPR, (u16)(offset - 16));

    return (s64)copy_len;
}

s64 rtl8139_recv_packet(void *buf, size_t max_len)
{
    if (!g_rtl_ready || !buf || max_len == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_rtl_lock);
    s64 ret = rtl8139_ring_pop(buf, max_len);
    spinlock_unlock_irqrestore(&g_rtl_lock, flags);

    /* A frame the chip flagged bad was consumed but yields no data. */
    if (ret == 0) return -(s64)EAGAIN;
    return ret;
}

/*
 * Drain the ring into the IP stack.
 *
 * Without this the driver could transmit but nothing it received ever reached
 * ipv4_input(): the ring was only ever read through read(/dev/net0), so a
 * machine whose only NIC was an RTL8139 could not answer an ARP request, let
 * alone complete DHCP.
 */
void rtl8139_poll_rx(void)
{
    if (!g_rtl_ready) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_rtl_lock);
    if (g_rtl_rx_draining) {       /* see the e1000 driver for the reasoning */
        spinlock_unlock_irqrestore(&g_rtl_lock, flags);
        return;
    }
    g_rtl_rx_draining = true;

    u8 frame[RTL8139_MAX_FRAME];
    for (;;) {
        s64 n = rtl8139_ring_pop(frame, sizeof(frame));
        if (n < 0) break;      /* ring empty, or reset after a bad header */
        if (n == 0) continue;  /* bad frame consumed — keep draining */

        spinlock_unlock_irqrestore(&g_rtl_lock, flags);
        extern void net_process_incoming(const u8 *pkt, size_t len);
        net_process_incoming(frame, (size_t)n);
        flags = spinlock_lock_irqsave(&g_rtl_lock);
    }

    g_rtl_rx_draining = false;
    spinlock_unlock_irqrestore(&g_rtl_lock, flags);
}

static void rtl8139_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;

    u16 isr = inw(g_rtl_io_base + REG_ISR);
    if (!isr) return;

    /* The ISR is write-1-to-clear and must be acknowledged before the ring is
     * drained, or a frame arriving during the drain leaves no pending bit. */
    outw(g_rtl_io_base + REG_ISR, isr);

    if (isr & (RTL_INT_ROK | RTL_INT_RER | RTL_INT_RXOVW)) {
        rtl8139_poll_rx();
    }
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

    outl(io_base + REG_RCR, RTL_RCR_CONFIG);
    outw(io_base + REG_CAPR, (u16)(0 - 16));
    outb(io_base + REG_CR, RTL_CR_TE | RTL_CR_RE);

    g_rtl_rx_offset = 0;
    g_rtl_tx_cur = 0;
    g_rtl_ready = true;

    /* Acknowledge anything latched during reset, then take receive interrupts.
     * Until now the mask register was left at its post-reset zero, so the chip
     * never raised one and the ring was only drained if somebody happened to
     * read /dev/net0. */
    g_rtl_irq = info->interrupt_line;
    outw(io_base + REG_ISR, 0xFFFF);
    if (g_rtl_irq > 0) {
        idt_register_irq(g_rtl_irq + 32, rtl8139_irq_handler, NULL);
        hal_irq_enable(g_rtl_irq, g_rtl_irq + 32);
    }
    outw(io_base + REG_IMR, RTL_INT_ROK | RTL_INT_RER | RTL_INT_RXOVW |
                            RTL_INT_TOK | RTL_INT_TER);

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strcpy(ndev.name, "rtl8139");
    memcpy(ndev.mac, g_rtl_mac, 6);
    ndev.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
    ndev.mtu = 1500;
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
