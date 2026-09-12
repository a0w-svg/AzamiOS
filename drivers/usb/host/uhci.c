/* ============================================================================
 * AzamiOS — Universal Host Controller Interface (UHCI) Driver
 * File: drivers/usb/host/uhci.c
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "uhci.h"
#include "../../../fs/vfs.h"
#include "../../../hal/pci.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../arch/x86_64/mm/vmm.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/lib/string.h"

static uhci_controller_t g_uhci_ctrl;
static bool              g_uhci_ready = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── Character Device Operations for /dev/uhci0 ───────────────────────────── */

static s64 uhci_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_uhci_ready) return 0;
    if (*offset > 0) return 0;

    char status_str[256];
    u16 cmd  = inw(g_uhci_ctrl.io_base + UHCI_REG_USBCMD);
    u16 sts  = inw(g_uhci_ctrl.io_base + UHCI_REG_USBSTS);
    u16 p1   = inw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC1);
    u16 p2   = inw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC2);

    int n = scnprintf(status_str, sizeof(status_str),
                      "UHCI at I/O 0x%04X, IRQ %d\n"
                      "USBCMD: 0x%04X, USBSTS: 0x%04X\n"
                      "Port 1: 0x%04X (%s, %s)\n"
                      "Port 2: 0x%04X (%s, %s)\n"
                      "Ports active: %u\n",
                      g_uhci_ctrl.io_base, g_uhci_ctrl.irq, cmd, sts,
                      p1, (p1 & UHCI_PORT_CCS) ? "connected" : "disconnected",
                      (p1 & UHCI_PORT_LSDA) ? "low-speed" : "full-speed",
                      p2, (p2 & UHCI_PORT_CCS) ? "connected" : "disconnected",
                      (p2 & UHCI_PORT_LSDA) ? "low-speed" : "full-speed",
                      g_uhci_ctrl.ports_detected);

    if (len > (size_t)n) len = (size_t)n;
    memcpy(buf, status_str, len);
    *offset += len;
    return (s64)len;
}

static s64 uhci_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp; (void)arg;
    if (!g_uhci_ready) return -ENODEV;

    switch (cmd) {
    case 0x5501: /* UHCI_IOC_RESET_PORT1 */
        outw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC1, UHCI_PORT_PR);
        for (volatile int i = 0; i < 100000; i++) cpu_pause();
        outw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC1, UHCI_PORT_PE);
        return 0;
    case 0x5502: /* UHCI_IOC_RESET_PORT2 */
        outw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC2, UHCI_PORT_PR);
        for (volatile int i = 0; i < 100000; i++) cpu_pause();
        outw(g_uhci_ctrl.io_base + UHCI_REG_PORTSC2, UHCI_PORT_PE);
        return 0;
    default:
        return -EINVAL;
    }
}

static file_operations_t g_uhci_fops = {
    .read  = uhci_read,
    .write = NULL,
    .ioctl = uhci_ioctl,
};

/* ── Port Reset & Detection Helper ───────────────────────────────────────── */

