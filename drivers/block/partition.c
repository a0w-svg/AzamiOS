/* ============================================================================
 * AzamiOS — MBR Partition Table Scanner & Block Partition Devices
 * File: drivers/block/partition.c
 *
 * Implements MBR partition table scanning for any registered block device.
 * For each valid partition found on a parent disk (e.g. sata0), this creates
 * and registers a child block_dev_t (e.g. sata0p1, sata0p2) with bounds-checked
 * sector offsets mapped directly to the parent disk.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "partition.h"
#include "block.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/syscall/syscall.h"

typedef struct {
    block_dev_t *parent;
    u64          lba_offset;
    u64          sector_count;
} partition_data_t;

static s64 partition_read_sectors(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    if (!dev || !dev->driver_data || !buf) return -(s64)EINVAL;
    partition_data_t *pdata = (partition_data_t *)dev->driver_data;

    if (lba + count > pdata->sector_count) return -(s64)EINVAL;
    if (!pdata->parent || !pdata->parent->ops || !pdata->parent->ops->read_sectors)
        return -(s64)ENODEV;

    return pdata->parent->ops->read_sectors(pdata->parent, pdata->lba_offset + lba, count, buf);
}

static s64 partition_write_sectors(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    if (!dev || !dev->driver_data || !buf) return -(s64)EINVAL;
    partition_data_t *pdata = (partition_data_t *)dev->driver_data;

    if (lba + count > pdata->sector_count) return -(s64)EINVAL;
    if (!pdata->parent || !pdata->parent->ops || !pdata->parent->ops->write_sectors)
        return -(s64)ENODEV;

    return pdata->parent->ops->write_sectors(pdata->parent, pdata->lba_offset + lba, count, buf);
}

static s64 partition_flush(block_dev_t *dev)
{
    if (!dev || !dev->driver_data) return -(s64)EINVAL;
    partition_data_t *pdata = (partition_data_t *)dev->driver_data;

    if (!pdata->parent || !pdata->parent->ops || !pdata->parent->ops->flush)
        return 0;

    return pdata->parent->ops->flush(pdata->parent);
}

static s64 partition_trim(block_dev_t *dev, u64 lba, u32 count)
{
    if (!dev || !dev->driver_data || count == 0) return -(s64)EINVAL;
    partition_data_t *pdata = (partition_data_t *)dev->driver_data;

    if (lba + count > pdata->sector_count) return -(s64)EINVAL;
    if (!pdata->parent || !pdata->parent->ops || !pdata->parent->ops->trim)
        return 0;

    return pdata->parent->ops->trim(pdata->parent, pdata->lba_offset + lba, count);
}

static block_ops_t g_partition_ops = {
    .read_sectors  = partition_read_sectors,
    .write_sectors = partition_write_sectors,
    .flush         = partition_flush,
    .trim          = partition_trim
};

void block_scan_partitions(block_dev_t *parent)
{
    if (!parent || !parent->ops || !parent->ops->read_sectors) return;
    if (parent->sector_count < 2) return;

    /* Do not recursively scan partition devices */
    for (int i = 0; parent->name[i]; i++) {
        if (parent->name[i] == 'p' && i > 0 && parent->name[i-1] >= '0' && parent->name[i-1] <= '9') {
            return;
        }
    }
    if (strncmp(parent->name, "loop", 4) == 0 || strncmp(parent->name, "ram", 3) == 0) {
        return;
    }

    size_t sec_size = parent->sector_size ? parent->sector_size : 512;
    u8 *sector = (u8 *)kzalloc(sec_size);
    if (!sector) return;

    s64 ret = parent->ops->read_sectors(parent, 0, 1, sector);
    if (ret < 512) {
        kfree(sector);
        return;
    }

    u16 sig = (u16)sector[510] | ((u16)sector[511] << 8);
    if (sig != MBR_SIGNATURE) {
        kfree(sector);
        return;
    }

    int part_count = 0;
    for (int i = 0; i < 4; i++) {
        mbr_entry_t *entry = (mbr_entry_t *)(sector + 446 + i * sizeof(mbr_entry_t));
        if (entry->type == 0 || entry->sector_count == 0) continue;

        if (parent->sector_count > 0 && (u64)entry->lba_start + entry->sector_count > parent->sector_count) {
            pr_debug("[PARTITION] Warning: partition %d on %s exceeds disk boundaries\n", i + 1, parent->name);
            continue;
        }

        partition_data_t *pdata = (partition_data_t *)kzalloc(sizeof(partition_data_t));
        if (!pdata) continue;

        pdata->parent       = parent;
        pdata->lba_offset   = entry->lba_start;
        pdata->sector_count = entry->sector_count;

        block_dev_t *part_dev = (block_dev_t *)kzalloc(sizeof(block_dev_t));
        if (!part_dev) {
            kfree(pdata);
            continue;
        }

        snprintf(part_dev->name, sizeof(part_dev->name), "%sp%d", parent->name, i + 1);
        part_dev->sector_size  = parent->sector_size;
        part_dev->phys_sector_size = parent->phys_sector_size;
        part_dev->sector_count = entry->sector_count;
        part_dev->start_lba    = entry->lba_start;
        part_dev->parent       = parent;
        part_dev->flags        = parent->flags;
        part_dev->ops          = &g_partition_ops;
        part_dev->driver_data  = pdata;

        block_dev_register(part_dev);
        part_count++;

        u32 size_mb = (u32)(((u64)entry->sector_count * parent->sector_size) / (1024 * 1024));
        pr_debug("[PARTITION] %s: MBR part %d (type 0x%02x, %s) LBA %u..%u (%u MB)\n",
                 part_dev->name, i + 1, entry->type,
                 (entry->status & 0x80) ? "bootable" : "data",
                 entry->lba_start, entry->lba_start + entry->sector_count - 1, size_mb);
    }

    kfree(sector);
}
