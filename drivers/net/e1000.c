/* ============================================================================
 * AzamiOS — Intel 8254x / 82574L (e1000 / e1000e) Gigabit Ethernet Driver
 * File: drivers/net/e1000.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "e1000.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../../hal/irq.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/idt.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"
#include "../../fs/vfs.h"

static e1000_device_t g_e1000_dev;
static spinlock_t     g_e1000_lock = SPINLOCK_INIT;
static bool           g_e1000_ready = false;
static void e1000_irq_handler(pt_regs_t *r, void *ctx);

/* ── MMIO Helpers ────────────────────────────────────────────────────────── */

static inline void e1000_write32(u32 reg, u32 val)
{
    volatile u32 *addr = (volatile u32 *)(PHYS_TO_VIRT((phys_addr_t)g_e1000_dev.mmio_base) + reg);
    *addr = val;
}

static inline u32 e1000_read32(u32 reg)
{
    volatile u32 *addr = (volatile u32 *)(PHYS_TO_VIRT((phys_addr_t)g_e1000_dev.mmio_base) + reg);
    return *addr;
}

/* ── EEPROM Helpers ──────────────────────────────────────────────────────── */

static u16 e1000_eeprom_read(u8 addr)
{
    u32 temp = 0;
    if (g_e1000_dev.has_eeprom) {
        e1000_write32(E1000_EERD, 1 | ((u32)addr << 8));
        while (!((temp = e1000_read32(E1000_EERD)) & (1 << 4))) {
            cpu_pause();
        }
    } else {
        e1000_write32(E1000_EERD, 1 | ((u32)addr << 2));
        while (!((temp = e1000_read32(E1000_EERD)) & (1 << 1))) {
            cpu_pause();
        }
    }
    return (u16)((temp >> 16) & 0xFFFF);
}

static void e1000_read_mac(void)
{
    /* Check if EEPROM is present */
    e1000_write32(E1000_EERD, 0x1);
    for (int i = 0; i < 1000; i++) {
        u32 val = e1000_read32(E1000_EERD);
        if (val & 0x10) {
            g_e1000_dev.has_eeprom = true;
            break;
        }
    }

    u16 mac16 = e1000_eeprom_read(0);
    g_e1000_dev.mac[0] = mac16 & 0xFF;
    g_e1000_dev.mac[1] = (mac16 >> 8) & 0xFF;
    mac16 = e1000_eeprom_read(1);
    g_e1000_dev.mac[2] = mac16 & 0xFF;
    g_e1000_dev.mac[3] = (mac16 >> 8) & 0xFF;
    mac16 = e1000_eeprom_read(2);
    g_e1000_dev.mac[4] = mac16 & 0xFF;
    g_e1000_dev.mac[5] = (mac16 >> 8) & 0xFF;

    /* If EEPROM read returned all 0 or all FF, fallback to RAL/RAH registers */
    if ((g_e1000_dev.mac[0] == 0 && g_e1000_dev.mac[1] == 0 && g_e1000_dev.mac[2] == 0) ||
        (g_e1000_dev.mac[0] == 0xFF && g_e1000_dev.mac[1] == 0xFF)) {
        u32 ral = e1000_read32(E1000_RAL);
        u32 rah = e1000_read32(E1000_RAH);
        g_e1000_dev.mac[0] = ral & 0xFF;
        g_e1000_dev.mac[1] = (ral >> 8) & 0xFF;
        g_e1000_dev.mac[2] = (ral >> 16) & 0xFF;
        g_e1000_dev.mac[3] = (ral >> 24) & 0xFF;
        g_e1000_dev.mac[4] = rah & 0xFF;
        g_e1000_dev.mac[5] = (rah >> 8) & 0xFF;
    }
}

/* ── RX & TX Initialization ──────────────────────────────────────────────── */

