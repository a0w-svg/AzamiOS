/* ============================================================================
 * AzamiOS — Intel PRO/100 (i8255x / e100) Fast Ethernet Driver
 * File: drivers/net/e100.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "e100.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"
#include "../../fs/vfs.h"

static e100_device_t g_e100_dev;
extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── CSR Register Accessors ─────────────────────────────────────────────── */

static inline u16 e100_csr_read16(e100_device_t *dev, u32 reg)
{
    if (dev->use_io) return inw(dev->io_base + reg);
    return *(volatile u16 *)(dev->mmio_base + reg);
}

static inline void e100_csr_write16(e100_device_t *dev, u32 reg, u16 val)
{
    if (dev->use_io) outw(dev->io_base + reg, val);
    else *(volatile u16 *)(dev->mmio_base + reg) = val;
}

static inline u32 e100_csr_read32(e100_device_t *dev, u32 reg)
{
    if (dev->use_io) return inl(dev->io_base + reg);
    return *(volatile u32 *)(dev->mmio_base + reg);
}

static inline void e100_csr_write32(e100_device_t *dev, u32 reg, u32 val)
{
    if (dev->use_io) outl(dev->io_base + reg, val);
    else *(volatile u32 *)(dev->mmio_base + reg) = val;
}

/* ── SCB Command Handshake ──────────────────────────────────────────────── */

static void e100_scb_wait(e100_device_t *dev)
{
    for (int i = 0; i < 10000; i++) {
        if ((e100_csr_read16(dev, E100_SCB_CMD) & 0xFF) == 0) return;
        cpu_pause();
    }
}

/* ── EEPROM Serial Bit-Bang Read ────────────────────────────────────────── */

static void eeprom_raise_clock(e100_device_t *dev, u16 *val)
{
    *val |= E100_EEPROM_EESK;
    e100_csr_write16(dev, E100_SCB_EEPROM_CTRL, *val);
    for (volatile int i = 0; i < 100; i++) cpu_pause();
}

static void eeprom_lower_clock(e100_device_t *dev, u16 *val)
{
    *val &= ~E100_EEPROM_EESK;
    e100_csr_write16(dev, E100_SCB_EEPROM_CTRL, *val);
    for (volatile int i = 0; i < 100; i++) cpu_pause();
}

static u16 e100_eeprom_read_word(e100_device_t *dev, u8 addr)
{
    u16 val = E100_EEPROM_EECS;
    e100_csr_write16(dev, E100_SCB_EEPROM_CTRL, val);

    /* 3-bit Opcode 110b (Read) + 6-bit Address */
    u16 cmd = (0x6 << 6) | (addr & 0x3F);
    for (int i = 8; i >= 0; i--) {
        if (cmd & (1 << i)) val |= E100_EEPROM_EEDI;
        else val &= ~E100_EEPROM_EEDI;
        e100_csr_write16(dev, E100_SCB_EEPROM_CTRL, val);
        eeprom_raise_clock(dev, &val);
        eeprom_lower_clock(dev, &val);
    }

    /* Shift in 16 bits of EEPROM word data */
    u16 data = 0;
    for (int i = 15; i >= 0; i--) {
        eeprom_raise_clock(dev, &val);
        u16 cur = e100_csr_read16(dev, E100_SCB_EEPROM_CTRL);
        if (cur & E100_EEPROM_EEDO) data |= (1 << i);
        eeprom_lower_clock(dev, &val);
    }

    val &= ~E100_EEPROM_EECS;
    e100_csr_write16(dev, E100_SCB_EEPROM_CTRL, val);
    for (volatile int i = 0; i < 100; i++) cpu_pause();

    return data;
}

/* ── Network Packet Send / Recv ─────────────────────────────────────────── */

