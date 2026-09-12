/* ============================================================================
 * AzamiOS — Block Device Abstraction Header
 * File: drivers/block/block.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

struct block_dev;

typedef struct block_ops {
    s64 (*read_sectors)(struct block_dev *dev, u64 lba, u32 count, void *buf);
    s64 (*write_sectors)(struct block_dev *dev, u64 lba, u32 count, const void *buf);
    /* flush(dev) — force any volatile write cache the device itself holds
     * out to stable media. Optional: a driver whose writes are already
     * synchronous at the hardware level (or that has no cache to speak of,
     * e.g. a ramdisk) can leave this NULL, and block_dev_flush() treats a
     * NULL flush as trivially successful rather than an error. Without
     * this, write_sectors() completing was never a guarantee the target
     * disk had actually committed the data — only that the *driver* had. */
    s64 (*flush)(struct block_dev *dev);
    /* trim(dev, lba, count) — advise the device that these sectors no
     * longer hold live data (ATA DATA SET MANAGEMENT/TRIM, NVMe Deallocate,
     * SCSI UNMAP, ...), so it can reclaim them internally instead of
     * treating them as data worth preserving. Purely an optimization hint:
     * optional like .flush, and block_dev_trim() treats a NULL trim the
     * same way — a device untouched by this call still behaves correctly,
     * it just cannot benefit from knowing the range is free. */
    s64 (*trim)(struct block_dev *dev, u64 lba, u32 count);
} block_ops_t;

typedef struct block_dev {
    char        name[32];
    u32         sector_size;   /* Typically 512 bytes */
    u64         sector_count;  /* Total sectors on device */
    block_ops_t *ops;
    void        *driver_data;  /* Driver-specific private data (e.g., Ramdisk base or AHCI port) */
    struct block_dev *next;
} block_dev_t;

/** block_dev_init() — Initialize block device registry. */
void block_dev_init(void);

/** block_dev_register(dev) — Register a new block device. */
s64 block_dev_register(block_dev_t *dev);

/** block_dev_get(name) — Find a registered block device by name. */
block_dev_t *block_dev_get(const char *name);

/** block_dev_flush(dev) — Ask the device to commit any volatile write cache
 *  to stable media. 0 on success (including a driver with no .flush op —
 *  there is nothing more to ask it to do), a negative errno on failure. */
s64 block_dev_flush(block_dev_t *dev);

/** block_dev_flush_all() — block_dev_flush() every registered device.
 *  Called from vfs_sync_all()/vfs_sync_fs(): those already write back this
 *  kernel's own dirty buffers, but that write completing is not the same
 *  guarantee as the disk's own cache having committed it — this closes
 *  that gap. Failures are logged, not propagated: one wedged disk should
 *  not stop sync(2) from flushing every other one. */
void block_dev_flush_all(void);

/** block_dev_trim(dev, lba, count) — Advise the device that [lba, lba+count)
 *  no longer holds live data. 0 on success (including a driver with no
 *  .trim op — there is nothing more to ask it to do), a negative errno on
 *  a real failure (bad range, hardware error). */
s64 block_dev_trim(block_dev_t *dev, u64 lba, u32 count);

/** block_ramdisk_init(phys_base, size) — Create and register a RAM disk (ram0). */
block_dev_t *block_ramdisk_init(phys_addr_t phys_base, size_t size);

/** block_ahci_init() — Initialize AHCI controller and register sata/ahci block devices. */
void block_ahci_init(void);