static int e1000_init_rx(void)
{
    phys_addr_t rx_phys = pmm_alloc_page();
    if (!rx_phys) return -1;

    g_e1000_dev.rx_descs_phys = rx_phys;
    g_e1000_dev.rx_descs = (e1000_rx_desc_t *)PHYS_TO_VIRT(rx_phys);
    memset(g_e1000_dev.rx_descs, 0, PAGE_SIZE);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        phys_addr_t buf_phys = pmm_alloc_page();
        if (!buf_phys) return -1;
        g_e1000_dev.rx_buffers[i] = (u8 *)PHYS_TO_VIRT(buf_phys);
        g_e1000_dev.rx_descs[i].addr = (u64)buf_phys;
        g_e1000_dev.rx_descs[i].status = 0;
    }

    e1000_write32(E1000_RDBAL, (u32)(rx_phys & 0xFFFFFFFF));
    e1000_write32(E1000_RDBAH, (u32)(rx_phys >> 32));
    e1000_write32(E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write32(E1000_RDH, 0);
    e1000_write32(E1000_RDT, E1000_NUM_RX_DESC - 1);
    g_e1000_dev.rx_cur = 0;

    /* Enable receiver */
    e1000_write32(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048);
    return 0;
}

static int e1000_init_tx(void)
{
    phys_addr_t tx_phys = pmm_alloc_page();
    if (!tx_phys) return -1;

    g_e1000_dev.tx_descs_phys = tx_phys;
    g_e1000_dev.tx_descs = (e1000_tx_desc_t *)PHYS_TO_VIRT(tx_phys);
    memset(g_e1000_dev.tx_descs, 0, PAGE_SIZE);

    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        phys_addr_t buf_phys = pmm_alloc_page();
        if (!buf_phys) return -1;
        g_e1000_dev.tx_buffers[i] = (u8 *)PHYS_TO_VIRT(buf_phys);
        g_e1000_dev.tx_descs[i].addr = (u64)buf_phys;
        g_e1000_dev.tx_descs[i].status = E1000_TXD_STAT_DD; /* Ready for transmit */
    }

    e1000_write32(E1000_TDBAL, (u32)(tx_phys & 0xFFFFFFFF));
    e1000_write32(E1000_TDBAH, (u32)(tx_phys >> 32));
    e1000_write32(E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write32(E1000_TDH, 0);
    e1000_write32(E1000_TDT, 0);
    g_e1000_dev.tx_cur = 0;

    /* Configure IPG and enable transmitter */
    e1000_write32(E1000_TIPG, 0x0060200A);
    e1000_write32(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (15 << E1000_TCTL_CT_SHIFT) | (64 << E1000_TCTL_COLD_SHIFT));
    return 0;
}

/* ── DevFS Device Operations (/dev/net0) ─────────────────────────────────── */

static s64 devfs_net_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_e1000_ready) return -(s64)ENODEV;
    return e1000_recv_packet(buf, len);
}

static s64 devfs_net_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_e1000_ready) return -(s64)ENODEV;
    return e1000_send_packet(buf, len);
}

static s64 devfs_net_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_e1000_ready) return -(s64)ENODEV;
    return net_ioctl(cmd, arg);
}

static file_operations_t g_net_fops = {
    .read = devfs_net_read,
    .write = devfs_net_write,
    .ioctl = devfs_net_ioctl,
};

/* ── PCI Device Probing & Initialization ─────────────────────────────────── */

static bool is_e1000_device(u16 vendor, u16 device)
{
    if (vendor != 0x8086) return false;
    switch (device) {
        case 0x100E: /* 82540EM */
        case 0x1004: /* 82543GC */
        case 0x100F: /* 82545EM */
        case 0x10D3: /* 82574L */
        case 0x1079: /* 82546GB */
        case 0x107C: /* 82541PI */
        case 0x1019: /* 82547EI */
        case 0x101E: /* 82540EP */
        case 0x153A: /* I217-LM */
        case 0x1533: /* I210 */
            return true;
        default:
            return false;
    }
}

