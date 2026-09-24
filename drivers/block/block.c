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
#include "../../kernel/lib/string.h"
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
    /* BLKROSET makes a device read-only for everyone, which is exactly what
     * a tool sets it for before examining a disk it must not disturb. */
    if (dev->flags & BLKDEV_RO) return -(s64)EPERM;

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

/* ── ioctl ────────────────────────────────────────────────────────────────
 *
 * A /dev/sda node used to answer no ioctl at all, which is not a small gap:
 * blockdev(8), fdisk, parted, mkfs.*, blkid, losetup, `lsblk` and mount's
 * own device probing all size and describe a disk through these commands,
 * and an unrecognized ioctl comes back -ENOTTY, which most of them read as
 * "this is not a block device" and give up on.
 *
 * Linux's numbering: _IO(0x12, nr) for the plain ones, _IOR/_IOW for the
 * ones that carry a size in the command word.
 */
#define BLK_IOC(nr)         (0x1200u | (nr))
#define BLKROSET            BLK_IOC(93)
#define BLKROGET            BLK_IOC(94)
#define BLKRRPART           BLK_IOC(95)
#define BLKGETSIZE          BLK_IOC(96)
#define BLKFLSBUF           BLK_IOC(97)
#define BLKRASET            BLK_IOC(98)
#define BLKRAGET            BLK_IOC(99)
#define BLKFRASET           BLK_IOC(100)
#define BLKFRAGET           BLK_IOC(101)
#define BLKSECTGET          BLK_IOC(103)
#define BLKSSZGET           BLK_IOC(104)
#define BLKBSZGET           0x80081270u   /* _IOR(0x12,112,size_t) */
#define BLKBSZSET           0x40081271u   /* _IOW(0x12,113,size_t) */
#define BLKGETSIZE64        0x80081272u   /* _IOR(0x12,114,size_t) */
#define BLKDISCARD          BLK_IOC(119)
#define BLKIOMIN            BLK_IOC(120)
#define BLKIOOPT            BLK_IOC(121)
#define BLKALIGNOFF         BLK_IOC(122)
#define BLKPBSZGET          BLK_IOC(123)
#define BLKDISCARDZEROES    BLK_IOC(124)
#define BLKSECDISCARD       BLK_IOC(125)
#define BLKROTATIONAL       BLK_IOC(126)
#define BLKZEROOUT          BLK_IOC(127)
#define BLKGETDISKSEQ       0x80081280u   /* _IOR(0x12,128,u64) */

#define HDIO_GETGEO         0x0301u

/* struct hd_geometry, as <linux/hdreg.h> lays it out on x86_64. */
struct hd_geometry {
    u8  heads;
    u8  sectors;
    u16 cylinders;
    unsigned long start;
};

/* The readahead figure BLKRAGET reports, in 512-byte sectors. There is no
 * readahead machinery to configure here, so BLKRASET records the request and
 * BLKRAGET gives it back — which is what the tools that set it check. */
static u32 g_block_readahead = 256;

extern int copy_to_user(void *dst, const void *src, size_t size);
extern int copy_from_user(void *dst, const void *src, size_t size);

static s64 blk_put_int(u64 arg, int v)
{
    return copy_to_user((void *)arg, &v, sizeof(v)) == 0 ? 0 : -(s64)EFAULT;
}

static s64 blk_put_ulong(u64 arg, unsigned long v)
{
    return copy_to_user((void *)arg, &v, sizeof(v)) == 0 ? 0 : -(s64)EFAULT;
}

static s64 blk_put_u64(u64 arg, u64 v)
{
    return copy_to_user((void *)arg, &v, sizeof(v)) == 0 ? 0 : -(s64)EFAULT;
}

/* Erase a byte range by writing zeroes through the normal write path. Backs
 * BLKZEROOUT, and BLKDISCARD on a device whose driver has no real discard
 * (Linux makes the same substitution rather than failing the call). */
