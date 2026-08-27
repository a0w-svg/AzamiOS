/* ============================================================================
 * AzamiOS — Block Device Abstraction & Ramdisk Implementation
 * File: drivers/block.c
 *
 * The AHCI/SATA driver lives in its own translation unit (drivers/block/ahci.c);
 * block_ahci_init() is defined there.
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
