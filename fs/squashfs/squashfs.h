/* ============================================================================
 * AzamiOS — SquashFS Filesystem Driver Header
 * File: fs/squashfs/squashfs.h
 *
 * On-disk format: SquashFS 4.0 (little-endian, all fields LE unless noted)
 * Reference: https://dr-emann.github.io/squashfs/squashfs.html
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../../drivers/block/block.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../vfs.h"

/* ── On-disk magic & version ───────────────────────────────────────────────── */
#define SQFS_MAGIC          0x73717368UL   /* "sqsh" LE */
#define SQFS_VERSION_MAJOR  4
#define SQFS_VERSION_MINOR  0

/* ── Compression IDs ───────────────────────────────────────────────────────── */
#define SQFS_COMP_ZLIB   1
#define SQFS_COMP_LZMA   2
#define SQFS_COMP_LZO    3
#define SQFS_COMP_XZ     4
#define SQFS_COMP_LZ4    5
#define SQFS_COMP_ZSTD   6

/* ── Superblock flags ──────────────────────────────────────────────────────── */
#define SQFS_FLAG_NOI          0x0001  /* Inodes uncompressed             */
#define SQFS_FLAG_NOD          0x0002  /* Data blocks uncompressed        */
#define SQFS_FLAG_CHECK        0x0004  /* Unused (old format)             */
#define SQFS_FLAG_NOF          0x0008  /* Fragment table uncompressed     */
#define SQFS_FLAG_NO_FRAG      0x0010  /* Fragments not used              */
#define SQFS_FLAG_ALWAYS_FRAG  0x0020  /* Always use fragments            */
#define SQFS_FLAG_DUPLICATE    0x0040  /* Duplicate checking performed    */
#define SQFS_FLAG_EXPORT       0x0080  /* NFS export table present        */
#define SQFS_FLAG_NOX          0x0100  /* Extended attrs uncompressed     */
#define SQFS_FLAG_NO_XATTR     0x0200  /* No extended attributes          */
#define SQFS_FLAG_COMP_OPT     0x0400  /* Compression options present     */
#define SQFS_FLAG_NOI2         0x0800  /* Inodes 2 uncompressed           */

/* ── Inode types ───────────────────────────────────────────────────────────── */
#define SQFS_ITYPE_DIR        1
#define SQFS_ITYPE_FILE       2
#define SQFS_ITYPE_SYMLINK    3
#define SQFS_ITYPE_BLKDEV     4
#define SQFS_ITYPE_CHRDEV     5
#define SQFS_ITYPE_FIFO       6
#define SQFS_ITYPE_SOCKET     7
#define SQFS_ITYPE_LDIR       8  /* Extended directory */
#define SQFS_ITYPE_LFILE      9  /* Extended regular file */
#define SQFS_ITYPE_LSYMLINK  10
#define SQFS_ITYPE_LBLKDEV   11
#define SQFS_ITYPE_LCHRDEV   12
#define SQFS_ITYPE_LFIFO     13
#define SQFS_ITYPE_LSOCKET   14

/* ── Metadata block constants ──────────────────────────────────────────────── */
#define SQFS_META_SIZE          8192   /* max uncompressed metadata block   */
#define SQFS_META_HEADER_MASK   0x8000 /* MSB set = stored (uncompressed)   */
#define SQFS_META_LEN_MASK      0x7FFF /* remaining bits = compressed size  */

/* ── Data block flags ──────────────────────────────────────────────────────── */
#define SQFS_BLOCK_UNCOMPRESSED 0x01000000UL  /* MSB of block-size u32 entry */

/* ── On-disk superblock (96 bytes) ────────────────────────────────────────── */
typedef struct __packed {
    u32 s_magic;            /* 0x73717368                                 */
    u32 s_inodes;           /* total number of inodes                     */
    u32 s_mkfs_time;        /* creation timestamp                         */
    u32 s_block_size;       /* data block size (power of 2, 4K–1M)        */
    u32 s_fragments;        /* number of entries in the fragment table     */
    u16 s_compression;      /* compression algorithm ID                   */
    u16 s_block_log;        /* log2(block_size)                           */
    u16 s_flags;            /* SQFS_FLAG_* bitmask                        */
    u16 s_no_ids;           /* number of IDs (uid/gid) in the ID table    */
    u16 s_s_major;          /* version major (must be 4)                  */
    u16 s_s_minor;          /* version minor (must be 0)                  */
    u64 s_root_inode;       /* inode reference to the root directory      */
    u64 s_bytes_used;       /* total bytes in the archive                 */
    u64 s_id_table_start;   /* byte offset of the ID table                */
    u64 s_xattr_table_start;/* byte offset of the xattr table             */
    u64 s_inode_table_start;/* byte offset of the inode table             */
    u64 s_directory_table_start; /* byte offset of the directory table    */
    u64 s_fragment_table_start;  /* byte offset of the fragment table      */
    u64 s_lookup_table_start;    /* byte offset of the export lookup table */
} sqfs_superblock_t;

/* ── Inode header (common to all inode types) ──────────────────────────────── */
typedef struct __packed {
    u16 inode_type;
    u16 permissions;
    u16 uid_idx;      /* index into the ID table */
    u16 gid_idx;      /* index into the ID table */
    u32 mtime;
    u32 inode_number;
} sqfs_inode_header_t;

