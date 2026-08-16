/* ============================================================================
 * AzamiOS — Block Device Abstraction & Ramdisk/AHCI Implementation
 * File: drivers/block.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "block.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../char/console.h"
#include "../../include/azami/defs.h"
#include "../../kernel/syscall/syscall.h" /* EINVAL, ENODEV */
#include "ahci.h"
#include "../../hal/pci.h"
#include "../../fs/vfs.h"


#define ENODEV  19

static spinlock_t g_block_lock = SPINLOCK_INIT;
static block_dev_t *g_block_devices = NULL;

void block_dev_init(void)
{
    pr_debug("[BLOCK] Block device registry initialized.\n");
}

static s64 block_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    block_dev_t *dev = (block_dev_t *)filp->private_data;
    if (!dev || !dev->ops || !dev->ops->read_sectors) return -1;
    
    u64 lba = (*offset) / dev->sector_size;
    u32 in_sector_offset = (*offset) % dev->sector_size;
    u32 count = (in_sector_offset + len + dev->sector_size - 1) / dev->sector_size;
    
    void *sec_buf = kzalloc(count * dev->sector_size);
    if (!sec_buf) return -(s64)ENOMEM;
    
    s64 ret = dev->ops->read_sectors(dev, lba, count, sec_buf);
    if (ret > 0) {
        s64 copy_len = ret - in_sector_offset;
        if (copy_len > (s64)len) copy_len = len;
        
        if (copy_len > 0) {
            __builtin_memcpy(buf, (u8*)sec_buf + in_sector_offset, copy_len);
            *offset += copy_len;
            ret = copy_len;
        } else {
            ret = 0; /* Read beyond EOF or error */
        }
    }
    
    kfree(sec_buf);
    return ret;
}

static s64 block_fops_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    block_dev_t *dev = (block_dev_t *)filp->private_data;
    if (!dev || !dev->ops || !dev->ops->write_sectors) return -1;
    
    u64 lba = (*offset) / dev->sector_size;
    u32 in_sector_offset = (*offset) % dev->sector_size;
    u32 count = (in_sector_offset + len + dev->sector_size - 1) / dev->sector_size;
    
    void *sec_buf = kzalloc(count * dev->sector_size);
    if (!sec_buf) return -(s64)ENOMEM;
    
    /* Read-modify-write if unaligned or partial sector write */
    if (dev->ops->read_sectors && (in_sector_offset != 0 || len % dev->sector_size != 0)) {
        dev->ops->read_sectors(dev, lba, count, sec_buf);
    }
    
    __builtin_memcpy((u8*)sec_buf + in_sector_offset, buf, len);
    
    s64 ret = dev->ops->write_sectors(dev, lba, count, sec_buf);
    if (ret > 0) {
        *offset += len;
        ret = len;
    }
    
    kfree(sec_buf);
    return ret;
}

static file_operations_t block_fops = {
    .read = block_fops_read,
    .write = block_fops_write,
};

s64 block_dev_register(block_dev_t *dev)
{
    if (!dev || !dev->ops || dev->sector_size == 0) return -(s64)EINVAL;

    spinlock_lock(&g_block_lock);
    dev->next = g_block_devices;
    g_block_devices = dev;
    spinlock_unlock(&g_block_lock);

    /* Register with devfs */
    devfs_register_block_device(dev->name, &block_fops, dev);

    pr_debug("[BLOCK] Registered block device '%s' (%llu sectors, %u B/sec)\n",
            dev->name, (unsigned long long)dev->sector_count, dev->sector_size);
    return 0;
}

