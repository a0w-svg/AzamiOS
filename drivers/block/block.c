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

/* Ceiling on the staging buffer one read()/write() may allocate.  Callers get
 * a short transfer beyond it, which read(2) and write(2) both allow. */
#define BLOCK_MAX_XFER (1U << 20)   /* 1 MiB */

/*
 * Map a byte range onto whole sectors, clamped to something we can allocate
 * and to the end of the device.
 *
 * The sector count and the staging allocation used to be plain 32-bit
 * arithmetic over a caller-supplied length.  A write() of 8 MiB to /dev/hda
 * with 512-byte sectors made `count * dev->sector_size` wrap to a small
 * value, so kzalloc() returned a buffer far smaller than the copy that
 * followed wrote into it — a heap overflow any process holding the device
 * open could trigger.  Nothing bounded the range against sector_count
 * either, so a large offset simply issued reads past the end of the disk.
 *
 * Returns the number of bytes the caller should transfer, or 0 at/after EOF.
 */
static size_t block_map_range(block_dev_t *dev, u64 offset, size_t len,
                              u64 *lba_out, u32 *in_sec_out, u32 *count_out)
{
    u32 ss = dev->sector_size;
    if (ss == 0 || len == 0) return 0;

    u64 lba    = offset / ss;
    u32 in_sec = (u32)(offset % ss);

    if (dev->sector_count && lba >= dev->sector_count) return 0;   /* EOF */

    if (len > BLOCK_MAX_XFER) len = BLOCK_MAX_XFER;

    u64 sectors = ((u64)in_sec + len + ss - 1) / ss;

    if (dev->sector_count) {
        u64 avail = dev->sector_count - lba;
        if (sectors > avail) {
            sectors = avail;
            u64 max_bytes = sectors * ss - in_sec;
            if (len > max_bytes) len = (size_t)max_bytes;
        }
    }
    if (sectors == 0 || len == 0) return 0;

    *lba_out   = lba;
    *in_sec_out = in_sec;
    *count_out = (u32)sectors;
    return len;
}

static s64 block_fops_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    block_dev_t *dev = (block_dev_t *)filp->private_data;
    if (!dev || !dev->ops || !dev->ops->read_sectors) return -1;

    u64 lba; u32 in_sector_offset, count;
    size_t xfer = block_map_range(dev, *offset, len, &lba, &in_sector_offset, &count);
    if (xfer == 0) return 0;

    void *sec_buf = kzalloc((size_t)count * dev->sector_size);
    if (!sec_buf) return -(s64)ENOMEM;

    s64 ret = dev->ops->read_sectors(dev, lba, count, sec_buf);
    if (ret > 0) {
        s64 copy_len = ret - (s64)in_sector_offset;
        if (copy_len > (s64)xfer) copy_len = (s64)xfer;

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

    u64 lba; u32 in_sector_offset, count;
    size_t xfer = block_map_range(dev, *offset, len, &lba, &in_sector_offset, &count);
    if (xfer == 0) return 0;

    void *sec_buf = kzalloc((size_t)count * dev->sector_size);
    if (!sec_buf) return -(s64)ENOMEM;

    /* Read-modify-write if unaligned or partial sector write */
    if (dev->ops->read_sectors && (in_sector_offset != 0 || xfer % dev->sector_size != 0)) {
        dev->ops->read_sectors(dev, lba, count, sec_buf);
    }

    __builtin_memcpy((u8*)sec_buf + in_sector_offset, buf, xfer);

    s64 ret = dev->ops->write_sectors(dev, lba, count, sec_buf);
    if (ret > 0) {
        *offset += xfer;
        ret = (s64)xfer;
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

s64 block_dev_flush(block_dev_t *dev)
{
    if (!dev) return -(s64)EINVAL;
    if (!dev->ops || !dev->ops->flush) return 0;   /* nothing to ask it to do */
    return dev->ops->flush(dev);
}

s64 block_dev_trim(block_dev_t *dev, u64 lba, u32 count)
{
    if (!dev || count == 0) return -(s64)EINVAL;
    if (dev->sector_count && lba + count > dev->sector_count) return -(s64)EINVAL;
    if (!dev->ops || !dev->ops->trim) return 0;   /* advisory only: nothing to ask it to do */
    return dev->ops->trim(dev, lba, count);
}

void block_dev_flush_all(void)
{
    /* g_block_devices is append-(prepend-)only — nothing ever unregisters a
     * block device — so walking it after a single lock/unlock is safe even
     * if another device registers concurrently; that new node just would
     * not be in this snapshot's chain yet. */
    spinlock_lock(&g_block_lock);
    block_dev_t *curr = g_block_devices;
    spinlock_unlock(&g_block_lock);

    for (; curr; curr = curr->next) {
        s64 ret = block_dev_flush(curr);
        if (ret < 0) {
            pr_debug("[BLOCK] flush of '%s' failed: %lld\n", curr->name, (long long)ret);
        }
    }
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
