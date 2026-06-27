/**
 * kernel/hal/hal_block.h  –  Block Device Hardware Abstraction Layer
 *
 * Defines the interface contract used by lib/fs/ (FAT32, tarfs, VFS) so
 * that filesystem code compiles independently of ATA registers, DMA, or
 * any other kernel-specific block driver.
 *
 * Each concrete driver (ATA, floppy, RAM disk, …) allocates a
 * hal_block_dev_t and registers it with the VFS at boot.
 *
 * NOTE: This HAL is complementary to block_device_t in vfs.h.
 *       vfs.h's block_device_t already uses function pointers; this file
 *       documents the same contract explicitly for clarity and to serve as
 *       the single authoritative definition for new drivers.
 */
#ifndef HAL_BLOCK_H
#define HAL_BLOCK_H

#include <stdint.h>

typedef struct hal_block_dev {
    /** Human-readable device name (e.g. "ata0", "floppy0", "initrd"). */
    char name[32];

    /** Block size in bytes (typically 512). */
    uint32_t block_size;

    /**
     * read – read count blocks starting at lba into buffer.
     * @return number of blocks actually read; 0 on error.
     */
    uint32_t (*read)(struct hal_block_dev *dev,
                     uint32_t lba, uint32_t count, void *buffer);

    /**
     * write – write count blocks from buffer to lba.
     * @return number of blocks actually written; 0 on error.
     */
    uint32_t (*write)(struct hal_block_dev *dev,
                      uint32_t lba, uint32_t count, const void *buffer);

    /** Driver-private data (e.g., ATA port base, DMA channel). */
    void *impl_data;
} hal_block_dev_t;

#endif /* HAL_BLOCK_H */