static s64 e100_send_packet(const void *buf, size_t len)
{
    if (!g_e100_dev.ready || !buf || len == 0 || len > 1514) return -1;

    e100_scb_wait(&g_e100_dev);

    g_e100_dev.tx_cb->status = 0;
    g_e100_dev.tx_cb->command = E100_CB_CMD_TX | E100_CB_CMD_EL;
    g_e100_dev.tx_cb->link = 0xFFFFFFFF;
    g_e100_dev.tx_cb->tbd_array_addr = 0xFFFFFFFF;
    g_e100_dev.tx_cb->byte_count = (u16)(len | 0x8000); /* EOF bit */
    g_e100_dev.tx_cb->tx_threshold = 0xE0;
    g_e100_dev.tx_cb->tbd_count = 0;
    memcpy((void *)g_e100_dev.tx_cb->data, buf, len);

    e100_csr_write32(&g_e100_dev, E100_SCB_POINTER, (u32)g_e100_dev.tx_cb_phys);
    e100_csr_write16(&g_e100_dev, E100_SCB_CMD, E100_CU_START);

    bool completed = false;
    for (int timeout = 0; timeout < 50000; timeout++) {
        if (g_e100_dev.tx_cb->status & E100_CB_STATUS_C) {
            completed = true;
            break;
        }
        cpu_pause();
    }

    u16 status = e100_csr_read16(&g_e100_dev, E100_SCB_STATUS);
    e100_csr_write16(&g_e100_dev, E100_SCB_STATUS, status & 0xFF00);
    return completed ? (s64)len : -1;
}

static s64 e100_recv_packet(void *buf, size_t max_len)
{
    if (!g_e100_dev.ready || !buf || max_len == 0) return 0;

    e100_rfd_t *rfd = g_e100_dev.rfd_ring[g_e100_dev.rx_cur];
    if (!(rfd->status & E100_RFD_STATUS_C)) {
        return 0;
    }

    u16 count = rfd->actual_count & 0x3FFF;
    if (count > max_len) count = (u16)max_len;
    memcpy(buf, (const void *)rfd->data, count);

    /* Reset current descriptor and advance circular EL marker */
    rfd->status = 0;
    rfd->command = 0;
    rfd->actual_count = 0;
    rfd->size = 1536;

    int prev = (g_e100_dev.rx_cur + E100_NUM_RFD - 1) % E100_NUM_RFD;
    g_e100_dev.rfd_ring[prev]->command = E100_RFD_CMD_EL;

    g_e100_dev.rx_cur = (g_e100_dev.rx_cur + 1) % E100_NUM_RFD;

    u16 status = e100_csr_read16(&g_e100_dev, E100_SCB_STATUS);
    e100_csr_write16(&g_e100_dev, E100_SCB_STATUS, status & 0xFF00);

    /* If RU is idle or suspended, restart RU on current RFD */
    u8 ru_stat = (status >> 2) & 0x0F;
    if (ru_stat == 0 || ru_stat == 1 || ru_stat == 2) {
        e100_scb_wait(&g_e100_dev);
        e100_csr_write32(&g_e100_dev, E100_SCB_POINTER, (u32)g_e100_dev.rfd_phys[g_e100_dev.rx_cur]);
        e100_csr_write16(&g_e100_dev, E100_SCB_CMD, E100_RU_START);
    }

    return (int)count;
}

/* ── Character Device Operations for /dev/net3 and /dev/e100 ─────────────── */

static s64 e100_dev_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_e100_dev.ready || !buf || len == 0) return 0;

    int n = e100_recv_packet(buf, len);
    if (n > 0) return n;

    /* If no packet and requested a status readout */
    if (len >= 128 && *offset == 0) {
        char s[128];
        int sl = scnprintf(s, sizeof(s),
                           "Intel e100 MAC: %02x:%02x:%02x:%02x:%02x:%02x (IRQ %d)\n",
                           g_e100_dev.mac[0], g_e100_dev.mac[1], g_e100_dev.mac[2],
                           g_e100_dev.mac[3], g_e100_dev.mac[4], g_e100_dev.mac[5],
                           g_e100_dev.irq);
        if (len > (size_t)sl) len = (size_t)sl;
        memcpy(buf, s, len);
        *offset += len;
        return (s64)len;
    }
    return 0;
}

static s64 e100_dev_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_e100_dev.ready || !buf || len == 0) return -ENODEV;
    if (len > 1514) return -EMSGSIZE;

    s64 ret = e100_send_packet(buf, len);
    if (ret < 0) return -EIO;
    return ret;
}