static s64 block_zero_range(block_dev_t *dev, u64 start, u64 len)
{
    u32 ss = dev->sector_size;
    if (ss == 0) return -(s64)EINVAL;
    if ((start % ss) || (len % ss)) return -(s64)EINVAL;
    if (!dev->ops || !dev->ops->write_sectors) return -(s64)EOPNOTSUPP;

    u64 lba = start / ss;
    u64 sectors = len / ss;
    if (dev->sector_count && (lba + sectors > dev->sector_count)) return -(s64)EINVAL;

    u32 chunk = (u32)(BLOCK_MAX_XFER / ss);
    if (chunk == 0) chunk = 1;
    void *zeros = kzalloc((size_t)chunk * ss);
    if (!zeros) return -(s64)ENOMEM;

    s64 rc = 0;
    while (sectors) {
        u32 n = (sectors > chunk) ? chunk : (u32)sectors;
        s64 w = dev->ops->write_sectors(dev, lba, n, zeros);
        if (w < 0) { rc = w; break; }
        lba     += n;
        sectors -= n;
    }
    kfree(zeros);
    return rc;
}

static s64 block_fops_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    block_dev_t *dev = (block_dev_t *)filp->private_data;
    if (!dev) return -(s64)ENODEV;

    u32 ss = dev->sector_size ? dev->sector_size : 512;
    u64 bytes = dev->sector_count * (u64)ss;

    switch (cmd) {
    case BLKGETSIZE:
        /* Deliberately 512-byte units regardless of the device's own sector
         * size — that is what this (deprecated but still used) ioctl means. */
        return blk_put_ulong(arg, (unsigned long)(bytes / 512));
    case BLKGETSIZE64:
        return blk_put_u64(arg, bytes);
    case BLKSSZGET:
        return blk_put_int(arg, (int)ss);
    case BLKPBSZGET:
        return blk_put_int(arg, (int)(dev->phys_sector_size ? dev->phys_sector_size : ss));
    case BLKIOMIN:
        return blk_put_int(arg, (int)ss);
    case BLKIOOPT:
        return blk_put_int(arg, 0);          /* no preferred I/O size to declare */
    case BLKALIGNOFF:
        return blk_put_int(arg, 0);          /* partitions here are sector-aligned */
    case BLKBSZGET:
        /* The block-layer soft block size. 4 KiB is the page size and what
         * every filesystem here mounts with. */
        return blk_put_int(arg, 4096);
    case BLKBSZSET: {
        int want = 0;
        if (copy_from_user(&want, (const void *)arg, sizeof(want)) != 0) return -(s64)EFAULT;
        /* Must be a power of two between the sector size and a page. */
        if (want < (int)ss || want > 4096 || (want & (want - 1))) return -(s64)EINVAL;
        return 0;
    }
    case BLKROGET:
        return blk_put_int(arg, (dev->flags & BLKDEV_RO) ? 1 : 0);
    case BLKROSET: {
        int ro = 0;
        if (copy_from_user(&ro, (const void *)arg, sizeof(ro)) != 0) return -(s64)EFAULT;
        if (ro) dev->flags |= BLKDEV_RO;
        else    dev->flags &= ~BLKDEV_RO;
        return 0;
    }
    case BLKROTATIONAL:
        return blk_put_int(arg, (dev->flags & BLKDEV_ROTATIONAL) ? 1 : 0);
    case BLKDISCARDZEROES:
        /* block_zero_range() is the discard fallback, and a real .trim is
         * not required to zero anything, so the honest answer is 0. */
        return blk_put_int(arg, 0);
    case BLKSECTGET:
        return blk_put_int(arg, (int)(BLOCK_MAX_XFER / ss));
    case BLKRAGET:
    case BLKFRAGET:
        return blk_put_ulong(arg, g_block_readahead);
    case BLKRASET:
    case BLKFRASET:
        g_block_readahead = (u32)arg;    /* value, not a pointer, for these two */
        return 0;
    case BLKFLSBUF:
        return block_dev_flush(dev);
    case BLKGETDISKSEQ:
        /* No disk-sequence counter is maintained; the device number is
         * stable and unique for the life of the device, which is the
         * property callers of this actually rely on. */
        return blk_put_u64(arg, dev->rdev);
    case BLKDISCARD:
    case BLKSECDISCARD: {
        u64 range[2];
        if (copy_from_user(range, (const void *)arg, sizeof(range)) != 0) return -(s64)EFAULT;
        if (dev->flags & BLKDEV_RO) return -(s64)EPERM;
        if (range[1] == 0) return 0;
        if ((range[0] % ss) || (range[1] % ss)) return -(s64)EINVAL;
        if (range[0] + range[1] > bytes) return -(s64)EINVAL;
        if (dev->ops && dev->ops->trim) {
            s64 rc = block_dev_trim(dev, range[0] / ss, (u32)(range[1] / ss));
            if (rc >= 0) return 0;
        }
        /* BLKSECDISCARD promises the data is gone, so it cannot fall back to
         * an advisory trim that may do nothing — overwrite instead. */
        return block_zero_range(dev, range[0], range[1]);
    }
    case BLKZEROOUT: {
        u64 range[2];
        if (copy_from_user(range, (const void *)arg, sizeof(range)) != 0) return -(s64)EFAULT;
        if (dev->flags & BLKDEV_RO) return -(s64)EPERM;
        if (range[1] == 0) return 0;
        if (range[0] + range[1] > bytes) return -(s64)EINVAL;
        return block_zero_range(dev, range[0], range[1]);
    }
    case BLKRRPART: {
        /* Re-read the partition table. Only meaningful on a whole disk. */
        if (dev->parent) return -(s64)EINVAL;
        block_drop_partitions(dev);
        block_scan_partitions(dev);
        return 0;
    }
    case HDIO_GETGEO: {
        /* No disk here reports a real CHS geometry, and nothing has needed
         * one since LBA. Report the conventional 255/63 translation fdisk
         * assumes, with the cylinder count derived from the real capacity —
         * which is what fdisk and parted actually use the answer for. */
        struct hd_geometry geo;
        geo.heads     = 255;
        geo.sectors   = 63;
        u64 cyl = dev->sector_count / (255ull * 63ull);
        geo.cylinders = (u16)(cyl > 0xFFFFull ? 0xFFFFull : cyl);
        geo.start     = (unsigned long)dev->start_lba;
        return copy_to_user((void *)arg, &geo, sizeof(geo)) == 0 ? 0 : -(s64)EFAULT;
    }
    default:
        return -(s64)ENOTTY;
    }
}

