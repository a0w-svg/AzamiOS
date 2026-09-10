/* ============================================================================
 * AzamiOS — ne2k-pci: NE2000-compatible PCI Ethernet (RTL8029AS)
 * File: drivers/net/ne2k_pci.c
 *
 * The DP8390 "NE2000" core, as found on the RTL8029AS and emulated by QEMU's
 * `-device ne2k_pci`.  Everything goes through a 32-byte I/O window: a paged
 * register file, and a remote-DMA port through which the host reads and
 * writes the card's private 16 KiB of packet memory.
 *
 *   0x4000-0x45FF   transmit buffer   (6 pages, one max-size frame)
 *   0x4600-0x7FFF   receive ring      (pages 0x46..0x7F)
 *
 * The receive ring is written by the card and read by the driver; BNRY trails
 * CURR by one page and marks how far the driver has consumed.  Frames are
 * prefixed on-card by a four-byte header giving status, the next frame's page
 * and the byte count, and a frame that runs off the end of the ring has to be
 * fetched in two remote-DMA bursts because the DMA engine does not wrap.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "../base/pci_bus.h"
#include "../../hal/irq.h"
#include "../../include/azami/net.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/cpu/idt.h"


/* ── Register offsets ────────────────────────────────────────────────────── */
#define NE_CR          0x00   /* Command, all pages                */
#define NE_P0_PSTART   0x01
#define NE_P0_PSTOP    0x02
#define NE_P0_BNRY     0x03
#define NE_P0_TPSR     0x04
#define NE_P0_TBCR0    0x05
#define NE_P0_TBCR1    0x06
#define NE_P0_ISR      0x07
#define NE_P0_RSAR0    0x08
#define NE_P0_RSAR1    0x09
#define NE_P0_RBCR0    0x0A
#define NE_P0_RBCR1    0x0B
#define NE_P0_RCR      0x0C
#define NE_P0_TCR      0x0D
#define NE_P0_DCR      0x0E
#define NE_P0_IMR      0x0F
#define NE_P0_TSR      0x04   /* read side of TPSR                 */
#define NE_P1_PAR0     0x01
#define NE_P1_CURR     0x07
#define NE_P1_MAR0     0x08
#define NE_DATA        0x10   /* remote DMA data window            */
#define NE_RESET       0x1F

/* ── Command register bits ───────────────────────────────────────────────── */
#define NE_CR_STP      0x01
#define NE_CR_STA      0x02
#define NE_CR_TXP      0x04
#define NE_CR_RD_READ  0x08   /* remote DMA: read from card        */
#define NE_CR_RD_WRITE 0x10   /* remote DMA: write to card         */
#define NE_CR_RD_ABORT 0x20   /* abort/complete remote DMA         */
#define NE_CR_PAGE1    0x40

/* ── Interrupt status bits ───────────────────────────────────────────────── */
#define NE_ISR_PRX     0x01   /* packet received                   */
#define NE_ISR_PTX     0x02   /* packet transmitted                */
#define NE_ISR_RXE     0x04
#define NE_ISR_TXE     0x08
#define NE_ISR_OVW     0x10   /* receive ring overwrite            */
#define NE_ISR_CNT     0x20
#define NE_ISR_RDC     0x40   /* remote DMA complete               */
#define NE_ISR_RST     0x80

/* ── Card memory map ─────────────────────────────────────────────────────── */
#define NE_TX_PAGE     0x40
#define NE_RX_START    0x46
#define NE_RX_STOP     0x80
#define NE_PAGE_SIZE   256
#define NE_FRAME_MAX   1518
#define NE_FRAME_MIN   60     /* Ethernet minimum, excluding FCS    */

typedef struct ne2k_device {
    u16        io;
    u8         mac[6];
    u8         irq;
    u8         next_page;     /* first ring page not yet consumed  */
    bool       ready;
    spinlock_t lock;
} ne2k_device_t;

static ne2k_device_t g_ne2k;

/* ── Low-level helpers ───────────────────────────────────────────────────── */

static inline void ne_out(u8 reg, u8 val) { outb((u16)(g_ne2k.io + reg), val); }
static inline u8   ne_in(u8 reg)          { return inb((u16)(g_ne2k.io + reg)); }

/* Wait for a remote DMA burst to retire, bounded so a wedged card cannot hang. */
static void ne_wait_dma(void)
{
    for (u32 i = 0; i < 100000; i++) {
        if (ne_in(NE_P0_ISR) & NE_ISR_RDC) break;
        cpu_pause();
    }
    ne_out(NE_P0_ISR, NE_ISR_RDC);
}

