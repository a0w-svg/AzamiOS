/* ============================================================================
 * AzamiOS — AMD PCnet-FAST III (Am79C973) Ethernet Driver Implementation
 * File: drivers/net/pcnet.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "pcnet.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../../hal/irq.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

extern void net_process_incoming(const u8 *pkt, size_t len);

#define PCNET_NUM_RX_DESC 8
#define PCNET_NUM_TX_DESC 8
#define PCNET_BUF_SIZE    1536

/* Ring descriptor structure */
typedef struct __attribute__((packed)) {
    u32 rmd0; /* Lower 32-bit physical address */
    u32 rmd1; /* Status and buffer size */
    u32 rmd2; /* Message length & reserved */
    u32 rmd3; /* User data */
} pcnet_desc_t;

/* Initialization Block */
typedef struct __attribute__((packed)) {
    u16 mode;
    u8  rlen;      /* Encoded RX ring length */
    u8  tlen;      /* Encoded TX ring length */
    u8  mac[6];
    u16 reserved;
    u64 ladr;     /* Logical address filter */
    u32 rx_ring;  /* Physical address of RX ring */
    u32 tx_ring;  /* Physical address of TX ring */
} pcnet_init_block_t;

static u16 g_pcnet_io_base = 0;
static u8  g_pcnet_mac[6] = {0};
static pcnet_init_block_t *g_init_block = NULL;
static pcnet_desc_t *g_rx_ring = NULL;
static pcnet_desc_t *g_tx_ring = NULL;
static u8 *g_rx_buffers[PCNET_NUM_RX_DESC];
static u8 *g_tx_buffers[PCNET_NUM_TX_DESC];
static u32 g_rx_cur = 0;
static u32 g_tx_cur = 0;
static spinlock_t g_pcnet_lock = SPINLOCK_INIT;
static bool g_pcnet_ready = false;
static u8  g_pcnet_irq = 0;

static void pcnet_irq_handler(pt_regs_t *r, void *ctx);

/* Register Offsets (32-bit DWIO) */
#define PCNET_RDP   0x10
#define PCNET_RAP   0x14
#define PCNET_RESET 0x18
#define PCNET_BDP   0x1C

static inline void pcnet_write_csr(u32 csr, u32 val)
{
    outl(g_pcnet_io_base + PCNET_RAP, csr);
    outl(g_pcnet_io_base + PCNET_RDP, val);
}

static inline u32 pcnet_read_csr(u32 csr)
{
    outl(g_pcnet_io_base + PCNET_RAP, csr);
    return inl(g_pcnet_io_base + PCNET_RDP);
}

static inline void pcnet_write_bcr(u32 bcr, u32 val)
{
    outl(g_pcnet_io_base + PCNET_RAP, bcr);
    outl(g_pcnet_io_base + PCNET_BDP, val);
}

void pcnet_get_mac(u8 mac[6])
{
    memcpy(mac, g_pcnet_mac, 6);
}

s64 pcnet_send_packet(const void *data, size_t len)
{
    if (!g_pcnet_ready || !data || len == 0 || len > PCNET_BUF_SIZE) {
        return -(s64)EINVAL;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_pcnet_lock);
    u32 idx = g_tx_cur;

    /* Check if descriptor is owned by host (bit 31 clear) */
    if (g_tx_ring[idx].rmd1 & 0x80000000) {
        spinlock_unlock_irqrestore(&g_pcnet_lock, flags);
        return -(s64)EBUSY;
    }

    memcpy(g_tx_buffers[idx], data, len);
    if (len < 60) len = 60; /* Ethernet minimum frame length */

    /* Setup buffer size (2's complement in bits 11:0) & STP(25)|ENP(24)|OWN(31) */
    u16 neg_len = (u16)(-len);
    g_tx_ring[idx].rmd1 = 0x8300F000 | (neg_len & 0x0FFF);
    g_tx_ring[idx].rmd2 = 0;

    /* Trigger transmit demand (CSR0 bit 3: TDMD) */
    pcnet_write_csr(0, pcnet_read_csr(0) | 0x0008);

    g_tx_cur = (idx + 1) % PCNET_NUM_TX_DESC;
    spinlock_unlock_irqrestore(&g_pcnet_lock, flags);

    return (s64)len;
}

s64 pcnet_recv_packet(void *buf, size_t max_len)
{
    if (!g_pcnet_ready || !buf || max_len == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_pcnet_lock);
    u32 idx = g_rx_cur;

    /* If card still owns the buffer (bit 31 set), no packet ready */
    if (g_rx_ring[idx].rmd1 & 0x80000000) {
        spinlock_unlock_irqrestore(&g_pcnet_lock, flags);
        return 0;
    }

    u32 pkt_len = (g_rx_ring[idx].rmd2 & 0x0FFF) - 4; /* strip CRC */
    if (pkt_len > max_len) pkt_len = (u32)max_len;

    memcpy(buf, g_rx_buffers[idx], pkt_len);

    /* Give descriptor back to card (set OWN bit) */
    u16 neg_len = (u16)(-PCNET_BUF_SIZE);
    g_rx_ring[idx].rmd1 = 0x8000F000 | (neg_len & 0x0FFF);
    g_rx_ring[idx].rmd2 = 0;

    g_rx_cur = (idx + 1) % PCNET_NUM_RX_DESC;
    spinlock_unlock_irqrestore(&g_pcnet_lock, flags);

    return (s64)pkt_len;
}