int e1000_init(void)
{
    device_t *pci_bus = device_find("PCI0");
    if (!pci_bus) return -1;

    device_t *pci_dev = NULL;
    pci_device_info_t *info = NULL;

    device_t *curr = pci_bus->children;
    while (curr) {
        if (curr->driver_data) {
            pci_device_info_t *p = (pci_device_info_t *)curr->driver_data;
            if (is_e1000_device(p->vendor_id, p->device_id)) {
                pci_dev = curr;
                info = p;
                break;
            }
        }
        curr = curr->sibling;
    }

    if (!pci_dev || !info) {
        pr_debug("[E1000] No supported Intel Gigabit Ethernet controller found.\n");
        return -1;
    }

    pr_debug("[E1000] Found Intel Ethernet Controller (PCI %02x:%02x.%u, %04x:%04x)\n",
             info->bus, info->slot, info->func, info->vendor_id, info->device_id);

    pci_enable_bus_mastering(pci_dev);

    /* Get MMIO BAR (BAR0) and map pages into kernel virtual address space */
    u32 bar0 = info->bar[0] & ~0xF;
    if (!bar0) {
        pr_debug("[E1000] Error: Invalid MMIO BAR0\n");
        return -1;
    }

    phys_addr_t bar0_aligned = ALIGN_DOWN(bar0, 4096);
    virt_addr_t bar0_virt = (virt_addr_t)PHYS_TO_VIRT(bar0_aligned);
    for (u32 off = 0; off < 0x20000; off += 4096) {
        vmm_map(0, bar0_virt + off, bar0_aligned + off, VMM_MMIO);
    }

    g_e1000_dev.mmio_base = bar0;
    g_e1000_dev.irq = info->interrupt_line;
    g_e1000_dev.has_eeprom = false;

    /* Reset device */
    e1000_write32(E1000_CTRL, e1000_read32(E1000_CTRL) | E1000_CTRL_RST);
    for (volatile int i = 0; i < 50000; i++) cpu_pause();

    /* Disable interrupts for now */
    e1000_write32(E1000_IMC, 0xFFFFFFFF);

    /* Read hardware MAC address */
    e1000_read_mac();
    pr_debug("[E1000] Hardware MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             g_e1000_dev.mac[0], g_e1000_dev.mac[1], g_e1000_dev.mac[2],
             g_e1000_dev.mac[3], g_e1000_dev.mac[4], g_e1000_dev.mac[5]);

    /* Set Link Up */
    e1000_write32(E1000_CTRL, e1000_read32(E1000_CTRL) | E1000_CTRL_SLU);

    /* Clear Multicast Table Array */
    for (int i = 0; i < 128; i++) {
        e1000_write32(E1000_MTA + (i * 4), 0);
    }

    /* Initialize RX and TX rings */
    if (e1000_init_rx() < 0 || e1000_init_tx() < 0) {
        pr_debug("[E1000] Failed to initialize RX/TX descriptor rings\n");
        return -1;
    }

    /* Register ISR and enable IRQ */
    if (g_e1000_dev.irq > 0) {
        extern void idt_register_irq(u8 vector, void (*fn)(pt_regs_t *, void *), void *ctx);
        idt_register_irq(g_e1000_dev.irq + 32, e1000_irq_handler, NULL);
        hal_irq_enable(g_e1000_dev.irq, g_e1000_dev.irq + 32);
    }

    /* Enable interrupts in controller */
    e1000_write32(E1000_IMS, E1000_IMS_RXT0 | E1000_IMS_RXO | E1000_IMS_LSC | E1000_IMS_TXDW);

    g_e1000_ready = true;
    g_e1000_dev.link_up = true;

    net_device_t ndev;
    memset(&ndev, 0, sizeof(ndev));
    strcpy(ndev.name, "e1000");
    memcpy(ndev.mac, g_e1000_dev.mac, 6);
    ndev.send = e1000_send_packet;
    ndev.recv = e1000_recv_packet;
    ndev.link_up = e1000_is_link_up;
    extern int net_register_device(const net_device_t *dev);
    net_register_device(&ndev);

    /* Register device file /dev/net0 */
    devfs_register_device("net0", &g_net_fops, &g_e1000_dev);
    pr_debug("[E1000] Registered /dev/net0 network interface successfully.\n");

    return 0;
}