static file_operations_t block_fops = {
    .read  = block_fops_read,
    .write = block_fops_write,
    .ioctl = block_fops_ioctl,
};

s64 block_dev_register(block_dev_t *dev)
{
    if (!dev || !dev->ops || dev->sector_size == 0) return -(s64)EINVAL;

    spinlock_lock(&g_block_lock);
    dev->next = g_block_devices;
    g_block_devices = dev;
    spinlock_unlock(&g_block_lock);

    /* Register with devfs, then read back the device number it assigned —
     * a filesystem mounted from this device (fs/ext2, fs/squashfs,
     * fs/fat32) uses it as the superblock's st_dev, matching what
     * stat("/dev/<name>") already reports for the same device. */
    devfs_register_block_device(dev->name, &block_fops, dev);
    dev->rdev = devfs_get_rdev(dev->name);
    /* Give the /dev node a real st_size. Without it lseek(SEEK_END) on the
     * device lands at 0 and every tool that sizes a disk that way sees an
     * empty one. */
    devfs_set_size(dev->name, dev->sector_count * (u64)dev->sector_size);

    pr_debug("[BLOCK] Registered block device '%s' (%llu sectors, %u B/sec)\n",
            dev->name, (unsigned long long)dev->sector_count, dev->sector_size);

    /* Automatically probe for MBR partitions on raw disks */
    extern void block_scan_partitions(block_dev_t *parent);
    block_scan_partitions(dev);

    return 0;
}