/* Drain every completed RX descriptor into the kernel network stack. This is
 * what makes pcnet actually usable as a network interface: previously the
 * chip came up and could send/receive through /dev/net1 by hand, but nothing
 * ever fed a received frame to net_process_incoming(), so DHCP/IP never saw
 * a packet arrive on this NIC. */
static void pcnet_drain_rx(void)
{
    u8 local_buf[PCNET_BUF_SIZE];

    for (;;) {
        irqflags_t flags = spinlock_lock_irqsave(&g_pcnet_lock);
        u32 idx = g_rx_cur;

        if (g_rx_ring[idx].rmd1 & 0x80000000) {
            spinlock_unlock_irqrestore(&g_pcnet_lock, flags);
            return;   /* card still owns the next slot: nothing more queued */
        }

        u32 pkt_len = g_rx_ring[idx].rmd2 & 0x0FFF;
        pkt_len = pkt_len >= 4 ? pkt_len - 4 : 0;   /* strip the trailing CRC */
        if (pkt_len > sizeof(local_buf)) pkt_len = sizeof(local_buf);
        memcpy(local_buf, g_rx_buffers[idx], pkt_len);

        /* Give the descriptor back to the card (OWN bit) before dropping the
         * lock, so a concurrent /dev/net1 reader never races this slot. */
        u16 neg_len = (u16)(-PCNET_BUF_SIZE);
        g_rx_ring[idx].rmd1 = 0x8000F000 | (neg_len & 0x0FFF);
        g_rx_ring[idx].rmd2 = 0;
        g_rx_cur = (idx + 1) % PCNET_NUM_RX_DESC;
        spinlock_unlock_irqrestore(&g_pcnet_lock, flags);

        if (pkt_len > 0) net_process_incoming(local_buf, pkt_len);
    }
}

static void pcnet_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;

    u32 csr0 = pcnet_read_csr(0);
    if (!(csr0 & 0x8000)) return;   /* INTR (bit 15) not set: not ours */

    /* CSR0's status bits (RINT/TINT/IDON/...) are write-1-to-clear; writing
     * back exactly what was read acks every condition this read saw without
     * touching the control bits (STRT/INEA) that live in the same register. */
    pcnet_write_csr(0, csr0);

    if (csr0 & 0x0400) pcnet_drain_rx();   /* RINT: bit 10 */
}

/* File operations for /dev/net1 */
static s64 dev_pcnet_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return pcnet_recv_packet(buf, len);
}

static s64 dev_pcnet_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return pcnet_send_packet(buf, len);
}

static file_operations_t g_pcnet_fops = {
    .read = dev_pcnet_read,
    .write = dev_pcnet_write,
    .open = NULL,
    .release = NULL,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL
};

