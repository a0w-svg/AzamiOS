/* ============================================================================
 * AzamiOS — Enhanced Host Controller Interface (EHCI) USB 2.0 Driver
 * File: drivers/usb/host/ehci.c
 * ============================================================================ */

#define DEBUG 1
#include "../../../include/azami/debug.h"
#include "ehci.h"
#include "../../../fs/vfs.h"
#include "../../../hal/pci.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../arch/x86_64/mm/vmm.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/lib/string.h"

static ehci_controller_t g_ehci_ctrl;
static bool              g_ehci_ready = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── Register Accessors ─────────────────────────────────────────────────── */

static inline u8 ehci_cap_read8(ehci_controller_t *ctrl, u32 reg)
{
    return *(volatile u8 *)(ctrl->mmio_base + reg);
}

static inline u16 ehci_cap_read16(ehci_controller_t *ctrl, u32 reg)
{
    return *(volatile u16 *)(ctrl->mmio_base + reg);
}

static inline u32 ehci_cap_read32(ehci_controller_t *ctrl, u32 reg)
{
    return *(volatile u32 *)(ctrl->mmio_base + reg);
}

static inline u32 ehci_read32(ehci_controller_t *ctrl, u32 reg)
{
    return *(volatile u32 *)(ctrl->op_regs + reg);
}

static inline void ehci_write32(ehci_controller_t *ctrl, u32 reg, u32 val)
{
    *(volatile u32 *)(ctrl->op_regs + reg) = val;
}

/* ── Character Device Operations for /dev/ehci0 ──────────────────────────── */

static s64 ehci_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_ehci_ready) return 0;
    if (*offset > 0) return 0;

    char status_str[512];
    u32 cmd = ehci_read32(&g_ehci_ctrl, EHCI_OP_USBCMD);
    u32 sts = ehci_read32(&g_ehci_ctrl, EHCI_OP_USBSTS);
    int written = scnprintf(status_str, sizeof(status_str),
                           "EHCI at MMIO 0x%lx, IRQ %d\n"
                           "Version: 0x%04X, Ports: %u\n"
                           "USBCMD: 0x%08X (Run=%d, PSE=%d, ASE=%d)\n"
                           "USBSTS: 0x%08X (HCHalted=%d, PSS=%d, ASS=%d)\n"
                           "Ports active: %u\n",
                           (unsigned long)g_ehci_ctrl.mmio_phys, g_ehci_ctrl.irq,
                           g_ehci_ctrl.hciversion, g_ehci_ctrl.num_ports,
                           cmd, (cmd & EHCI_CMD_RUN) ? 1 : 0,
                           (cmd & EHCI_CMD_PSE) ? 1 : 0, (cmd & EHCI_CMD_ASE) ? 1 : 0,
                           sts, (sts & EHCI_STS_HCHALTED) ? 1 : 0,
                           (sts & EHCI_STS_PSS) ? 1 : 0, (sts & EHCI_STS_ASS) ? 1 : 0,
                           g_ehci_ctrl.ports_detected);

    for (u8 p = 1; p <= g_ehci_ctrl.num_ports && written < (int)sizeof(status_str) - 64; p++) {
        u32 portsc = ehci_read32(&g_ehci_ctrl, EHCI_OP_PORTSC(p));
        written += scnprintf(status_str + written, sizeof(status_str) - written,
                             "Port %u: 0x%08X (CCS=%d, PE=%d, PP=%d)\n",
                             p, portsc,
                             (portsc & EHCI_PORT_CCS) ? 1 : 0,
                             (portsc & EHCI_PORT_PE) ? 1 : 0,
                             (portsc & EHCI_PORT_PP) ? 1 : 0);
    }

    if (len > (size_t)written) len = (size_t)written;
    memcpy(buf, status_str, len);
    *offset += len;
    return (s64)len;
}