s64 block_dev_unregister(block_dev_t *dev)
{
    if (!dev) return -(s64)EINVAL;

    spinlock_lock(&g_block_lock);
    block_dev_t **link = &g_block_devices;
    bool found = false;
    while (*link) {
        if (*link == dev) { *link = dev->next; found = true; break; }
        link = &(*link)->next;
    }
    spinlock_unlock(&g_block_lock);
    if (!found) return -(s64)ENODEV;

    devfs_unregister_device(dev->name);
    pr_debug("[BLOCK] Unregistered block device '%s'\n", dev->name);

    /* The devfs inode outlives this and keeps pointing at `dev` through
     * i_private, so it must not become a dangling pointer for anything that
     * still has the device open. Clearing ->ops is what makes the fops fail
     * cleanly (-1 / -ENODEV) instead of jumping through freed memory.
     *
     * driver_data is deliberately left alone: it belongs to whoever created
     * the device and only that code knows whether it is a plain allocation
     * (a partition's mapping) or a structure with other references into it. */
    dev->ops = NULL;
    kfree(dev);
    return 0;
}

void block_drop_partitions(block_dev_t *parent)
{
    if (!parent) return;
    for (;;) {
        block_dev_t *victim = NULL;
        spinlock_lock(&g_block_lock);
        for (block_dev_t *c = g_block_devices; c; c = c->next) {
            if (c->parent == parent) { victim = c; break; }
        }
        spinlock_unlock(&g_block_lock);
        if (!victim) break;
        /* partition.c owns this allocation; see block_dev_unregister(). */
        void *pdata = victim->driver_data;
        block_dev_unregister(victim);
        kfree(pdata);
    }
}

block_dev_t *block_dev_first(void)
{
    spinlock_lock(&g_block_lock);
    block_dev_t *d = g_block_devices;
    spinlock_unlock(&g_block_lock);
    return d;
}

block_dev_t *block_dev_next(block_dev_t *dev)
{
    return dev ? dev->next : NULL;
}

/* /proc/partitions: "major minor #blocks name", #blocks in 1 KiB units.
 *
 * The registry is a prepend-only list, so walking it directly would print
 * the devices in reverse registration order and interleave partitions with
 * the disks they belong to. Print each whole disk followed by its own
 * partitions, which is the ordering every parser of this file expects. */
static size_t blk_emit_row(char *buf, size_t max, const block_dev_t *d)
{
    u64 kb = (d->sector_count * (u64)d->sector_size) / 1024;
    return (size_t)scnprintf(buf, max, "%4u %7u %10llu %s\n",
                             (unsigned)MAJOR(d->rdev), (unsigned)MINOR(d->rdev),
                             (unsigned long long)kb, d->name);
}

size_t block_format_proc_partitions(char *buf, size_t max)
{
    if (!buf || max == 0) return 0;
    size_t off = 0;

    spinlock_lock(&g_block_lock);
    for (block_dev_t *d = g_block_devices; d && off + 1 < max; d = d->next) {
        if (d->parent) continue;                    /* partitions come below */
        off += blk_emit_row(buf + off, max - off, d);
        for (block_dev_t *p = g_block_devices; p && off + 1 < max; p = p->next) {
            if (p->parent == d) off += blk_emit_row(buf + off, max - off, p);
        }
    }
    spinlock_unlock(&g_block_lock);
    return off;
}

block_dev_t *block_dev_get(const char *name)
{
    if (!name) return NULL;

    /* Strip optional /dev/ prefix */
    if (name[0] == '/' && name[1] == 'd' && name[2] == 'e' && name[3] == 'v' && name[4] == '/') {
        name += 5;
    }

    /* Translate sda / sda1 / sda2 aliases to sata0 / sata0p1 / sata0p2 */
    const char *lookup_name = name;
    if (name[0] == 's' && name[1] == 'd' && name[2] == 'a') {
        if (name[3] == '\0') lookup_name = "sata0";
        else if (name[3] == '1' && name[4] == '\0') lookup_name = "sata0p1";
        else if (name[3] == '2' && name[4] == '\0') lookup_name = "sata0p2";
        else if (name[3] == '3' && name[4] == '\0') lookup_name = "sata0p3";
    }

    spinlock_lock(&g_block_lock);
    block_dev_t *curr = g_block_devices;
    while (curr) {
        bool match = true;
        for (int i = 0; lookup_name[i] || curr->name[i]; i++) {
            if (lookup_name[i] != curr->name[i]) {
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
    dev->flags = 0;   /* RAM: not rotational, not removable, writable */

    block_dev_register(dev);
    return dev;
}