static int pcnet_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    /* This driver's state is a single set of global rings/buffers: refuse a
     * second device rather than silently reinitializing them underneath the
     * first NIC. */
    if (g_pcnet_ready) return -EBUSY;

    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;

    g_pcnet_io_base = (u16)(pci->bar[0] & ~0x3);
    if (!g_pcnet_io_base) return -ENODEV;

    pci_enable_bus_mastering(dm->hal);

    /* Reset PCnet chip */
    inl(g_pcnet_io_base + PCNET_RESET);
    outl(g_pcnet_io_base + PCNET_RESET, 0);

    /* Enable 32-bit software mode (DWIO) */
    outl(g_pcnet_io_base + PCNET_RDP, 0);

    /* Read MAC from PROM */
    for (int i = 0; i < 6; i++) {
        g_pcnet_mac[i] = inb(g_pcnet_io_base + i);
    }

    pr_debug("[PCNET] Found AMD PCnet-FAST III at I/O 0x%04x, MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_pcnet_io_base,
             g_pcnet_mac[0], g_pcnet_mac[1], g_pcnet_mac[2],
             g_pcnet_mac[3], g_pcnet_mac[4], g_pcnet_mac[5]);

    /* Stop chip for configuration (CSR0 bit 2: STOP) */
    pcnet_write_csr(0, 0x0004);

    /* Set BCR20 (Software Style) to 32-bit mode (Style 2 / Style 3) */
    pcnet_write_bcr(20, 0x0002);

    /* Allocate Initialization Block */
    g_init_block = (pcnet_init_block_t *)kzalloc(sizeof(pcnet_init_block_t));
    if (!g_init_block) return -ENOMEM;

    g_init_block->mode = 0; /* Promiscuous = 0, Normal operation */
    g_init_block->rlen = (3 << 4); /* 8 descriptors = 2^3 */
    g_init_block->tlen = (3 << 4); /* 8 descriptors = 2^3 */
    memcpy(g_init_block->mac, g_pcnet_mac, 6);

    /* Allocate Descriptor Rings */
    g_rx_ring = (pcnet_desc_t *)kzalloc(sizeof(pcnet_desc_t) * PCNET_NUM_RX_DESC);
    g_tx_ring = (pcnet_desc_t *)kzalloc(sizeof(pcnet_desc_t) * PCNET_NUM_TX_DESC);
    if (!g_rx_ring || !g_tx_ring) return -ENOMEM;

    memset(g_rx_ring, 0, sizeof(pcnet_desc_t) * PCNET_NUM_RX_DESC);
    memset(g_tx_ring, 0, sizeof(pcnet_desc_t) * PCNET_NUM_TX_DESC);

    for (int i = 0; i < PCNET_NUM_RX_DESC; i++) {
        g_rx_buffers[i] = (u8 *)kmalloc(PCNET_BUF_SIZE);
        phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_rx_buffers[i]);
        g_rx_ring[i].rmd0 = (u32)phys;
        u16 neg_len = (u16)(-PCNET_BUF_SIZE);
        g_rx_ring[i].rmd1 = 0x8000F000 | (neg_len & 0x0FFF); /* OWN bit set */
    }

    for (int i = 0; i < PCNET_NUM_TX_DESC; i++) {
        g_tx_buffers[i] = (u8 *)kmalloc(PCNET_BUF_SIZE);
        phys_addr_t phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_tx_buffers[i]);
        g_tx_ring[i].rmd0 = (u32)phys;
        g_tx_ring[i].rmd1 = 0x0000F000; /* Host owns buffer */
    }

    phys_addr_t rx_ring_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_rx_ring);
    phys_addr_t tx_ring_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_tx_ring);
    g_init_block->rx_ring = (u32)rx_ring_phys;
    g_init_block->tx_ring = (u32)tx_ring_phys;

    /* Write Init Block Address to CSR1 (low) & CSR2 (high) */
    phys_addr_t init_phys = vmm_translate(vmm_kernel_space(), (virt_addr_t)g_init_block);
    pcnet_write_csr(1, (u32)(init_phys & 0xFFFF));
    pcnet_write_csr(2, (u32)((init_phys >> 16) & 0xFFFF));

    /* Initialize controller (CSR0 bit 0: INIT) */
    pcnet_write_csr(0, 0x0001);

    /* Wait for initialization complete (IDON bit 8) */
    int timeout = 10000;
    u32 spins = 0;
    while (!(pcnet_read_csr(0) & 0x0100) && timeout-- > 0) {
        hw_spin_wait(spins++);
    }

    /* Start controller (CSR0 bit 1: STRT, bit 6: INEA enable interrupts) */
    pcnet_write_csr(0, 0x0042);

    g_pcnet_ready = true;
    devfs_register_device("net1", &g_pcnet_fops, NULL);

    /* Wire the chip into the kernel IP stack: an interrupt-driven RX drain
     * (see pcnet_irq_handler/pcnet_drain_rx above) feeding net_process_incoming(),
     * and a net_device_t so routing/DHCP/etc. know this interface exists.
     * Neither of these existed before — the hardware came up and could push
     * packets by hand through /dev/net1, but the stack itself never saw it. */
    g_pcnet_irq = pci->interrupt_line;
    if (g_pcnet_irq > 0 && g_pcnet_irq < 16) {
        idt_register_irq((u8)(g_pcnet_irq + 32), pcnet_irq_handler, NULL);
        hal_irq_enable(g_pcnet_irq, (u8)(g_pcnet_irq + 32));
    }

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strncpy(ndev.name, "eth-pcnet", sizeof(ndev.name) - 1);
    memcpy(ndev.mac, g_pcnet_mac, 6);
    ndev.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
    ndev.mtu   = 1500;
    ndev.send  = pcnet_send_packet;
    net_register_device(&ndev);

    pr_debug("[PCNET] AMD PCnet-FAST III online and registered as /dev/net1 and %s\n",
             ndev.name);

    return 0;
}

static void pcnet_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_pcnet_ready) return;
    if (g_pcnet_irq) hal_irq_disable(g_pcnet_irq);
    pcnet_write_csr(0, 0x0004);   /* STOP */
    g_pcnet_ready = false;
}

static const pci_device_id_t pcnet_pci_ids[] = {
    { PCI_DEVICE(PCNET_VENDOR_ID, PCNET_DEVICE_ID) },
    { 0 }
};

static pci_driver_t pcnet_pci_driver = {
    .drv      = { .name = "pcnet" },
    .id_table = pcnet_pci_ids,
    .probe    = pcnet_probe,
    .remove   = pcnet_remove,
};

void pcnet_init(void)
{
    pci_driver_register(&pcnet_pci_driver);
}
