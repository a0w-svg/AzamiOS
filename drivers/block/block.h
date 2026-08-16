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

/** block_ramdisk_init(phys_base, size) — Create and register a RAM disk (ram0). */
block_dev_t *block_ramdisk_init(phys_addr_t phys_base, size_t size);

/** block_ahci_init() — Initialize AHCI controller and register sata/ahci block devices. */
void block_ahci_init(void);