static s64 e100_dev_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_e100_dev.ready) return -ENODEV;

    switch (cmd) {
    case 0x8927: /* SIOCGIFHWADDR - get MAC */
        if (!arg) return -EFAULT;
        if (copy_to_user((void *)arg, g_e100_dev.mac, 6) != 0) return -EFAULT;
        return 0;
    default:
        return -EINVAL;
    }
}

static file_operations_t g_e100_fops = {
    .read  = e100_dev_read,
    .write = e100_dev_write,
    .ioctl = e100_dev_ioctl,
};

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int e100_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    pci_enable_bus_mastering(dev->hal);

    /* Check BAR0 (MMIO) and BAR1 (I/O) */
    u32 bar0 = info->bar[0];
    u32 bar1 = info->bar[1];

    if (!(bar0 & 1) && (bar0 & ~0xFULL) != 0) {
        /* MMIO Mode */
        phys_addr_t bar0_aligned = ALIGN_DOWN(bar0 & ~0xFULL, 4096);
        virt_addr_t bar0_virt = (virt_addr_t)PHYS_TO_VIRT(bar0_aligned);
        vmm_map(0, bar0_virt, bar0_aligned, VMM_MMIO);
        g_e100_dev.mmio_phys = bar0_aligned;
        g_e100_dev.mmio_base = bar0_virt;
        g_e100_dev.use_io    = false;
    } else if ((bar1 & 1) && (bar1 & ~0x3) != 0) {
        /* Port I/O Mode */
        g_e100_dev.io_base = (u16)(bar1 & ~0x3);
        g_e100_dev.use_io  = true;
    } else {
        pr_debug("[E100] Neither valid MMIO nor I/O BAR found on %04X:%04X\n",
                 info->vendor_id, info->device_id);
        return -ENXIO;
    }

    g_e100_dev.irq = info->interrupt_line;

    /* Software Reset via PORT register */
    e100_csr_write32(&g_e100_dev, E100_SCB_PORT, E100_PORT_SOFTWARE_RESET);
    for (volatile int i = 0; i < 50000; i++) cpu_pause();

    /* Disable interrupts for polling operation */
    e100_csr_write16(&g_e100_dev, E100_SCB_CMD, E100_INT_MASK);

    /* Read MAC from EEPROM words 0..2 */
    u16 w0 = e100_eeprom_read_word(&g_e100_dev, 0);
    u16 w1 = e100_eeprom_read_word(&g_e100_dev, 1);
    u16 w2 = e100_eeprom_read_word(&g_e100_dev, 2);

    g_e100_dev.mac[0] = (u8)(w0 & 0xFF);
    g_e100_dev.mac[1] = (u8)(w0 >> 8);
    g_e100_dev.mac[2] = (u8)(w1 & 0xFF);
    g_e100_dev.mac[3] = (u8)(w1 >> 8);
    g_e100_dev.mac[4] = (u8)(w2 & 0xFF);
    g_e100_dev.mac[5] = (u8)(w2 >> 8);

    /* Fallback if unconfigured in virtual environment */
    if ((w0 == 0 && w1 == 0 && w2 == 0) || (w0 == 0xFFFF && w1 == 0xFFFF && w2 == 0xFFFF)) {
        g_e100_dev.mac[0] = 0x52;
        g_e100_dev.mac[1] = 0x54;
        g_e100_dev.mac[2] = 0x00;
        g_e100_dev.mac[3] = 0x12;
        g_e100_dev.mac[4] = 0x34;
        g_e100_dev.mac[5] = 0x58;
    }

    /* Allocate Tx Command Block page */
    phys_addr_t tx_phys = pmm_alloc_page();
    if (!tx_phys) return -ENOMEM;
    g_e100_dev.tx_cb_phys = tx_phys;
    g_e100_dev.tx_cb      = (e100_tx_cb_t *)PHYS_TO_VIRT(tx_phys);
    memset(g_e100_dev.tx_cb, 0, sizeof(e100_tx_cb_t));

    /* Allocate Receive Frame Descriptor (RFD) ring pages */
    for (int i = 0; i < E100_NUM_RFD; i++) {
        phys_addr_t rfd_p = pmm_alloc_page();
        if (!rfd_p) return -ENOMEM;
        g_e100_dev.rfd_phys[i] = rfd_p;
        g_e100_dev.rfd_ring[i] = (e100_rfd_t *)PHYS_TO_VIRT(rfd_p);
        memset(g_e100_dev.rfd_ring[i], 0, sizeof(e100_rfd_t));
        g_e100_dev.rfd_ring[i]->size = 1536;
    }

    /* Link RFD ring circularly */
    for (int i = 0; i < E100_NUM_RFD; i++) {
        int next = (i + 1) % E100_NUM_RFD;
        g_e100_dev.rfd_ring[i]->link = (u32)g_e100_dev.rfd_phys[next];
        if (i == E100_NUM_RFD - 1) {
            g_e100_dev.rfd_ring[i]->command = E100_RFD_CMD_EL; /* End of list */
        }
    }
    g_e100_dev.rx_cur = 0;

    /* Start Receive Unit (RU) */
    e100_scb_wait(&g_e100_dev);
    e100_csr_write32(&g_e100_dev, E100_SCB_POINTER, (u32)g_e100_dev.rfd_phys[0]);
    e100_csr_write16(&g_e100_dev, E100_SCB_CMD, E100_RU_START);

    /* Initialize AzamiOS network device descriptor */
    memset(&g_e100_dev.netdev, 0, sizeof(net_device_t));
    strcpy(g_e100_dev.netdev.name, "e100");
    memcpy(g_e100_dev.netdev.mac, g_e100_dev.mac, 6);
    g_e100_dev.netdev.flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST;
    g_e100_dev.netdev.mtu   = 1500;
    g_e100_dev.netdev.send  = e100_send_packet;
    g_e100_dev.netdev.recv  = e100_recv_packet;
    net_register_device(&g_e100_dev.netdev);

    /* Register /dev/net3 and /dev/e100 */
    devfs_register_device("net3", &g_e100_fops, &g_e100_dev);
    devfs_register_device("e100", &g_e100_fops, &g_e100_dev);

    g_e100_dev.ready = true;

    pr_debug("[E100] Intel PRO/100 Ethernet ready: MAC %02x:%02x:%02x:%02x:%02x:%02x, IRQ %d (/dev/net3, /dev/e100)\n",
             g_e100_dev.mac[0], g_e100_dev.mac[1], g_e100_dev.mac[2],
             g_e100_dev.mac[3], g_e100_dev.mac[4], g_e100_dev.mac[5],
             g_e100_dev.irq);
    return 0;
}

