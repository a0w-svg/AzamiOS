#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"

/* ─── FAT32 BPB (BIOS Parameter Block) ───────────────────────────────────── */

typedef struct {
    /* DOS 2.0 BPB */
    uint8_t  jump_boot[3];          /* 0   jmp + nop                  */
    char     oem_name[8];           /* 3   OEM identifier             */
    uint16_t bytes_per_sector;      /* 11  bytes per logical sector   */
    uint8_t  sectors_per_cluster;   /* 13  logical sectors per cluster */
    uint16_t reserved_sectors;      /* 14  reserved sector count      */
    uint8_t  fat_count;             /* 16  number of FAT copies       */
    uint16_t root_entry_count;      /* 17  0 for FAT32                */
    uint16_t total_sectors_16;      /* 19  0 for FAT32                */
    uint8_t  media_type;            /* 21                             */
    uint16_t fat_size_16;           /* 22  0 for FAT32                */
    uint16_t sectors_per_track;     /* 24                             */
    uint16_t head_count;            /* 26                             */
    uint32_t hidden_sectors;        /* 28                             */
    uint32_t total_sectors_32;      /* 32                             */
    /* FAT32 extended BPB */
    uint32_t fat_size_32;           /* 36  sectors per FAT            */
    uint16_t ext_flags;             /* 40                             */
    uint16_t fs_version;            /* 42                             */
    uint32_t root_cluster;          /* 44  first cluster of root dir  */
    uint16_t fs_info_sector;        /* 48                             */
    uint16_t backup_boot_sector;    /* 50                             */
    uint8_t  reserved[12];          /* 52                             */
    uint8_t  drive_number;          /* 64                             */
    uint8_t  reserved1;             /* 65                             */
    uint8_t  boot_signature;        /* 66  0x29 if fields below valid */
    uint32_t volume_id;             /* 67                             */
    char     volume_label[11];      /* 71                             */
    char     fs_type[8];            /* 82  "FAT32   "                 */
} __attribute__((packed)) fat32_bpb_t;

/* ─── 8.3 directory entry ─────────────────────────────────────────────────── */

typedef struct {
    char     name[8];               /* 0   padded with spaces         */
    char     ext[3];                /* 8   extension                  */
    uint8_t  attributes;            /* 11                             */
    uint8_t  reserved;              /* 12                             */
    uint8_t  create_time_tenths;    /* 13                             */
    uint16_t create_time;           /* 14                             */
    uint16_t create_date;           /* 16                             */
    uint16_t access_date;           /* 18                             */
    uint16_t cluster_high;          /* 20  high 16 bits of cluster    */
    uint16_t write_time;            /* 22                             */
    uint16_t write_date;            /* 24                             */
    uint16_t cluster_low;           /* 26  low 16 bits of cluster     */
    uint32_t file_size;             /* 28                             */
} __attribute__((packed)) fat32_dir_entry_t;

/* ─── Long File Name (LFN) entry ──────────────────────────────────────────── */

typedef struct {
    uint8_t  order;                 /* 0   sequence number (bit6=last) */
    uint16_t name1[5];              /* 1   chars 1–5  (UTF-16LE)      */
    uint8_t  attributes;            /* 11  always 0x0F                */
    uint8_t  type;                  /* 12  always 0                   */
    uint8_t  checksum;              /* 13                             */
    uint16_t name2[6];              /* 14  chars 6–11                 */
    uint16_t cluster_low;           /* 26  always 0                   */
    uint16_t name3[2];              /* 28  chars 12–13                */
} __attribute__((packed)) fat32_lfn_entry_t;

/* ─── File attribute bits ─────────────────────────────────────────────────── */
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F    /* LFN entry mask (all four bits) */

/* ─── FAT32 cluster chain markers ────────────────────────────────────────── */
#define FAT32_EOC           0x0FFFFFF8U  /* end-of-chain threshold  */
#define FAT32_BAD           0x0FFFFFF7U
#define FAT32_FREE          0x00000000U

/* ─── LFN constants ──────────────────────────────────────────────────────── */
#define FAT32_LFN_LAST      0x40    /* bit set on last LFN entry      */
#define FAT32_LFN_CHARS     13      /* UTF-16 chars per LFN entry     */
#define FAT32_LFN_MAX_SEQ   20      /* max LFN entries (260 chars)    */

/* ─── Public API ──────────────────────────────────────────────────────────── */

/**
 * fat32_init – read the BPB, validate the FAT32 signature, and return a
 *              VFS root directory node.
 * @param dev  block device to read from (ATA, floppy, …).
 * @return     pointer to root fs_node_t on success, NULL on failure.
 *
 * The returned node supports: read, readdir, finddir.
 */
fs_node_t *fat32_init(block_device_t *dev);

/* Global root node (set by fat32_init, readable from x86arch.c) */
extern fs_node_t *fat32_root;

#endif /* FAT32_H */
