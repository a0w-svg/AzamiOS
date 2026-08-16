/* ============================================================================
 * AzamiOS — FAT32 Filesystem Driver Header
 * File: fs/fat32.h
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "vfs.h"
#include "../drivers/block/block.h"

/* BIOS Parameter Block (BPB) / Boot Sector for FAT32 */
typedef struct __packed {
    u8  jmp_boot[3];       /* 0x00: Jump instruction to boot code */
    char oem_name[8];      /* 0x03: OEM Name identifier ("AZAMIOS ") */
    u16 bytes_per_sector;  /* 0x0B: Bytes per logical sector (usually 512) */
    u8  sectors_per_cluster;/* 0x0D: Sectors per allocation cluster */
    u16 reserved_sectors;  /* 0x0E: Reserved sector count (including boot sector) */
    u8  fat_count;         /* 0x10: Number of File Allocation Tables (usually 2) */
    u16 root_entries;      /* 0x11: Must be 0 for FAT32 */
    u16 total_sectors_16;  /* 0x13: Must be 0 for FAT32 (see total_sectors_32) */
    u8  media_type;        /* 0x15: Media descriptor byte (0xF8 for hard disks) */
    u16 fat_size_16;       /* 0x16: Must be 0 for FAT32 (see fat_size_32) */
    u16 sectors_per_track; /* 0x18: Sectors per track */
    u16 head_count;        /* 0x1A: Number of heads */
    u32 hidden_sectors;    /* 0x1C: Hidden sector count */
    u32 total_sectors_32;  /* 0x20: Total sectors on volume */

    /* FAT32-Specific Fields (starting at offset 0x24) */
    u32 fat_size_32;       /* 0x24: Sectors occupied by one FAT table */
    u16 ext_flags;         /* 0x28: Extended FAT flags */
    u16 fs_version;        /* 0x2A: Filesystem version (high=major, low=minor) */
    u32 root_cluster;      /* 0x2C: First cluster of root directory (usually 2) */
    u16 fs_info_sector;    /* 0x30: Sector number of FSInfo structure */
    u16 backup_boot_sector;/* 0x32: Sector number of backup boot sector */
    u8  reserved_boot[12]; /* 0x34: Reserved space */
    u8  drive_number;      /* 0x40: BIOS drive number */
    u8  reserved_nt;       /* 0x41: Reserved */
    u8  boot_sig;          /* 0x42: Extended boot signature (0x29) */
    u32 vol_id;            /* 0x43: Volume ID serial number */
    char vol_label[11];    /* 0x47: Volume label string */
    char fs_type[8];       /* 0x52: Filesystem type string ("FAT32   ") */
} fat32_bpb_t;

BUILD_ASSERT(sizeof(fat32_bpb_t) == 90, "fat32_bpb_t must be 90 bytes");

/* Standard 32-byte FAT Directory Entry */
typedef struct __packed {
    char name[8];          /* 0x00: Short file name (8 chars, padded with spaces) */
    char ext[3];           /* 0x08: File extension (3 chars, padded with spaces) */
    u8   attr;             /* 0x0B: File attributes (read-only, hidden, dir, etc.) */
    u8   reserved_nt;      /* 0x0C: Reserved for Windows NT */
    u8   create_time_tenth;/* 0x0D: Creation time in tenths of a second */
    u16  create_time;      /* 0x0E: Creation time */
    u16  create_date;      /* 0x10: Creation date */
    u16  access_date;      /* 0x12: Last access date */
    u16  cluster_high;     /* 0x14: High 16 bits of first cluster number */
    u16  mod_time;         /* 0x16: Last modification time */
    u16  mod_date;         /* 0x18: Last modification date */
    u16  cluster_low;      /* 0x1A: Low 16 bits of first cluster number */
    u32  file_size;        /* 0x1C: File size in bytes */
} fat32_dirent_t;

BUILD_ASSERT(sizeof(fat32_dirent_t) == 32, "fat32_dirent_t must be 32 bytes");

/* Attributes */
#define FAT_ATTR_READONLY   0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LONG_NAME  (FAT_ATTR_READONLY | FAT_ATTR_HIDDEN | FAT_ATTR_SYSTEM | FAT_ATTR_VOLUME_ID)

/* Cluster entry values in FAT table */
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_RESERVED  0x0FFFFFF0
#define FAT32_CLUSTER_BAD       0x0FFFFFF7
#define FAT32_CLUSTER_EOF       0x0FFFFFF8

typedef struct {
    block_dev_t *bdev;
    u32 bytes_per_sector;
    u32 sectors_per_cluster;
    u32 bytes_per_cluster;
    u32 first_fat_sector;
    u32 first_data_sector;
    u32 root_cluster;
    u32 total_clusters;
} fat32_volume_t;

/** fat32_format_ramdisk(dev, size_mb) — Format an empty block device as a clean FAT32 volume. */
s64 fat32_format_ramdisk(block_dev_t *dev, u32 size_mb);

/** fat32_mount(dev, mount_point) — Mount a FAT32 filesystem from block device into VFS tree. */
s64 fat32_mount(block_dev_t *dev, const char *mount_point);