/* Read @len bytes of card memory at @addr through the remote DMA window. */
static void ne_dma_read(u16 addr, void *dst, u16 len)
{
    ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT);
    ne_out(NE_P0_ISR, NE_ISR_RDC);
    ne_out(NE_P0_RBCR0, (u8)(len & 0xFF));
    ne_out(NE_P0_RBCR1, (u8)(len >> 8));
    ne_out(NE_P0_RSAR0, (u8)(addr & 0xFF));
    ne_out(NE_P0_RSAR1, (u8)(addr >> 8));
    ne_out(NE_CR, NE_CR_STA | NE_CR_RD_READ);

    u8  *out   = (u8 *)dst;
    u16  words = (u16)((len + 1) / 2);
    for (u16 i = 0; i < words; i++) {
        u16 w = inw((u16)(g_ne2k.io + NE_DATA));
        out[i * 2] = (u8)(w & 0xFF);
        /* An odd length leaves the high byte of the last word unused. */
        if (i * 2 + 1 < len) out[i * 2 + 1] = (u8)(w >> 8);
    }
    ne_wait_dma();
}

static void ne_dma_write(u16 addr, const void *src, u16 len)
{
    ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT);
    ne_out(NE_P0_ISR, NE_ISR_RDC);
    ne_out(NE_P0_RBCR0, (u8)(len & 0xFF));
    ne_out(NE_P0_RBCR1, (u8)(len >> 8));
    ne_out(NE_P0_RSAR0, (u8)(addr & 0xFF));
    ne_out(NE_P0_RSAR1, (u8)(addr >> 8));
    ne_out(NE_CR, NE_CR_STA | NE_CR_RD_WRITE);

    const u8 *in    = (const u8 *)src;
    u16       words = (u16)((len + 1) / 2);
    for (u16 i = 0; i < words; i++) {
        u16 lo = in[i * 2];
        u16 hi = (i * 2 + 1 < len) ? in[i * 2 + 1] : 0;
        outw((u16)(g_ne2k.io + NE_DATA), (u16)(lo | (hi << 8)));
    }
    ne_wait_dma();
}

/* ── Receive ─────────────────────────────────────────────────────────────── */

struct ne_rx_hdr {
    u8  status;
    u8  next_page;
    u16 count;      /* header + payload, in bytes */
} __packed;

void ne2k_poll_rx(void)
{
    if (!g_ne2k.ready) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_ne2k.lock);

    for (u32 guard = 0; guard < 64; guard++) {
        ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT | NE_CR_PAGE1);
        u8 curr = ne_in(NE_P1_CURR);
        ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT);

        if (curr == g_ne2k.next_page) break;               /* ring empty */
        if (curr < NE_RX_START || curr >= NE_RX_STOP) break; /* card confused */

        struct ne_rx_hdr hdr;
        ne_dma_read((u16)(g_ne2k.next_page << 8), &hdr, sizeof(hdr));

        u16 total = hdr.count;
        if (total < sizeof(hdr) + NE_FRAME_MIN || total > sizeof(hdr) + NE_FRAME_MAX ||
            hdr.next_page < NE_RX_START || hdr.next_page >= NE_RX_STOP) {
            /* A corrupt header means the ring is no longer trustworthy;
             * resynchronise on CURR rather than chasing bad pages. */
            g_ne2k.next_page = curr;
            break;
        }

        u16 len = (u16)(total - sizeof(hdr));
        u8  frame[NE_FRAME_MAX];
        u16 start = (u16)((g_ne2k.next_page << 8) + sizeof(hdr));

        /* The DMA engine does not wrap at PSTOP, so a frame straddling the
         * end of the ring is fetched as two bursts. */
        u16 ring_end = (u16)(NE_RX_STOP << 8);
        if (start + len > ring_end) {
            u16 first = (u16)(ring_end - start);
            ne_dma_read(start, frame, first);
            ne_dma_read((u16)(NE_RX_START << 8), frame + first, (u16)(len - first));
        } else {
            ne_dma_read(start, frame, len);
        }

        /* Release the pages before handing the frame up: BNRY trails the
         * next unread page by one. */
        g_ne2k.next_page = hdr.next_page;
        u8 bnry = (u8)(hdr.next_page - 1);
        if (bnry < NE_RX_START) bnry = NE_RX_STOP - 1;
        ne_out(NE_P0_BNRY, bnry);

        if (hdr.status & 0x01) {   /* receive status: packet OK */
            spinlock_unlock_irqrestore(&g_ne2k.lock, flags);
            net_process_incoming(frame, len);
            flags = spinlock_lock_irqsave(&g_ne2k.lock);
        }
    }

    spinlock_unlock_irqrestore(&g_ne2k.lock, flags);
}