void e1000_poll_rx(void)
{
    if (!g_e1000_ready) return;

    irqflags_t flags = spinlock_lock_irqsave(&g_e1000_lock);
    while (g_e1000_dev.rx_descs[g_e1000_dev.rx_cur].status & E1000_RXD_STAT_DD) {
        u32 cur = g_e1000_dev.rx_cur;
        u16 len = g_e1000_dev.rx_descs[cur].length;
        u8 *pkt = g_e1000_dev.rx_buffers[cur];

        if (len > 0) {
            extern void net_process_incoming(const u8 *pkt, size_t len);
            net_process_incoming(pkt, len);
        }

        g_e1000_dev.rx_descs[cur].status = 0;
        e1000_write32(E1000_RDT, cur);
        g_e1000_dev.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
    }
    spinlock_unlock_irqrestore(&g_e1000_lock, flags);
}

static void e1000_irq_handler(pt_regs_t *r, void *ctx)
{
    (void)r; (void)ctx;
    u32 icr = e1000_read32(E1000_ICR);
    if (!icr) return;

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXO | E1000_ICR_LSC)) {
        e1000_poll_rx();
    }
}

s64 e1000_send_packet(const void *data, size_t len)
{
    if (!g_e1000_ready || !data || len == 0 || len > E1000_PKT_BUF_SIZE) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_e1000_lock);
    u32 cur = g_e1000_dev.tx_cur;

    /* Wait for descriptor to be free */
    while (!(g_e1000_dev.tx_descs[cur].status & E1000_TXD_STAT_DD)) {
        cpu_pause();
    }

    memcpy(g_e1000_dev.tx_buffers[cur], data, len);
    g_e1000_dev.tx_descs[cur].length = (u16)len;
    g_e1000_dev.tx_descs[cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    g_e1000_dev.tx_descs[cur].status = 0;

    g_e1000_dev.tx_cur = (cur + 1) % E1000_NUM_TX_DESC;
    e1000_write32(E1000_TDT, g_e1000_dev.tx_cur);

    spinlock_unlock_irqrestore(&g_e1000_lock, flags);
    return (s64)len;
}

s64 e1000_recv_packet(void *buf, size_t max_len)
{
    if (!g_e1000_ready || !buf || max_len == 0) return -(s64)EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_e1000_lock);
    u32 cur = g_e1000_dev.rx_cur;

    if (!(g_e1000_dev.rx_descs[cur].status & E1000_RXD_STAT_DD)) {
        spinlock_unlock_irqrestore(&g_e1000_lock, flags);
        return -(s64)EAGAIN; /* No packet ready */
    }

    u16 len = g_e1000_dev.rx_descs[cur].length;
    size_t copy_len = (len < max_len) ? len : max_len;
    memcpy(buf, g_e1000_dev.rx_buffers[cur], copy_len);

    g_e1000_dev.rx_descs[cur].status = 0;
    e1000_write32(E1000_RDT, cur);

    g_e1000_dev.rx_cur = (cur + 1) % E1000_NUM_RX_DESC;
    spinlock_unlock_irqrestore(&g_e1000_lock, flags);

    return (s64)copy_len;
}

void e1000_get_mac(u8 mac_out[6])
{
    if (mac_out) {
        memcpy(mac_out, g_e1000_dev.mac, 6);
    }
}

bool e1000_is_link_up(void)
{
    return g_e1000_ready && g_e1000_dev.link_up;
}