block_dev_t *block_dev_get(const char *name)
{
    if (!name) return NULL;

    spinlock_lock(&g_block_lock);
    block_dev_t *curr = g_block_devices;
    while (curr) {
        bool match = true;
        for (int i = 0; name[i] || curr->name[i]; i++) {
            if (name[i] != curr->name[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            spinlock_unlock(&g_block_lock);
            return curr;
        }
        curr = curr->next;
    }
    spinlock_unlock(&g_block_lock);
    return NULL;
}

/* ── RAM Disk (ram0) Driver ──────────────────────────────────────────────── */

typedef struct {
    phys_addr_t phys_base;
    virt_addr_t virt_base;
    size_t      total_size;
} ramdisk_data_t;

static s64 ramdisk_read(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    if (!dev || !dev->driver_data || !buf) return -(s64)EINVAL;
    ramdisk_data_t *data = (ramdisk_data_t *)dev->driver_data;

    u64 offset = lba * dev->sector_size;
    u64 length = (u64)count * dev->sector_size;

    if (offset + length > data->total_size) return -(s64)EINVAL;

    __builtin_memcpy(buf, (const void *)(data->virt_base + offset), (size_t)length);
    return (s64)length;
}

static s64 ramdisk_write(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    if (!dev || !dev->driver_data || !buf) return -(s64)EINVAL;
    ramdisk_data_t *data = (ramdisk_data_t *)dev->driver_data;

    u64 offset = lba * dev->sector_size;
    u64 length = (u64)count * dev->sector_size;

    if (offset + length > data->total_size) return -(s64)EINVAL;

    __builtin_memcpy((void *)(data->virt_base + offset), buf, (size_t)length);
    return (s64)length;
}

static block_ops_t g_ramdisk_ops = {
    .read_sectors = ramdisk_read,
    .write_sectors = ramdisk_write
};

block_dev_t *block_ramdisk_init(phys_addr_t phys_base, size_t size)
{
    if (size == 0) return NULL;

    phys_addr_t actual_phys = phys_base;
    if (actual_phys == 0) {
        /* Allocate memory for ramdisk dynamically if not passed from bootloader */
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        actual_phys = pmm_alloc_pages(pages);
        if (!actual_phys) PANIC("Failed to allocate physical pages for ram0!");
    }

    ramdisk_data_t *data = (ramdisk_data_t *)kzalloc(sizeof(ramdisk_data_t));
    if (!data) return NULL;

    data->phys_base = actual_phys;
    data->virt_base = (virt_addr_t)PHYS_TO_VIRT(actual_phys);
    data->total_size = size;

    /* Zero out newly allocated ramdisk buffer if we allocated it */
    if (phys_base == 0) {
        __builtin_memset((void *)data->virt_base, 0, size);
    }

    block_dev_t *dev = (block_dev_t *)kzalloc(sizeof(block_dev_t));
    if (!dev) {
        kfree(data);
        return NULL;
    }

    dev->name[0] = 'r'; dev->name[1] = 'a'; dev->name[2] = 'm'; dev->name[3] = '0'; dev->name[4] = '\0';
    dev->sector_size = 512;
    dev->sector_count = size / 512;
    dev->ops = &g_ramdisk_ops;
    dev->driver_data = data;

    block_dev_register(dev);
    return dev;
}

/* ── AHCI / SATA Block Device Driver ─────────────────────────────────────── */

typedef struct {
    ahci_port_t *port;
    u64          ahci_base_virt;
    u32          port_idx;
    spinlock_t   lock;
} ahci_data_t;

static int check_port_type(ahci_port_t *port)
{
    u32 ssts = port->ssts;
    u8 ipm = (ssts >> 8) & 0x0F;
    u8 det = ssts & 0x0F;

    if (det != 3) return 0;
    if (ipm != 1) return 0;

    if (port->sig == SATA_SIG_ATA) return AHCI_DEV_SATA;
    if (port->sig == SATA_SIG_ATAPI) return AHCI_DEV_SATAPI;
    if (port->sig == SATA_SIG_SEMB) return AHCI_DEV_SEMB;
    if (port->sig == SATA_SIG_PM) return AHCI_DEV_PM;

    return 0;
}

static void port_start_cmd(ahci_port_t *port)
{
    while (port->cmd & HBA_PxCMD_CR);
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static void port_stop_cmd(ahci_port_t *port)
{
    port->cmd &= ~HBA_PxCMD_ST;
    port->cmd &= ~HBA_PxCMD_FRE;
    while (1) {
        if (port->cmd & HBA_PxCMD_FR) continue;
        if (port->cmd & HBA_PxCMD_CR) continue;
        break;
    }
}

static int ahci_issue_command(ahci_port_t *port)
{
    int spin = 0;
    while ((port->tfd & (0x80 | 0x08)) && spin < 1000000) {
        spin++;
        cpu_pause();
    }
    if (spin == 1000000) return -1;

    port->ci = 1;
    while (1) {
        if ((port->ci & 1) == 0) break;
        if (port->is & HBA_PxIS_TFES) return -1;
    }

    if (port->is & HBA_PxIS_TFES) return -1;
    return 0;
}

static s64 ahci_rw_sectors(block_dev_t *dev, u64 lba, u32 count, void *buf, int write)
{
    if (!dev || !dev->driver_data || !buf) return -(s64)EINVAL;
    ahci_data_t *data = (ahci_data_t *)dev->driver_data;
    ahci_port_t *port = data->port;

    irqflags_t flags = spinlock_lock_irqsave(&data->lock);

    port->is = (u32)-1;
    int slot = 0;
    ahci_cmd_header_t *cmdheader = (ahci_cmd_header_t *)PHYS_TO_VIRT((phys_addr_t)port->clb | ((u64)port->clbu << 32));

    cmdheader[slot].cfl = sizeof(fis_reg_h2d_t) / sizeof(u32);
    cmdheader[slot].w = write ? 1 : 0;
    cmdheader[slot].prdtl = 1;

    ahci_cmd_table_t *cmdtbl = (ahci_cmd_table_t *)PHYS_TO_VIRT((phys_addr_t)cmdheader[slot].ctba | ((u64)cmdheader[slot].ctbau << 32));
    __builtin_memset(cmdtbl, 0, sizeof(ahci_cmd_table_t));

    phys_addr_t phys_buf = VIRT_TO_PHYS((virt_addr_t)buf);
    cmdtbl->prdt_entry[0].dba = (u32)phys_buf;
    cmdtbl->prdt_entry[0].dbau = (u32)(phys_buf >> 32);
    cmdtbl->prdt_entry[0].dbc = (count * dev->sector_size) - 1;
    cmdtbl->prdt_entry[0].dbc |= (1U << 31);

    fis_reg_h2d_t *cmdfis = (fis_reg_h2d_t *)(&cmdtbl->cfis);
    cmdfis->fis_type = FIS_TYPE_REG_H2D;
    cmdfis->c = 1;
    cmdfis->command = write ? ATA_CMD_WRITE_DMA_EX : ATA_CMD_READ_DMA_EX;

    cmdfis->lba0 = (u8)lba;
    cmdfis->lba1 = (u8)(lba >> 8);
    cmdfis->lba2 = (u8)(lba >> 16);
    cmdfis->device = 1 << 6; // LBA mode

    cmdfis->lba3 = (u8)(lba >> 24);
    cmdfis->lba4 = (u8)(lba >> 32);
    cmdfis->lba5 = (u8)(lba >> 40);

    cmdfis->countl = count & 0xFF;
    cmdfis->counth = (count >> 8) & 0xFF;

    s64 result;
    if (ahci_issue_command(port) < 0) {
        result = -(s64)EIO;
    } else {
        result = (s64)(count * dev->sector_size);
    }
    spinlock_unlock_irqrestore(&data->lock, flags);
    return result;
}

static s64 ahci_read(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    return ahci_rw_sectors(dev, lba, count, buf, 0);
}

static s64 ahci_write(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    return ahci_rw_sectors(dev, lba, count, (void*)buf, 1);
}

static block_ops_t g_ahci_ops = {
    .read_sectors = ahci_read,
    .write_sectors = ahci_write
};

static void ahci_port_init(ahci_hba_t *hba, ahci_port_t *port, u32 port_no)
{
    port_stop_cmd(port);

    phys_addr_t clb_phys = pmm_alloc_page();
    port->clb  = (u32)clb_phys;
    port->clbu = (u32)(clb_phys >> 32);
    __builtin_memset((void*)PHYS_TO_VIRT(clb_phys), 0, PAGE_SIZE);

    phys_addr_t fb_phys = pmm_alloc_page();
    port->fb  = (u32)fb_phys;
    port->fbu = (u32)(fb_phys >> 32);
    __builtin_memset((void*)PHYS_TO_VIRT(fb_phys), 0, PAGE_SIZE);

    ahci_cmd_header_t *cmdheader = (ahci_cmd_header_t *)PHYS_TO_VIRT(clb_phys);
    phys_addr_t ctba_phys = pmm_alloc_page();
    cmdheader[0].ctba  = (u32)ctba_phys;
    cmdheader[0].ctbau = (u32)(ctba_phys >> 32);
    __builtin_memset((void*)PHYS_TO_VIRT(ctba_phys), 0, PAGE_SIZE);

    port_start_cmd(port);

    ahci_data_t *data = kzalloc(sizeof(ahci_data_t));
    if (!data) {
        pr_debug("[AHCI] Out of memory allocating driver data for port %u\n", port_no);
        return;
    }
    data->port = port;
    data->port_idx = port_no;
    data->ahci_base_virt = (u64)hba;
    data->lock = (spinlock_t)SPINLOCK_INIT;

    block_dev_t *dev = kzalloc(sizeof(block_dev_t));
    if (!dev) {
        pr_debug("[AHCI] Out of memory allocating block device for port %u\n", port_no);
        kfree(data);
        return;
    }
    dev->name[0] = 's'; dev->name[1] = 'a'; dev->name[2] = 't'; dev->name[3] = 'a';
    dev->name[4] = '0' + (char)port_no; dev->name[5] = '\0';
    dev->sector_size  = 512;
    dev->sector_count = 1000000;
    dev->ops          = &g_ahci_ops;
    dev->driver_data  = data;

    block_dev_register(dev);
    pr_debug("[AHCI] Registered SATA drive %s on port %u\n", dev->name, port_no);
}

static void ahci_scan_device(device_t *dev)
{
    pci_device_info_t *pci = pci_get_device_info(dev);
    if (!pci) return;

    if (pci->class_code == 0x01 && pci->subclass == 0x06) {
        pr_debug("[AHCI] Found controller at %02x:%02x.%x\n", pci->bus, pci->slot, pci->func);

        pci_enable_bus_mastering(dev);
        phys_addr_t abar_phys = pci_get_bar(dev, 5);
        if (abar_phys == 0) return;

        phys_addr_t abar_aligned = ALIGN_DOWN(abar_phys, 4096);
        virt_addr_t abar_virt = (virt_addr_t)PHYS_TO_VIRT(abar_aligned);
        vmm_map(0, abar_virt, abar_aligned, VMM_MMIO);
        vmm_map(0, abar_virt + 4096, abar_aligned + 4096, VMM_MMIO);

        ahci_hba_t *hba = (ahci_hba_t *)((u64)abar_virt + (abar_phys - abar_aligned));
        hba->ghc |= (1U << 31); // AE

        u32 pi = hba->pi;
        for (u32 i = 0; i < 32; i++) {
            if (pi & (1U << i)) {
                int dt = check_port_type(&hba->ports[i]);
                if (dt == AHCI_DEV_SATA) {
                    pr_debug("[AHCI] SATA drive detected on port %u\n", i);
                    ahci_port_init(hba, &hba->ports[i], i);
                } else if (dt == AHCI_DEV_SATAPI) {
                    pr_debug("[AHCI] SATAPI (CD-ROM) drive detected on port %u\n", i);
                    // Just print for now, no SATAPI support in ahci_port_init yet
                } else {
                    pr_debug("[AHCI] Unknown/No device (type %d) on port %u\n", dt, i);
                }
            }
        }
    }
}

static void ahci_scan_tree(device_t *dev)
{
    if (!dev) return;
    ahci_scan_device(dev);
    ahci_scan_tree(dev->children);
    ahci_scan_tree(dev->sibling);
}

void block_ahci_init(void)
{
    pr_debug("[BLOCK] Probing for AHCI SATA controllers via PCI class 0x0106...\n");
    ahci_scan_tree(device_tree_root());
}