static void ne2k_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    if (!g_ne2k.ready) return;

    u8 isr = ne_in(NE_P0_ISR);
    if (!isr) return;                 /* shared line, not ours */
    ne_out(NE_P0_ISR, isr);           /* write-1-to-clear */

    if (isr & NE_ISR_OVW) {
        /* Overwrite warning: the ring filled up.  Restarting the receiver is
         * the documented recovery, and the frames already in the ring are
         * still drained below. */
        ne_out(NE_CR, NE_CR_STP | NE_CR_RD_ABORT);
        ne_out(NE_P0_RBCR0, 0);
        ne_out(NE_P0_RBCR1, 0);
        ne_out(NE_P0_TCR, 0x02);
        ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT);
        ne_out(NE_P0_TCR, 0x00);
    }
    if (isr & (NE_ISR_PRX | NE_ISR_OVW | NE_ISR_RXE)) {
        ne2k_poll_rx();
    }
}

/* ── Transmit ────────────────────────────────────────────────────────────── */

static s64 ne2k_send_packet(const void *data, size_t len)
{
    if (!g_ne2k.ready || !data) return -(s64)ENODEV;
    if (len > NE_FRAME_MAX) return -(s64)EMSGSIZE;

    /* Short frames are padded to the Ethernet minimum; the card transmits
     * exactly the byte count it is given. */
    u8 padded[NE_FRAME_MIN];
    if (len < NE_FRAME_MIN) {
        memset(padded, 0, sizeof(padded));
        memcpy(padded, data, len);
        data = padded;
        len  = NE_FRAME_MIN;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_ne2k.lock);

    ne_dma_write((u16)(NE_TX_PAGE << 8), data, (u16)len);
    ne_out(NE_P0_TPSR, NE_TX_PAGE);
    ne_out(NE_P0_TBCR0, (u8)(len & 0xFF));
    ne_out(NE_P0_TBCR1, (u8)(len >> 8));
    ne_out(NE_CR, NE_CR_STA | NE_CR_TXP | NE_CR_RD_ABORT);

    /* Wait for the transmitter to release, so back-to-back sends cannot
     * overwrite a frame still on the wire. */
    for (u32 i = 0; i < 100000; i++) {
        if (!(ne_in(NE_CR) & NE_CR_TXP)) break;
        cpu_pause();
    }

    spinlock_unlock_irqrestore(&g_ne2k.lock, flags);
    return (s64)len;
}

static s64 ne2k_recv_packet(void *buf, size_t max_len)
{
    (void)buf; (void)max_len;
    /* Frames are pushed straight into the stack from the drain path. */
    ne2k_poll_rx();
    return 0;
}

static bool ne2k_link_up(void) { return g_ne2k.ready; }

/* ── Bring-up ────────────────────────────────────────────────────────────── */

static int ne2k_reset(void)
{
    outb((u16)(g_ne2k.io + NE_RESET), inb((u16)(g_ne2k.io + NE_RESET)));
    for (u32 i = 0; i < 100000; i++) {
        if (ne_in(NE_P0_ISR) & NE_ISR_RST) return 0;
        cpu_pause();
    }
    return -EIO;
}

static void ne2k_read_prom(void)
{
    /* In 16-bit mode each PROM byte is delivered twice, so the MAC occupies
     * the even offsets of the first 12 bytes. */
    u8 prom[32];
    ne_dma_read(0x0000, prom, sizeof(prom));
    for (int i = 0; i < 6; i++) g_ne2k.mac[i] = prom[i * 2];
}