static const pci_device_id_t g_e100_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x1229) }, /* Intel 82557/8/9 Pro/100 */
    { PCI_DEVICE(0x8086, 0x1209) }, /* Intel 82559ER */
    { PCI_DEVICE(0x8086, 0x1029) }, /* Intel 82559 InBusiness */
    { PCI_DEVICE(0x8086, 0x1030) }, /* Intel 82559 InBusiness 10/100 */
    { PCI_DEVICE(0x8086, 0x1031) }, /* Intel 82562ET Pro/100 VE */
    { PCI_DEVICE(0x8086, 0x1039) }, /* Intel 82562ET Pro/100 VE */
    { PCI_DEVICE(0x8086, 0x103D) }, /* Intel 82562ET Pro/100 VM */
    { PCI_DEVICE(0x8086, 0x1050) }, /* Intel 82562EZ Pro/100 VE */
    { 0 }
};

static pci_driver_t g_e100_pci_driver = {
    .drv = {
        .name = "e100",
    },
    .id_table = g_e100_pci_ids,
    .probe    = e100_pci_probe,
    .remove   = NULL,
};

int e100_init(void)
{
    return pci_driver_register(&g_e100_pci_driver);
}

void e100_get_mac(u8 mac_out[6])
{
    if (mac_out) {
        memcpy(mac_out, g_e100_dev.mac, 6);
    }
}