static void uhci_probe_port(u16 reg_offset, u8 port_idx)
{
    u16 port_reg = g_uhci_ctrl.io_base + reg_offset;
    u16 val = inw(port_reg);

    if (val & UHCI_PORT_CCS) {
        bool low_speed = (val & UHCI_PORT_LSDA) != 0;
        pr_debug("[UHCI] Port %d device attached (%s)\n",
                 port_idx, low_speed ? "Low-Speed 1.5 Mbps" : "Full-Speed 12 Mbps");

        /* Reset port per USB 1.1 spec */
        outw(port_reg, UHCI_PORT_PR);
        for (volatile int i = 0; i < 500000; i++) cpu_pause();
        outw(port_reg, 0);
        for (volatile int i = 0; i < 100000; i++) cpu_pause();

        /* Enable port */
        outw(port_reg, UHCI_PORT_PE);
        for (volatile int i = 0; i < 100000; i++) cpu_pause();

        g_uhci_ctrl.ports_detected++;
        usb_device_create(&g_uhci_ctrl.bus, port_idx,
                          low_speed ? USB_SPEED_LOW : USB_SPEED_FULL);
    }
}

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int uhci_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    /* Find I/O base in PCI BARs (typically BAR4 on Intel PIIX/ICH) */
    u16 io_base = 0;
    for (int b = 0; b < 6; b++) {
        if ((info->bar[b] & 1) && (info->bar[b] & ~0x3) != 0) {
            io_base = (u16)(info->bar[b] & ~0x3);
            break;
        }
    }

    if (io_base == 0) {
        pr_debug("[UHCI] No valid I/O BAR found on PCI device %04X:%04X\n",
                 info->vendor_id, info->device_id);
        return -ENXIO;
    }

    /* Enable Bus Mastering */
    pci_enable_bus_mastering(dev->hal);

    g_uhci_ctrl.io_base = io_base;
    g_uhci_ctrl.irq     = info->interrupt_line;
    g_uhci_ctrl.ports_detected = 0;

    /* Allocate 4KB page-aligned 1024-entry Frame List */
    phys_addr_t frame_phys = pmm_alloc_page();
    if (!frame_phys) {
        pr_debug("[UHCI] Failed to allocate physical page for Frame List\n");
        return -ENOMEM;
    }
    g_uhci_ctrl.frame_list_phys = frame_phys;
    g_uhci_ctrl.frame_list_virt = (u32 *)PHYS_TO_VIRT(frame_phys);
    memset(g_uhci_ctrl.frame_list_virt, 0, 4096);

    /* Allocate default Queue Head */
    g_uhci_ctrl.default_qh = (uhci_qh_t *)kmalloc(sizeof(uhci_qh_t));
    if (!g_uhci_ctrl.default_qh) {
        pmm_free_page(frame_phys);
        return -ENOMEM;
    }
    memset(g_uhci_ctrl.default_qh, 0, sizeof(uhci_qh_t));
    g_uhci_ctrl.default_qh->head_link    = 1; /* Terminate */
    g_uhci_ctrl.default_qh->element_link = 1; /* Terminate */
    g_uhci_ctrl.default_qh_phys = VIRT_TO_PHYS(g_uhci_ctrl.default_qh);

    /* Link all 1024 frame entries to our default Queue Head (QH=1, active=0) */
    u32 qh_entry = (u32)(g_uhci_ctrl.default_qh_phys | 2);
    for (int i = 0; i < 1024; i++) {
        g_uhci_ctrl.frame_list_virt[i] = qh_entry;
    }

    /* Reset Host Controller */
    outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_HCRESET);
    for (int timeout = 0; timeout < 1000; timeout++) {
        if (!(inw(io_base + UHCI_REG_USBCMD) & UHCI_CMD_HCRESET)) break;
        cpu_pause();
    }

    /* Program Frame List Base Address */
    outl(io_base + UHCI_REG_FLBASEADD, (u32)frame_phys);
    outw(io_base + UHCI_REG_FRNUM, 0);
    outb(io_base + UHCI_REG_SOFMOD, 0x40);

    /* Clear pending status flags */
    outw(io_base + UHCI_REG_USBSTS, 0xFFFF);

    /* Start Host Controller: Run/Stop=1, Configure Flag=1, Max Packet 64=1 */
    outw(io_base + UHCI_REG_USBCMD, UHCI_CMD_RS | UHCI_CMD_CF | UHCI_CMD_MAXP);
    g_uhci_ctrl.running = true;

    /* Initialize core bus descriptor */
    g_uhci_ctrl.bus.name    = "uhci";
    g_uhci_ctrl.bus.bus_num = 0;
    g_uhci_ctrl.bus.hcd     = &g_uhci_ctrl;
    g_uhci_ctrl.bus.dev     = dev;

    /* Probe Root Hub Ports */
    uhci_probe_port(UHCI_REG_PORTSC1, 1);
    uhci_probe_port(UHCI_REG_PORTSC2, 2);

    /* Register /dev/uhci0 */
    devfs_register_device("uhci0", &g_uhci_fops, &g_uhci_ctrl);
    g_uhci_ready = true;

    pr_debug("[UHCI] Intel UHCI USB 1.1 controller ready at I/O 0x%04X, IRQ %d (/dev/uhci0)\n",
             io_base, g_uhci_ctrl.irq);
    return 0;
}

static const pci_device_id_t g_uhci_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x7020) }, /* Intel PIIX3 UHCI */
    { PCI_DEVICE(0x8086, 0x7112) }, /* Intel PIIX4 UHCI */
    { PCI_DEVICE(0x8086, 0x2412) }, /* Intel 82801AA ICH UHCI */
    { PCI_DEVICE(0x8086, 0x2422) }, /* Intel 82801AB ICH0 UHCI */
    { PCI_DEVICE(0x8086, 0x2442) }, /* Intel 82801BA/BAM ICH2 UHCI */
    { PCI_DEVICE(0x8086, 0x2934) }, /* Intel ICH9 UHCI1 */
    { PCI_DEVICE(0x8086, 0x2935) }, /* Intel ICH9 UHCI2 */
    { PCI_DEVICE(0x8086, 0x2936) }, /* Intel ICH9 UHCI3 */
    { PCI_DEVICE(0x8086, 0x2937) }, /* Intel ICH9 UHCI4 */
    { PCI_DEVICE(0x8086, 0x2938) }, /* Intel ICH9 UHCI5 */
    { PCI_DEVICE(0x8086, 0x2939) }, /* Intel ICH9 UHCI6 */
    { PCI_DEVICE_CLASS(0x0C0300, 0xFFFFFF) }, /* Generic UHCI */
    { 0 }
};

static pci_driver_t g_uhci_pci_driver = {
    .drv = {
        .name = "uhci_pci",
    },
    .id_table = g_uhci_pci_ids,
    .probe    = uhci_pci_probe,
    .remove   = NULL,
};

int uhci_init(void)
{
    usb_core_init();
    return pci_driver_register(&g_uhci_pci_driver);
}