static s64 ehci_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (!g_ehci_ready) return -ENODEV;

    switch (cmd) {
    case EHCI_IOC_GET_NUM_PORTS:
        return (s64)g_ehci_ctrl.num_ports;

    case EHCI_IOC_RESET_PORT: {
        u8 port = (u8)arg;
        if (port < 1 || port > g_ehci_ctrl.num_ports) return -EINVAL;

        u32 portsc = ehci_read32(&g_ehci_ctrl, EHCI_OP_PORTSC(port));
        /* Preserve write-zero-to-clear status bits and keep power enabled */
        portsc &= ~(EHCI_PORT_CSC | EHCI_PORT_PEC | EHCI_PORT_OCC);
        portsc |= EHCI_PORT_PP | EHCI_PORT_RESET;
        ehci_write32(&g_ehci_ctrl, EHCI_OP_PORTSC(port), portsc);

        /* Wait at least 50ms for USB reset signal */
        for (volatile int i = 0; i < 500000; i++) cpu_pause();

        /* Terminate reset */
        portsc &= ~EHCI_PORT_RESET;
        ehci_write32(&g_ehci_ctrl, EHCI_OP_PORTSC(port), portsc);

        for (volatile int i = 0; i < 100000; i++) cpu_pause();
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static file_operations_t g_ehci_fops = {
    .read  = ehci_read,
    .write = NULL,
    .ioctl = ehci_ioctl,
};

/* ── Port Reset & Detection Helper ───────────────────────────────────────── */

static void ehci_probe_ports(ehci_controller_t *ctrl)
{
    ctrl->ports_detected = 0;
    for (u8 p = 1; p <= ctrl->num_ports; p++) {
        u32 portsc = ehci_read32(ctrl, EHCI_OP_PORTSC(p));

        /* Ensure port power is turned on */
        if (!(portsc & EHCI_PORT_PP)) {
            portsc |= EHCI_PORT_PP;
            ehci_write32(ctrl, EHCI_OP_PORTSC(p), portsc);
            for (volatile int i = 0; i < 100000; i++) cpu_pause();
            portsc = ehci_read32(ctrl, EHCI_OP_PORTSC(p));
        }

        if (portsc & EHCI_PORT_CCS) {
            pr_debug("[EHCI] Port %u device connected, resetting port...\n", p);

            /* Issue port reset */
            ehci_write32(ctrl, EHCI_OP_PORTSC(p), (portsc & ~EHCI_PORT_PE) | EHCI_PORT_RESET);
            for (volatile int i = 0; i < 500000; i++) cpu_pause();

            /* Clear reset bit */
            portsc = ehci_read32(ctrl, EHCI_OP_PORTSC(p));
            ehci_write32(ctrl, EHCI_OP_PORTSC(p), portsc & ~EHCI_PORT_RESET);
            for (volatile int i = 0; i < 200000; i++) cpu_pause();

            portsc = ehci_read32(ctrl, EHCI_OP_PORTSC(p));
            if (portsc & EHCI_PORT_PE) {
                ctrl->ports_detected++;
                pr_debug("[EHCI] Port %u enabled at High-Speed (480 Mbps)\n", p);
                usb_device_create(&ctrl->bus, p, USB_SPEED_HIGH);
            }
        }
    }
}

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int ehci_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    /* Get MMIO BAR0 */
    u32 bar0 = info->bar[0] & ~0xFULL;
    if (bar0 == 0) {
        pr_debug("[EHCI] Invalid or zero MMIO BAR0 on PCI device %04X:%04X\n",
                 info->vendor_id, info->device_id);
        return -ENXIO;
    }

    /* Enable Bus Mastering and Memory Space Access */
    pci_enable_bus_mastering(dev->hal);

    /* Map MMIO pages (typical EHCI MMIO space is <= 4KB) */
    phys_addr_t bar0_aligned = ALIGN_DOWN(bar0, 4096);
    virt_addr_t bar0_virt = (virt_addr_t)PHYS_TO_VIRT(bar0_aligned);
    vmm_map(0, bar0_virt, bar0_aligned, VMM_MMIO);

    g_ehci_ctrl.mmio_phys = bar0_aligned;
    g_ehci_ctrl.mmio_base = bar0_virt;
    g_ehci_ctrl.irq       = info->interrupt_line;

    /* Read Capability Registers */
    g_ehci_ctrl.caplength  = ehci_cap_read8(&g_ehci_ctrl, EHCI_CAP_CAPLENGTH);
    g_ehci_ctrl.hciversion = ehci_cap_read16(&g_ehci_ctrl, EHCI_CAP_HCIVERSION);
    u32 hcsparams          = ehci_cap_read32(&g_ehci_ctrl, EHCI_CAP_HCSPARAMS);
    g_ehci_ctrl.num_ports  = (u8)(hcsparams & 0x0F);

    if (g_ehci_ctrl.num_ports == 0 || g_ehci_ctrl.num_ports > 15) {
        g_ehci_ctrl.num_ports = 4; /* Safe fallback */
    }

    g_ehci_ctrl.op_regs = g_ehci_ctrl.mmio_base + g_ehci_ctrl.caplength;

    pr_debug("[EHCI] Initializing EHCI %04X (CAPLENGTH 0x%02X, %u ports, IRQ %d)\n",
             g_ehci_ctrl.hciversion, g_ehci_ctrl.caplength, g_ehci_ctrl.num_ports, g_ehci_ctrl.irq);

    /* Allocate 4KB page-aligned 1024-entry Periodic Frame List */
    phys_addr_t frame_phys = pmm_alloc_page();
    if (!frame_phys) {
        pr_debug("[EHCI] Failed to allocate physical page for Frame List\n");
        return -ENOMEM;
    }
    g_ehci_ctrl.periodic_list_phys = frame_phys;
    g_ehci_ctrl.periodic_list_virt = (u32 *)PHYS_TO_VIRT(frame_phys);
    /* Set bit 0 (terminate bit) on all 1024 entries */
    for (int i = 0; i < 1024; i++) {
        g_ehci_ctrl.periodic_list_virt[i] = 1;
    }

    /* Allocate physical page for circular Asynchronous Queue Head (QH) */
    phys_addr_t qh_phys = pmm_alloc_page();
    if (!qh_phys) {
        pmm_free_page(frame_phys);
        return -ENOMEM;
    }
    g_ehci_ctrl.async_qh_phys = qh_phys;
    g_ehci_ctrl.async_qh      = (ehci_qh_t *)PHYS_TO_VIRT(qh_phys);
    memset(g_ehci_ctrl.async_qh, 0, sizeof(ehci_qh_t));

    /* Circular link pointing to itself (bit 1 indicates QH structure) */
    g_ehci_ctrl.async_qh->horizontal_link = (u32)(qh_phys | 0x02);
    /* High-Speed, Head of reclamation list (H=1), Max packet length 64 */
    g_ehci_ctrl.async_qh->ep_char = (64 << 16) | (2 << 12) | (1 << 15);
    g_ehci_ctrl.async_qh->ep_caps = (1 << 30); /* Multiplier = 1 */
    g_ehci_ctrl.async_qh->next_qtd = 1;        /* Terminate */
    g_ehci_ctrl.async_qh->alt_next_qtd = 1;    /* Terminate */

    /* Reset Host Controller */
    ehci_write32(&g_ehci_ctrl, EHCI_OP_USBCMD, 0); /* Stop controller */
    for (volatile int i = 0; i < 50000; i++) cpu_pause();

    ehci_write32(&g_ehci_ctrl, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    for (int timeout = 0; timeout < 1000; timeout++) {
        if (!(ehci_read32(&g_ehci_ctrl, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) break;
        cpu_pause();
    }

    /* Program Schedule List Addresses */
    ehci_write32(&g_ehci_ctrl, EHCI_OP_CTRLDSSEGMENT, 0);
    ehci_write32(&g_ehci_ctrl, EHCI_OP_PERIODICLISTBASE, (u32)g_ehci_ctrl.periodic_list_phys);
    ehci_write32(&g_ehci_ctrl, EHCI_OP_ASYNCLISTADDR, (u32)g_ehci_ctrl.async_qh_phys);

    /* Clear pending status flags */
    ehci_write32(&g_ehci_ctrl, EHCI_OP_USBSTS, 0x3F);

    /* Route all ports to the EHCI host controller */
    ehci_write32(&g_ehci_ctrl, EHCI_OP_CONFIGFLAG, 1);
    for (volatile int i = 0; i < 50000; i++) cpu_pause();

    /* Start Host Controller (Run, Periodic Schedule Enable, Async Schedule Enable) */
    ehci_write32(&g_ehci_ctrl, EHCI_OP_USBCMD, EHCI_CMD_RUN | EHCI_CMD_PSE | EHCI_CMD_ASE);
    g_ehci_ctrl.running = true;

    /* Initialize core bus descriptor */
    g_ehci_ctrl.bus.name    = "ehci";
    g_ehci_ctrl.bus.bus_num = 1;
    g_ehci_ctrl.bus.hcd     = &g_ehci_ctrl;
    g_ehci_ctrl.bus.dev     = dev;

    /* Probe Root Hub Ports */
    ehci_probe_ports(&g_ehci_ctrl);

    /* Register /dev/ehci0 */
    devfs_register_device("ehci0", &g_ehci_fops, &g_ehci_ctrl);
    g_ehci_ready = true;

    pr_debug("[EHCI] Intel EHCI USB 2.0 controller ready at MMIO 0x%lx, IRQ %d (/dev/ehci0)\n",
             (unsigned long)bar0_aligned, g_ehci_ctrl.irq);
    return 0;
}

static const pci_device_id_t g_ehci_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x293A) }, /* Intel ICH9 EHCI #1 */
    { PCI_DEVICE(0x8086, 0x293C) }, /* Intel ICH9 EHCI #2 */
    { PCI_DEVICE(0x8086, 0x265C) }, /* Intel ICH6 EHCI */
    { PCI_DEVICE(0x8086, 0x24DD) }, /* Intel ICH5 EHCI */
    { PCI_DEVICE(0x8086, 0x27CC) }, /* Intel ICH7 EHCI */
    { PCI_DEVICE(0x8086, 0x2836) }, /* Intel ICH8 EHCI #1 */
    { PCI_DEVICE(0x8086, 0x283A) }, /* Intel ICH8 EHCI #2 */
    { PCI_DEVICE_CLASS(0x0C0320, 0xFFFFFF) }, /* Generic USB 2.0 EHCI Controller */
    { 0 }
};

static pci_driver_t g_ehci_pci_driver = {
    .drv = {
        .name = "ehci_pci",
    },
    .id_table = g_ehci_pci_ids,
    .probe    = ehci_pci_probe,
    .remove   = NULL,
};

int ehci_init(void)
{
    usb_core_init();
    return pci_driver_register(&g_ehci_pci_driver);
}
