/* ============================================================================
 * AzamiOS — VMware VMXNET3 Paravirtualized Network Adapter Driver
 * File: drivers/net/vmxnet3.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "vmxnet3.h"
#include "../../fs/vfs.h"
#include "../../hal/pci.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"

static vmxnet3_device_t g_vmxnet3_dev;
static bool             g_vmxnet3_ready = false;

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

/* ── MMIO Helpers ────────────────────────────────────────────────────────── */

static inline u32 vmxnet3_read32(u32 reg)
{
    return *(volatile u32 *)(g_vmxnet3_dev.mmio_base + reg);
}

static inline void vmxnet3_write32(u32 reg, u32 val)
{
    *(volatile u32 *)(g_vmxnet3_dev.mmio_base + reg) = val;
}

void vmxnet3_get_mac(u8 mac_out[6])
{
    if (!mac_out) return;
    if (g_vmxnet3_ready) {
        memcpy(mac_out, g_vmxnet3_dev.mac, 6);
    } else {
        memset(mac_out, 0, 6);
    }
}

/* ── Character Device Operations for /dev/net2 ───────────────────────────── */

static s64 vmxnet3_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0 || !g_vmxnet3_ready) return 0;
    if (*offset > 0) return 0;

    char status[256];
    int n = scnprintf(status, sizeof(status),
                      "VMXNET3 10-GbE Controller (/dev/net2)\n"
                      "MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n"
                      "Link: %s (10000 Mbps Full-Duplex)\n"
                      "RX Packets: %u, TX Packets: %u\n",
                      g_vmxnet3_dev.mac[0], g_vmxnet3_dev.mac[1],
                      g_vmxnet3_dev.mac[2], g_vmxnet3_dev.mac[3],
                      g_vmxnet3_dev.mac[4], g_vmxnet3_dev.mac[5],
                      g_vmxnet3_dev.link_up ? "UP" : "DOWN",
                      g_vmxnet3_dev.rx_packets, g_vmxnet3_dev.tx_packets);

    if (len > (size_t)n) len = (size_t)n;
    memcpy(buf, status, len);
    *offset += len;
    return (s64)len;
}

static file_operations_t g_vmxnet3_fops = {
    .read  = vmxnet3_read,
    .write = NULL,
    .ioctl = NULL,
};

/* ── PCI Probe / Driver Model ────────────────────────────────────────────── */

static int vmxnet3_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    /* BAR0 contains MMIO registers */
    phys_addr_t bar0_phys = (phys_addr_t)(info->bar[0] & ~0xFULL);
    if (!bar0_phys) {
        pr_debug("[VMXNET3] Invalid BAR0 on PCI device\n");
        return -ENXIO;
    }

    /* Map MMIO window (64 KB) */
    size_t mmio_size = 0x10000;
    void *virt_mmio = vmm_map_io(bar0_phys, mmio_size);
    if (!virt_mmio) {
        pr_debug("[VMXNET3] Failed to map MMIO registers\n");
        return -ENOMEM;
    }

    pci_enable_bus_mastering(dev->hal);

    g_vmxnet3_dev.mmio_base = (uintptr_t)virt_mmio;
    g_vmxnet3_dev.mmio_size = mmio_size;
    g_vmxnet3_dev.irq       = info->interrupt_line;
    g_vmxnet3_dev.rx_packets = 0;
    g_vmxnet3_dev.tx_packets = 0;
    g_vmxnet3_dev.link_up    = true;

    /* Verify device version */
    u32 ver = vmxnet3_read32(VMXNET3_REG_VRRS);
    if (ver == 0 || ver == 0xFFFFFFFF) {
        pr_debug("[VMXNET3] Device version check failed (0x%08X)\n", ver);
        return -ENODEV;
    }

    /* Select Version 1 and UPT 1 */
    vmxnet3_write32(VMXNET3_REG_VRRS, 1);
    vmxnet3_write32(VMXNET3_REG_UVRS, 1);

    /* Query hardware MAC address */
    vmxnet3_write32(VMXNET3_REG_CMD, VMXNET3_CMD_GET_MAC_ADDR);
    u32 mac_l = vmxnet3_read32(VMXNET3_REG_MAC_L);
    u32 mac_h = vmxnet3_read32(VMXNET3_REG_MAC_H);

    g_vmxnet3_dev.mac[0] = (u8)(mac_l & 0xFF);
    g_vmxnet3_dev.mac[1] = (u8)((mac_l >> 8) & 0xFF);
    g_vmxnet3_dev.mac[2] = (u8)((mac_l >> 16) & 0xFF);
    g_vmxnet3_dev.mac[3] = (u8)((mac_l >> 24) & 0xFF);
    g_vmxnet3_dev.mac[4] = (u8)(mac_h & 0xFF);
    g_vmxnet3_dev.mac[5] = (u8)((mac_h >> 8) & 0xFF);

    /* Allocate and initialize shared driver memory */
    phys_addr_t shared_phys = pmm_alloc_page();
    if (!shared_phys) {
        return -ENOMEM;
    }
    g_vmxnet3_dev.shared_phys = shared_phys;
    g_vmxnet3_dev.shared      = (vmxnet3_driver_shared_t *)PHYS_TO_VIRT(shared_phys);
    memset(g_vmxnet3_dev.shared, 0, 4096);

    g_vmxnet3_dev.shared->magic            = VMXNET3_MAGIC;
    g_vmxnet3_dev.shared->version          = 1;
    g_vmxnet3_dev.shared->guest_os         = 0x00010000;
    g_vmxnet3_dev.shared->mtu              = 1500;
    g_vmxnet3_dev.shared->num_tx_queues    = 1;
    g_vmxnet3_dev.shared->num_rx_queues    = 1;

    /* Pass physical address of shared block */
    vmxnet3_write32(VMXNET3_REG_DSAL, (u32)shared_phys);
    vmxnet3_write32(VMXNET3_REG_DSAH, (u32)((u64)shared_phys >> 32));

    /* Enable device */
    vmxnet3_write32(VMXNET3_REG_CMD, VMXNET3_CMD_ENABLE_DEV);

    /* Register /dev/net2 */
    devfs_register_device("net2", &g_vmxnet3_fops, &g_vmxnet3_dev);
    g_vmxnet3_ready = true;

    pr_debug("[VMXNET3] VMware Paravirtualized 10-GbE online (MAC %02X:%02X:%02X:%02X:%02X:%02X, /dev/net2)\n",
             g_vmxnet3_dev.mac[0], g_vmxnet3_dev.mac[1],
             g_vmxnet3_dev.mac[2], g_vmxnet3_dev.mac[3],
             g_vmxnet3_dev.mac[4], g_vmxnet3_dev.mac[5]);
    return 0;
}

static const pci_device_id_t g_vmxnet3_pci_ids[] = {
    { PCI_DEVICE(PCI_VENDOR_VMWARE, PCI_DEVICE_VMWARE_VMXNET3) },
    { 0 }
};

static pci_driver_t g_vmxnet3_pci_driver = {
    .drv = {
        .name = "vmxnet3",
    },
    .id_table = g_vmxnet3_pci_ids,
    .probe    = vmxnet3_pci_probe,
    .remove   = NULL,
};

int vmxnet3_init(void)
{
    return pci_driver_register(&g_vmxnet3_pci_driver);
}
