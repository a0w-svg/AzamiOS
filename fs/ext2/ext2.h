/* ============================================================================
 * AzamiOS — Ext2 Filesystem Driver Header
 * File: fs/ext2/ext2.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../drivers/block/block.h"
#include "../vfs.h"

#define EXT2_SUPER_MAGIC 0xEF53

/* Ext2 Superblock (at offset 1024 on disk) */
typedef struct __packed {
    u32 s_inodes_count;
    u32 s_blocks_count;
    u32 s_r_blocks_count;
    u32 s_free_blocks_count;
    u32 s_free_inodes_count;
    u32 s_first_data_block;
    u32 s_log_block_size;
    u32 s_log_frag_size;
    u32 s_blocks_per_group;
    u32 s_frags_per_group;
    u32 s_inodes_per_group;
    u32 s_mtime;
    u32 s_wtime;
    u16 s_mnt_count;
    u16 s_max_mnt_count;
    u16 s_magic;
    u16 s_state;
    u16 s_errors;
    u16 s_minor_rev_level;
    u32 s_lastcheck;
    u32 s_checkinterval;
    u32 s_creator_os;
    u32 s_rev_level;
    u16 s_def_resuid;
    u16 s_def_resgid;
    
    /* Extended superblock fields (if s_rev_level >= 1) */
    u32 s_first_ino;
    u16 s_inode_size;
    u16 s_block_group_nr;
    u32 s_feature_compat;
    u32 s_feature_incompat;
    u32 s_feature_ro_compat;
    u8  s_uuid[16];
    u8  s_volume_name[16];
    char s_last_mounted[64];
    u32 s_algo_bitmap;
    u8  s_prealloc_blocks;
    u8  s_prealloc_dir_blocks;
    u16 s_padding1;
    /* ... rest is ignored for now ... */
} ext2_superblock_t;

/* Ext2 Block Group Descriptor */
typedef struct __packed {
    u32 bg_block_bitmap;
    u32 bg_inode_bitmap;
    u32 bg_inode_table;
    u16 bg_free_blocks_count;
    u16 bg_free_inodes_count;
    u16 bg_used_dirs_count;
    u16 bg_pad;
    u32 bg_reserved[3];
} ext2_bg_descriptor_t;

/* Ext2 Inode */
typedef struct __packed {
    u16 i_mode;
    u16 i_uid;
    u32 i_size;
    u32 i_atime;
    u32 i_ctime;
    u32 i_mtime;
    u32 i_dtime;
    u16 i_gid;
    u16 i_links_count;
    u32 i_blocks;
    u32 i_flags;
    u32 i_osd1;
    u32 i_block[15]; /* 12 direct, 1 indirect, 1 d-indirect, 1 t-indirect */
    u32 i_generation;
    u32 i_file_acl;
    u32 i_dir_acl;
    u32 i_faddr;
    u32 i_osd2[3];
} ext2_inode_t;

/* Ext2 Directory Entry */
typedef struct __packed {
    u32 inode;
    u16 rec_len;
    u8  name_len;
    u8  file_type;
    char name[];
} ext2_dir_entry_t;

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

#define EXT2_ROOT_INO    2

typedef struct ext2_fs_info {
    block_dev_t *bdev;
    ext2_superblock_t *sb;
    u32 block_size;
    u32 inodes_per_group;
    u32 blocks_per_group;
    u32 block_groups_count;
    ext2_bg_descriptor_t *bgdt; /* Block Group Descriptor Table */
    
    u8 **block_bitmaps;
    u8 **inode_bitmaps;
} ext2_fs_info_t;

typedef struct ext2_inode_info {
    u32 i_block[15];
} ext2_inode_info_t;

/** ext2_init() — Register the ext2 filesystem type with the VFS. */
void ext2_init(void);

/* Allocation Primitives */
u32 ext2_alloc_block(ext2_fs_info_t *fs);
void ext2_free_block(ext2_fs_info_t *fs, u32 block);
u32 ext2_alloc_inode(ext2_fs_info_t *fs);
void ext2_free_inode(ext2_fs_info_t *fs, u32 ino);