static int ne2k_init_hw(void)
{
    if (ne2k_reset() != 0) return -EIO;
    ne_out(NE_P0_ISR, 0xFF);

    /* Stop the card and put it in 16-bit DMA mode before touching memory. */
    ne_out(NE_CR, NE_CR_STP | NE_CR_RD_ABORT);
    ne_out(NE_P0_DCR, 0x49);          /* word transfers, normal loopback   */
    ne_out(NE_P0_RBCR0, 0);
    ne_out(NE_P0_RBCR1, 0);
    ne_out(NE_P0_RCR, 0x20);          /* monitor: drop everything for now  */
    ne_out(NE_P0_TCR, 0x02);          /* internal loopback during setup    */

    ne2k_read_prom();

    ne_out(NE_P0_TPSR, NE_TX_PAGE);
    ne_out(NE_P0_PSTART, NE_RX_START);
    ne_out(NE_P0_BNRY, NE_RX_START);
    ne_out(NE_P0_PSTOP, NE_RX_STOP);
    ne_out(NE_P0_ISR, 0xFF);
    ne_out(NE_P0_IMR, NE_ISR_PRX | NE_ISR_PTX | NE_ISR_RXE | NE_ISR_TXE | NE_ISR_OVW);

    /* Page 1: station address, accept-all multicast filter, ring head. */
    ne_out(NE_CR, NE_CR_STP | NE_CR_RD_ABORT | NE_CR_PAGE1);
    for (int i = 0; i < 6; i++) ne_out((u8)(NE_P1_PAR0 + i), g_ne2k.mac[i]);
    for (int i = 0; i < 8; i++) ne_out((u8)(NE_P1_MAR0 + i), 0xFF);
    ne_out(NE_P1_CURR, NE_RX_START + 1);
    ne_out(NE_CR, NE_CR_STP | NE_CR_RD_ABORT);

    g_ne2k.next_page = NE_RX_START + 1;

    /* Go live: normal transmit, accept broadcast and multicast. */
    ne_out(NE_CR, NE_CR_STA | NE_CR_RD_ABORT);
    ne_out(NE_P0_TCR, 0x00);
    ne_out(NE_P0_RCR, 0x0C);
    return 0;
}

static int ne2k_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_ne2k.ready) return -EBUSY;   /* one instance drives the stack */

    pci_device_info_t *info = to_pci_info(dm);
    u16 io = (u16)pci_get_bar(dm->hal, 0);
    if (!info || io == 0) return -ENODEV;

    memset(&g_ne2k, 0, sizeof(g_ne2k));
    spinlock_init(&g_ne2k.lock);
    g_ne2k.io  = io;
    g_ne2k.irq = info->interrupt_line;

    pci_enable_bus_mastering(dm->hal);

    if (ne2k_init_hw() != 0) {
        pr_debug("[NE2K] card did not come out of reset at I/O 0x%04x\n", io);
        return -EIO;
    }
    g_ne2k.ready = true;

    if (g_ne2k.irq > 0 && g_ne2k.irq < 16) {
        idt_register_irq((u8)(g_ne2k.irq + 32), ne2k_irq_handler, NULL);
        hal_irq_enable(g_ne2k.irq, (u8)(g_ne2k.irq + 32));
    }

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strncpy(ndev.name, "eth1", sizeof(ndev.name) - 1);
    memcpy(ndev.mac, g_ne2k.mac, 6);
    ndev.mtu      = 1500;
    ndev.send     = ne2k_send_packet;
    ndev.recv     = ne2k_recv_packet;
    ndev.link_up  = ne2k_link_up;
    net_register_device(&ndev);

    dm_set_drvdata(dm, &g_ne2k);
    pr_debug("[NE2K] NE2000-compatible NIC at I/O 0x%04x IRQ %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
             io, g_ne2k.irq,
             g_ne2k.mac[0], g_ne2k.mac[1], g_ne2k.mac[2],
             g_ne2k.mac[3], g_ne2k.mac[4], g_ne2k.mac[5]);
    return 0;
}

static void ne2k_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_ne2k.ready) return;
    ne_out(NE_P0_IMR, 0);
    ne_out(NE_CR, NE_CR_STP | NE_CR_RD_ABORT);
    if (g_ne2k.irq) hal_irq_disable(g_ne2k.irq);
    g_ne2k.ready = false;
}

static const pci_device_id_t ne2k_pci_ids[] = {
    { PCI_DEVICE(0x10EC, 0x8029) },   /* Realtek RTL8029AS      */
    { PCI_DEVICE(0x1050, 0x0940) },   /* Winbond W89C940        */
    { PCI_DEVICE(0x1106, 0x0926) },   /* VIA VT86C926 Amazon    */
    { 0 }
};

static pci_driver_t ne2k_pci_driver = {
    .drv      = { .name = "ne2k-pci" },
    .id_table = ne2k_pci_ids,
    .probe    = ne2k_probe,
    .remove   = ne2k_remove,
};

void ne2k_pci_init(void)
{
    pci_driver_register(&ne2k_pci_driver);
}
