/* ============================================================================
 * AzamiOS — Partition Table Scanner & Block Partition Devices
 * File: drivers/block/partition.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "block.h"

#define MBR_SIGNATURE 0xAA55

/* MBR Partition Table Entry (16 bytes) */
typedef struct __attribute__((packed)) mbr_entry {
    u8  status;         /* 0x80 = active/bootable, 0x00 = inactive */
    u8  start_chs[3];   /* CHS address of first sector */
    u8  type;           /* Partition type (0x83 = Linux ext2/3/4, 0x0C = FAT32, etc.) */
    u8  end_chs[3];     /* CHS address of last sector */
    u32 lba_start;      /* LBA of first sector in partition */
    u32 sector_count;   /* Number of sectors in partition */
} mbr_entry_t;

/**
 * Scan a parent block device (e.g. sata0, hda) for an MBR partition table.
 * For each valid partition, registers a child block_dev_t (e.g. sata0p1, sata0p2).
 */
void block_scan_partitions(block_dev_t *parent);
