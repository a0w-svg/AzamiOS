/* ============================================================================
 * AzamiOS — QEMU Paravirtualized Panic (pvpanic) Driver
 * File: drivers/misc/pvpanic.c
 *
 * Provides hypervisor panic notification for QEMU / KVM guest environments.
 * Supports both standard ISA I/O port 0x505 and PCI device 1B36:0011 (MMIO or I/O).
 * When AzamiOS panics, pvpanic_notify() immediately signals the host hypervisor.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "pvpanic.h"
#include "../base/platform.h"
#include "../base/pci_bus.h"
#include "../../fs/vfs.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static u16          g_pvpanic_io_port = 0;
static volatile u8 *g_pvpanic_mmio = NULL;
static u8           g_pvpanic_mask = 0;
static bool         g_pvpanic_ready = false;
static spinlock_t   g_pvpanic_lock = SPINLOCK_INIT;

bool pvpanic_is_present(void)
{
    return g_pvpanic_ready;
}

void pvpanic_notify(u8 event)
{
    if (!g_pvpanic_ready) return;
    if (!(g_pvpanic_mask & event)) return;

    if (g_pvpanic_mmio) {
        *g_pvpanic_mmio = event;
    } else if (g_pvpanic_io_port) {
        outb(g_pvpanic_io_port, event);
    }
}

static u8 pvpanic_read_raw(void)
{
    if (g_pvpanic_mmio) {
        return *g_pvpanic_mmio;
    } else if (g_pvpanic_io_port) {
        return inb(g_pvpanic_io_port);
    }
    return 0;
}

/* ── /dev/pvpanic file operations ────────────────────────────────────────── */

static s64 dev_pvpanic_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_pvpanic_ready || !buf || len == 0) return 0;

    u8 supported = pvpanic_read_raw();
    *(u8 *)buf = supported;
    return 1;
}

static s64 dev_pvpanic_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!g_pvpanic_ready || !buf || len == 0) return 0;

    u8 event = *(const u8 *)buf;
    pvpanic_notify(event);
    return 1;
}

static file_operations_t g_pvpanic_fops = {
    .read    = dev_pvpanic_read,
    .write   = dev_pvpanic_write,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

/* ── Platform Driver (ISA port 0x505) ────────────────────────────────────── */

static int pvpanic_platform_probe(platform_device_t *pdev)
{
    (void)pdev;
    u8 mask = inb(PVPANIC_PORT);

    /* If port float (0xFF) or 0, pvpanic is not present on ISA */
    if (mask == 0xFF || mask == 0x00) {
        return -ENODEV;
    }

    if (!(mask & (PVPANIC_PANICKED | PVPANIC_CRASHLOADED))) {
        return -ENODEV;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_pvpanic_lock);
    g_pvpanic_io_port = PVPANIC_PORT;
    g_pvpanic_mmio = NULL;
    g_pvpanic_mask = mask;
    g_pvpanic_ready = true;
    spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);

    devfs_register_device("pvpanic", &g_pvpanic_fops, NULL);
    pr_debug("[PVPANIC] Paravirtualized panic device ready at port 0x%04x (events=0x%02x, /dev/pvpanic)\n",
             g_pvpanic_io_port, g_pvpanic_mask);

    return 0;
}

static platform_driver_t g_pvpanic_pdrv = {
    .drv   = { .name = "pvpanic" },
    .probe = pvpanic_platform_probe,
};

/* ── PCI Driver (1B36:0011) ──────────────────────────────────────────────── */

static int pvpanic_pci_probe(dm_device_t *dev, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dev);
    if (!info) return -ENODEV;

    u32 bar0 = info->bar[0];
    u8 mask = 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_pvpanic_lock);

    if (bar0 & 1) {
        /* I/O port mode */
        u16 port = (u16)(bar0 & ~0x3);
        mask = inb(port);
        if (mask == 0xFF || mask == 0x00) {
            spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);
            return -ENODEV;
        }
        g_pvpanic_io_port = port;
        g_pvpanic_mmio = NULL;
    } else {
        /* MMIO mode */
        phys_addr_t phys = (phys_addr_t)(bar0 & ~0xF);
        if (!phys) {
            spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);
            return -ENODEV;
        }
        volatile u8 *mapped = (volatile u8 *)vmm_map_io(phys, 0x1000);
        if (!mapped) {
            spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);
            return -ENOMEM;
        }
        mask = *mapped;
        if (mask == 0xFF || mask == 0x00) {
            spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);
            return -ENODEV;
        }
        g_pvpanic_mmio = mapped;
        g_pvpanic_io_port = 0;
    }

    g_pvpanic_mask = mask;
    g_pvpanic_ready = true;
    spinlock_unlock_irqrestore(&g_pvpanic_lock, flags);

    devfs_register_device("pvpanic", &g_pvpanic_fops, NULL);
    dm_set_drvdata(dev, &g_pvpanic_ready);

    pr_debug("[PVPANIC] PCI pvpanic controller online (events=0x%02x, /dev/pvpanic, %s=0x%llx)\n",
             mask, g_pvpanic_mmio ? "MMIO" : "Port",
             (unsigned long long)(g_pvpanic_mmio ? (bar0 & ~0xF) : g_pvpanic_io_port));
    return 0;
}

static const pci_device_id_t pvpanic_pci_ids[] = {
    { PCI_DEVICE(0x1B36, 0x0011) },  /* QEMU pvpanic PCI */
    { 0 }
};

static pci_driver_t g_pvpanic_pci_drv = {
    .drv      = { .name = "pvpanic_pci" },
    .id_table = pvpanic_pci_ids,
    .probe    = pvpanic_pci_probe,
};

void pvpanic_init(void)
{
    platform_driver_register(&g_pvpanic_pdrv);
    pci_driver_register(&g_pvpanic_pci_drv);
}