/* ── Basic directory inode (type 1) ────────────────────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 dir_block_start;   /* Start of directory listing in directory table */
    u32 hard_link_count;
    u16 file_size;         /* Real size = file_size - 3 for non-root dirs   */
    u16 block_offset;      /* Offset within the first metadata block        */
    u32 parent_inode;
} sqfs_inode_dir_t;

/* ── Extended directory inode (type 8) ─────────────────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 hard_link_count;
    u32 file_size;
    u32 dir_block_start;
    u32 parent_inode;
    u16 index_count;
    u16 block_offset;
    u32 xattr_idx;
} sqfs_inode_ldir_t;

/* ── Basic regular file inode (type 2) ─────────────────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 blocks_start;      /* Byte offset of first data block in the image  */
    u32 fragment_block_index;
    u32 block_offset;      /* Offset of data within the last fragment block  */
    u32 file_size;
    /* Followed by variable number of u32 block sizes (one per full block) */
} sqfs_inode_file_t;

/* ── Extended regular file inode (type 9) ──────────────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u64 blocks_start;
    u64 file_size;
    u64 sparse;
    u32 hard_link_count;
    u32 fragment_block_index;
    u32 block_offset;
    u32 xattr_idx;
    /* Followed by variable number of u32 block sizes */
} sqfs_inode_lfile_t;

/* ── Symlink inode (types 3 and 10) ────────────────────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 hard_link_count;
    u32 symlink_size;
    /* Followed by symlink_size bytes of target path (no NUL terminator) */
} sqfs_inode_symlink_t;

/* ── Device inode (block / char, types 4/5/11/12) ─────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 hard_link_count;
    u32 device;   /* major<<8 | minor */
} sqfs_inode_dev_t;

/* ── IPC inode (fifo / socket, types 6/7/13/14) ───────────────────────────── */
typedef struct __packed {
    sqfs_inode_header_t hdr;
    u32 hard_link_count;
} sqfs_inode_ipc_t;

/* ── Directory table structures ─────────────────────────────────────────────── */
typedef struct __packed {
    u32 count;          /* Number of entries following minus one         */
    u32 start;          /* Metadata block start containing these inodes  */
    u32 inode_number;   /* Inode number of the first entry               */
} sqfs_dir_header_t;

typedef struct __packed {
    u16 offset;         /* Byte offset of inode within its metadata block */
    s16 inode_delta;    /* inode_number = header.inode_number + inode_delta */
    u16 type;           /* SQFS_ITYPE_* */
    u16 name_size;      /* Length of name minus one                        */
    /* Followed by name_size+1 bytes of entry name (no NUL terminator)    */
} sqfs_dir_entry_t;

/* ── Fragment table entry ───────────────────────────────────────────────────── */
typedef struct __packed {
    u64 start;          /* Byte offset of this fragment block              */
    u32 size;           /* Compressed size; MSB set if uncompressed        */
    u32 _unused;
} sqfs_fragment_entry_t;

/* ── Inode reference (block + offset packed as u64) ─────────────────────────── */
typedef struct {
    u64 block;   /* Byte offset of metadata block relative to inode table start */
    u32 offset;  /* Byte offset of inode within the uncompressed metadata block  */
} sqfs_inode_ref_t;

static inline sqfs_inode_ref_t sqfs_iref_unpack(u64 raw)
{
    sqfs_inode_ref_t r;
    r.block  = (raw >> 16) & 0x0000FFFFFFFFFFFFULL;
    r.offset = (u32)(raw & 0xFFFFu);
    return r;
}

/* ── In-memory filesystem private data ─────────────────────────────────────── */
#define SQFS_META_BUF_SIZE   (SQFS_META_SIZE)          /* 8 KiB              */
#define SQFS_DATA_BUF_SIZE   (1024 * 1024)             /* 1 MiB max block    */

typedef struct sqfs_fs_info {
    block_dev_t    *bdev;

    /* Superblock fields kept in host endian */
    u32             block_size;
    u32             block_log;
    u16             compression;
    u16             flags;
    u32             inodes;
    u32             fragments;
    u64             inode_table;    /* byte offset on device */
    u64             dir_table;      /* byte offset on device */
    u64             frag_table_ptr; /* byte offset of fragment-ptr list  */
    u64             id_table_ptr;   /* byte offset of ID-ptr list        */
    u64             bytes_used;

    /* Root inode reference (unpacked) */
    sqfs_inode_ref_t root_iref;

    /* ID table cache (uid/gid lookup) */
    u32            *id_table;       /* flat array of IDs; NULL until first use */
    u32             id_count;

    /* Fragment table cache */
    sqfs_fragment_entry_t *frag_table; /* NULL until first fragment read */

    /* Pre-allocated scratch buffers (allocated once in sqfs_mount) */
    u8             *meta_buf;    /* SQFS_META_BUF_SIZE bytes  */
    u8             *data_buf;    /* SQFS_DATA_BUF_SIZE bytes  */

    spinlock_t      lock;
} sqfs_fs_info_t;

/* ── squashfs_init() — register "squashfs" with the VFS ───────────────────── */
void squashfs_init(void);
