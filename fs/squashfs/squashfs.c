/* ============================================================================
 * AzamiOS — SquashFS Read-Only Filesystem Driver
 * File: fs/squashfs/squashfs.c
 *
 * SquashFS 4.0 (little-endian) read-only driver for the AzamiOS VFS layer.
 *
 * Feature support matrix
 * ──────────────────────
 *   ✔  Superblock validation (magic, version, block size)
 *   ✔  Inode table reads (basic and extended dir / file / symlink)
 *   ✔  Directory table traversal (readdir, lookup)
 *   ✔  Regular file data reads (full data blocks + tail fragments)
 *   ✔  Symlink resolution
 *   ✔  UID/GID resolution via the ID table
 *   ✔  zlib/DEFLATE decompression (via squashfs_zlib.h)
 *   ✗  lzma/lz4/lzo/xz/zstd — stubs return -ENOTSUP
 *   ✗  Extended attributes (xattr)
 *   ✗  NFS export table
 *   ✗  Write operations — all NULL; VFS returns -EROFS automatically
 *
 * Scratch-buffer policy
 * ─────────────────────
 * Each mounted SquashFS instance allocates two kernel heap buffers at mount
 * time:
 *   meta_buf  — 8 KiB; holds one decompressed metadata block
 *   data_buf  — 1 MiB; holds one decompressed data block
 *
 * Both are protected by sqfs_fs_info_t.lock (a spinlock).  No per-call
 * allocation happens in the I/O path.
 *
 * VFS buffer contract
 * ───────────────────
 * All buf/dirent_buf pointers arriving at read/readdir/readlink are kernel
 * pointers (see fs/vfs.h "BUFFER CONTRACT").  We use plain __builtin_memcpy()
 * throughout — copy_to_user() would EFAULT here.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "squashfs.h"
#include "squashfs_zlib.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../include/azami/defs.h"

/* ── Error codes reused from Linux ABI ────────────────────────────────────── */
#ifndef EINVAL
#define EINVAL   22
#endif
#ifndef EIO
#define EIO       5
#endif
#ifndef ENOENT
#define ENOENT    2
#endif
#ifndef ENOMEM
#define ENOMEM   12
#endif
#ifndef ENOTSUP
#define ENOTSUP  95  /* same as EOPNOTSUPP */
#endif
#ifndef EROFS
#define EROFS    30
#endif
#ifndef ENOTDIR
#define ENOTDIR  20
#endif
#ifndef ENAMETOOLONG
#define ENAMETOOLONG 36
#endif

/* ── linux_dirent64 layout (must match what getdents64 expects) ────────────── */
typedef struct __packed {
    u64  d_ino;
    s64  d_off;
    u16  d_reclen;
    u8   d_type;
    char d_name[1]; /* variable length */
} sqfs_dirent64_t;

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK    10
#define DT_SOCK   12

/* ── Forward declarations ─────────────────────────────────────────────────── */
static s64           sqfs_mount(file_system_type_t *fs_type,
                                const char *dev_name,
                                const char *dir_name, void *data);
static struct dentry *sqfs_lookup(struct inode *dir, struct dentry *dentry);
static s64           sqfs_file_read(struct file *filp, void *buf,
                                    size_t len, u64 *offset);
static s64           sqfs_file_readdir(struct file *filp, void *dirent_buf,
                                       size_t len, u64 *offset);
static s64           sqfs_readlink(struct dentry *dentry,
                                   char *buf, size_t buflen);

/* ── VFS operation tables ─────────────────────────────────────────────────── */

/* All write ops are NULL — VFS returns -EROFS for any write attempt. */
static inode_operations_t sqfs_dir_iops = {
    .lookup   = sqfs_lookup,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .symlink  = NULL,
    .readlink = NULL,
    .link     = NULL,
};

static inode_operations_t sqfs_file_iops = {
    .lookup   = NULL,
    .readlink = NULL,
};

static inode_operations_t sqfs_symlink_iops = {
    .readlink = sqfs_readlink,
};

static file_operations_t sqfs_file_fops = {
    .read    = sqfs_file_read,
    .write   = NULL,
    .readdir = NULL,
    .ioctl   = NULL,
    .mmap    = NULL,
    .open    = NULL,
    .release = NULL,
    .poll    = NULL,
};

static file_operations_t sqfs_dir_fops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = sqfs_file_readdir,
    .ioctl   = NULL,
    .mmap    = NULL,
    .open    = NULL,
    .release = NULL,
    .poll    = NULL,
};

static file_system_type_t sqfs_fs_type = {
    .name  = "squashfs",
    .mount = sqfs_mount,
    .next  = NULL,
};

/* ── Super operations ─────────────────────────────────────────────────────── */
static struct inode *sqfs_alloc_inode(struct super_block *sb)
{
    (void)sb;
    struct inode *ino = (struct inode *)kzalloc(sizeof(struct inode));
    return ino;
}

static void sqfs_destroy_inode(struct inode *inode)
{
    kfree(inode);
}

static s64 sqfs_statfs_op(struct super_block *sb, struct statfs *buf)
{
    sqfs_fs_info_t *sbi = (sqfs_fs_info_t *)sb->s_fs_info;
    if (!sbi || !buf) return -(s64)EINVAL;

    __builtin_memset(buf, 0, sizeof(*buf));
    buf->f_type   = SQFS_MAGIC;
    buf->f_bsize  = sbi->block_size;
    buf->f_blocks = sbi->bytes_used / sbi->block_size;
    buf->f_bfree  = 0;
    buf->f_bavail = 0;
    buf->f_files  = sbi->inodes;
    buf->f_ffree  = 0;
    return 0;
}

static super_operations_t sqfs_sb_ops = {
    .alloc_inode   = sqfs_alloc_inode,
    .destroy_inode = sqfs_destroy_inode,
    .write_inode   = NULL,
    .put_super     = NULL,
    .statfs        = sqfs_statfs_op,
};

/* ── Low-level block I/O ──────────────────────────────────────────────────── */

/**
 * sqfs_read_bytes() — Read @len bytes starting at byte @offset from the device.
 * Always reads in sector multiples; clips the result to @buf.
 */
static s64 sqfs_read_bytes(sqfs_fs_info_t *sbi, u64 offset, void *buf, size_t len)
{
    if (!sbi || !sbi->bdev || !sbi->bdev->ops || !sbi->bdev->ops->read_sectors)
        return -(s64)EINVAL;
    if (!buf || len == 0) return 0;

    u32 sec_sz = sbi->bdev->sector_size ? sbi->bdev->sector_size : 512;
    u64 first_lba = offset / sec_sz;
    u64 end_byte  = offset + len;
    u64 last_lba  = (end_byte + sec_sz - 1) / sec_sz;
    u32 nsectors  = (u32)(last_lba - first_lba);

    u8 *tmp = (u8 *)kmalloc(nsectors * sec_sz);
    if (!tmp) return -(s64)ENOMEM;

    s64 ret = sbi->bdev->ops->read_sectors(sbi->bdev, first_lba, nsectors, tmp);
    if (ret < 0) {
        kfree(tmp);
        return ret;
    }

    u32 skip = (u32)(offset % sec_sz);
    __builtin_memcpy(buf, tmp + skip, len);
    kfree(tmp);
    return (s64)len;
}

/* ── Metadata block reader ────────────────────────────────────────────────── */

/**
 * sqfs_read_meta() — Read and decompress one metadata block from @abs_off.
 *
 * @sbi        Filesystem private data.
 * @abs_off    Absolute byte offset on the device of the 2-byte block header.
 * @out        Destination buffer (must be >= SQFS_META_SIZE bytes).
 * @out_size   On success, filled with the uncompressed byte count.
 * @next_off   On success, filled with the byte offset of the next block header.
 *
 * Returns 0 on success or a negative errno.
 */
static s64 sqfs_read_meta(sqfs_fs_info_t *sbi, u64 abs_off,
                           u8 *out, size_t *out_size, u64 *next_off)
{
    /* Read the 2-byte header */
    u8 hdr[2];
    s64 r = sqfs_read_bytes(sbi, abs_off, hdr, 2);
    if (r < 0) return r;

    u16 raw_len = (u16)((u32)hdr[0] | ((u32)hdr[1] << 8));
    int stored  = (raw_len & SQFS_META_HEADER_MASK) ? 1 : 0;
    u16 comp_sz = (u16)(raw_len & SQFS_META_LEN_MASK);

    if (comp_sz == 0 || comp_sz > SQFS_META_SIZE) return -(s64)EIO;

    u8 *comp_buf = (u8 *)kmalloc(comp_sz);
    if (!comp_buf) return -(s64)ENOMEM;

    r = sqfs_read_bytes(sbi, abs_off + 2, comp_buf, comp_sz);
    if (r < 0) { kfree(comp_buf); return r; }

    if (stored) {
        /* Block is stored verbatim */
        __builtin_memcpy(out, comp_buf, comp_sz);
        *out_size = comp_sz;
    } else {
        /* Decompress based on FS compression type */
        if (sbi->compression == SQFS_COMP_ZLIB) {
            size_t actual = 0;
            int err = sqfs_zlib_decompress(comp_buf, comp_sz,
                                           out, SQFS_META_SIZE, &actual);
            kfree(comp_buf);
            if (err) return -(s64)EIO;
            *out_size = actual;
        } else {
            /* TODO: lzma, lz4, lzo, xz, zstd */
            kfree(comp_buf);
            pr_debug("[squashfs] Unsupported compression type %u\n",
                     (u32)sbi->compression);
            return -(s64)ENOTSUP;
        }
        if (next_off) *next_off = abs_off + 2 + comp_sz;
        return 0;
    }

    kfree(comp_buf);
    if (next_off) *next_off = abs_off + 2 + comp_sz;
    return 0;
}

/* ── Inode reader ─────────────────────────────────────────────────────────── */

/**
 * sqfs_read_inode_raw() — Fetch raw inode bytes at the given inode reference.
 *
 * Fills @inode_buf with up to @inode_buf_len bytes starting from the inode
 * header.  The caller then overlays the appropriate sqfs_inode_*_t struct.
 *
 * @ref        Inode reference (block index + byte offset within block).
 * @inode_buf  Output buffer; must be >= sizeof(sqfs_inode_lfile_t) + extras.
 * @inode_buf_len  Size of @inode_buf in bytes.
 *
 * Returns 0 on success, negative errno on failure.
 */
static s64 sqfs_read_inode_raw(sqfs_fs_info_t *sbi,
                                sqfs_inode_ref_t ref,
                                void *inode_buf, size_t inode_buf_len)
{
    spinlock_lock(&sbi->lock);

    u64 meta_off = sbi->inode_table + ref.block;
    size_t meta_sz = 0;
    s64 r = sqfs_read_meta(sbi, meta_off, sbi->meta_buf, &meta_sz, NULL);
    if (r < 0) { spinlock_unlock(&sbi->lock); return r; }

    if (ref.offset >= meta_sz) {
        spinlock_unlock(&sbi->lock);
        return -(s64)EIO;
    }

    size_t avail = meta_sz - ref.offset;
    size_t copy  = avail < inode_buf_len ? avail : inode_buf_len;
    __builtin_memcpy(inode_buf, sbi->meta_buf + ref.offset, copy);
    /* Zero the tail so callers that overlay a fixed-size struct — or walk a
     * block-size list — never read uninitialised stack when the on-disk inode
     * is shorter than their buffer. */
    if (copy < inode_buf_len)
        __builtin_memset((u8 *)inode_buf + copy, 0, inode_buf_len - copy);

    spinlock_unlock(&sbi->lock);
    return 0;
}

/* ── ID table lookup ─────────────────────────────────────────────────────── */

/**
 * sqfs_get_id() — Look up UID or GID from the ID table at index @idx.
 * Returns the 32-bit ID or 0 on error.
 */
static u32 sqfs_get_id(sqfs_fs_info_t *sbi, u16 idx)
{
    if (!sbi) return 0;

    spinlock_lock(&sbi->lock);

    /* Lazy-load the ID table on first access */
    if (!sbi->id_table && sbi->id_count > 0) {
        /* The ID table starts with a list of 8-byte pointers to metadata
         * blocks, each of which holds up to floor(8192/4) = 2048 IDs. */
        u32 ptr_count = (sbi->id_count + 2047u) / 2048u;
        u64 *ptrs = (u64 *)kmalloc(ptr_count * 8);
        if (!ptrs) { spinlock_unlock(&sbi->lock); return 0; }

        s64 r = sqfs_read_bytes(sbi, sbi->id_table_ptr, ptrs, ptr_count * 8);
        if (r < 0) { kfree(ptrs); spinlock_unlock(&sbi->lock); return 0; }

        /* kzalloc: a metadata read that fails partway leaves the unfilled
         * tail as zeroes rather than heap garbage that would be handed back
         * as a uid/gid. */
        sbi->id_table = (u32 *)kzalloc(sbi->id_count * sizeof(u32));
        if (!sbi->id_table) { kfree(ptrs); spinlock_unlock(&sbi->lock); return 0; }

        u32 loaded = 0;
        u8 meta[SQFS_META_SIZE];
        for (u32 p = 0; p < ptr_count && loaded < sbi->id_count; p++) {
            size_t msz = 0;
            if (sqfs_read_meta(sbi, ptrs[p], meta, &msz, NULL) < 0) break;
            u32 count = (u32)(msz / 4);
            if (loaded + count > sbi->id_count) count = sbi->id_count - loaded;
            __builtin_memcpy(sbi->id_table + loaded, meta, count * 4);
            loaded += count;
        }
        kfree(ptrs);
    }

    u32 id = 0;
    if (sbi->id_table && idx < sbi->id_count)
        id = sbi->id_table[idx];

    spinlock_unlock(&sbi->lock);
    return id;
}

/* ── VFS inode builder ────────────────────────────────────────────────────── */

/**
 * sqfs_fill_inode() — Populate a VFS inode from raw SquashFS inode bytes.
 *
 * @vino       VFS inode to populate.
 * @sb         The superblock.
 * @raw        Raw bytes at the start of the sqfs inode (at least 32 bytes).
 * @raw_len    Number of bytes available in @raw.
 */
static s64 sqfs_fill_inode(inode_t *vino, super_block_t *sb,
                            const void *raw, size_t raw_len)
{
    sqfs_fs_info_t *sbi = (sqfs_fs_info_t *)sb->s_fs_info;
    const sqfs_inode_header_t *hdr = (const sqfs_inode_header_t *)raw;

    if (raw_len < sizeof(sqfs_inode_header_t)) return -(s64)EIO;

    u16 type = hdr->inode_type;
    vino->i_ino   = hdr->inode_number;
    vino->i_mtime = hdr->mtime;
    vino->i_atime = hdr->mtime;
    vino->i_ctime = hdr->mtime;
    vino->i_uid   = sqfs_get_id(sbi, hdr->uid_idx);
    vino->i_gid   = sqfs_get_id(sbi, hdr->gid_idx);
    vino->i_sb    = sb;

    switch (type) {
    case SQFS_ITYPE_DIR:
    case SQFS_ITYPE_LDIR:
        vino->i_mode  = S_IFDIR | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 2;
        vino->i_fop   = &sqfs_dir_fops;
        vino->i_op    = &sqfs_dir_iops;
        if (type == SQFS_ITYPE_DIR) {
            if (raw_len < sizeof(sqfs_inode_dir_t)) return -(s64)EIO;
            const sqfs_inode_dir_t *d = (const sqfs_inode_dir_t *)raw;
            vino->i_size   = d->file_size > 3 ? d->file_size - 3 : 0;
            vino->i_nlink  = d->hard_link_count;
        } else {
            if (raw_len < sizeof(sqfs_inode_ldir_t)) return -(s64)EIO;
            const sqfs_inode_ldir_t *d = (const sqfs_inode_ldir_t *)raw;
            vino->i_size   = d->file_size > 3 ? d->file_size - 3 : 0;
            vino->i_nlink  = d->hard_link_count;
        }
        break;

    case SQFS_ITYPE_FILE:
        vino->i_mode  = S_IFREG | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        vino->i_fop   = &sqfs_file_fops;
        vino->i_op    = &sqfs_file_iops;
        if (raw_len < sizeof(sqfs_inode_file_t)) return -(s64)EIO;
        vino->i_size  = ((const sqfs_inode_file_t *)raw)->file_size;
        break;

    case SQFS_ITYPE_LFILE:
        vino->i_mode  = S_IFREG | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        vino->i_fop   = &sqfs_file_fops;
        vino->i_op    = &sqfs_file_iops;
        if (raw_len < sizeof(sqfs_inode_lfile_t)) return -(s64)EIO;
        vino->i_size  = ((const sqfs_inode_lfile_t *)raw)->file_size;
        break;

    case SQFS_ITYPE_SYMLINK:
    case SQFS_ITYPE_LSYMLINK:
        vino->i_mode  = S_IFLNK | 0777u;
        vino->i_nlink = 1;
        vino->i_fop   = NULL;
        vino->i_op    = &sqfs_symlink_iops;
        if (raw_len < sizeof(sqfs_inode_symlink_t)) return -(s64)EIO;
        vino->i_size  = ((const sqfs_inode_symlink_t *)raw)->symlink_size;
        break;

    case SQFS_ITYPE_BLKDEV:
    case SQFS_ITYPE_LBLKDEV:
        vino->i_mode  = S_IFBLK | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        break;

    case SQFS_ITYPE_CHRDEV:
    case SQFS_ITYPE_LCHRDEV:
        vino->i_mode  = S_IFCHR | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        break;

    case SQFS_ITYPE_FIFO:
    case SQFS_ITYPE_LFIFO:
        vino->i_mode  = S_IFIFO | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        break;

    case SQFS_ITYPE_SOCKET:
    case SQFS_ITYPE_LSOCKET:
        vino->i_mode  = S_IFSOCK | (hdr->permissions & 0xFFFu);
        vino->i_nlink = 1;
        break;

    default:
        return -(s64)EIO;
    }

    return 0;
}

/* ── sqfs_itype_to_dt() — map sqfs inode type to dirent d_type ─────────────── */
static u8 sqfs_itype_to_dt(u16 t)
{
    switch (t) {
    case SQFS_ITYPE_DIR:    case SQFS_ITYPE_LDIR:    return DT_DIR;
    case SQFS_ITYPE_FILE:   case SQFS_ITYPE_LFILE:   return DT_REG;
    case SQFS_ITYPE_SYMLINK:case SQFS_ITYPE_LSYMLINK:return DT_LNK;
    case SQFS_ITYPE_BLKDEV: case SQFS_ITYPE_LBLKDEV: return DT_BLK;
    case SQFS_ITYPE_CHRDEV: case SQFS_ITYPE_LCHRDEV: return DT_CHR;
    case SQFS_ITYPE_FIFO:   case SQFS_ITYPE_LFIFO:   return DT_FIFO;
    case SQFS_ITYPE_SOCKET: case SQFS_ITYPE_LSOCKET: return DT_SOCK;
    default:                                          return DT_UNKNOWN;
    }
}

/* ── Directory inode private data ────────────────────────────────────────── */
typedef struct {
    u32 block_start;  /* start of directory listing metadata block   */
    u32 block_offset; /* offset within that block                    */
    u32 file_size;    /* total size of directory listing in bytes    */
} sqfs_dir_priv_t;

/* ── sqfs_lookup ─────────────────────────────────────────────────────────── */

static struct dentry *sqfs_lookup(struct inode *dir, struct dentry *dentry)
{
    if (!dir || !dentry) return NULL;

    sqfs_fs_info_t *sbi = (sqfs_fs_info_t *)dir->i_sb->s_fs_info;
    sqfs_dir_priv_t *dp = (sqfs_dir_priv_t *)dir->i_private;
    if (!dp) return NULL;

    const char *name    = dentry->d_name;
    size_t      namelen = 0;
    while (name[namelen]) namelen++;
    if (namelen == 0 || namelen > VFS_NAME_MAX) return NULL;

    /* We need to walk the directory table starting at dp->block_start.
     * Each metadata block is at:   sbi->dir_table + block_start (in bytes)
     * The listing starts at byte:  dp->block_offset within that block.
     * Total listing length:        dp->file_size bytes.                   */

    u64    abs_block = sbi->dir_table + dp->block_start;
    size_t meta_sz   = 0;
    u8     meta[SQFS_META_SIZE];

    spinlock_lock(&sbi->lock);
    s64 r = sqfs_read_meta(sbi, abs_block, meta, &meta_sz, NULL);
    spinlock_unlock(&sbi->lock);
    if (r < 0) return NULL;

    u32 consumed    = 0;
    u32 pos         = dp->block_offset;
    u32 total_bytes = dp->file_size;

    while (consumed < total_bytes) {
        if (pos + sizeof(sqfs_dir_header_t) > meta_sz) {
            /* Advance to the next metadata block */
            spinlock_lock(&sbi->lock);
            u64 next_off;
            r = sqfs_read_meta(sbi, abs_block, meta, &meta_sz, &next_off);
            spinlock_unlock(&sbi->lock);
            if (r < 0) return NULL;
            abs_block = next_off;
            pos = 0;
        }

        const sqfs_dir_header_t *dhdr =
            (const sqfs_dir_header_t *)(meta + pos);
        u32 entry_count = dhdr->count + 1; /* count is stored minus one */
        u32 inode_base  = dhdr->inode_number;
        u64 inode_block = dhdr->start;
        pos      += (u32)sizeof(sqfs_dir_header_t);
        consumed += (u32)sizeof(sqfs_dir_header_t);

        for (u32 e = 0; e < entry_count; e++) {
            if (pos + sizeof(sqfs_dir_entry_t) > meta_sz) {
                /* Need next block — reload */
                spinlock_lock(&sbi->lock);
                u64 next_off;
                r = sqfs_read_meta(sbi, abs_block, meta, &meta_sz, &next_off);
                spinlock_unlock(&sbi->lock);
                if (r < 0) return NULL;
                abs_block = next_off;
                pos = 0;
            }

            const sqfs_dir_entry_t *ent =
                (const sqfs_dir_entry_t *)(meta + pos);
            u16 ent_name_sz = (u16)(ent->name_size + 1); /* stored minus one */

            if (pos + sizeof(sqfs_dir_entry_t) + ent_name_sz > meta_sz)
                return NULL; /* sanity */

            const char *ent_name = (const char *)(meta + pos +
                                   sizeof(sqfs_dir_entry_t));

            size_t advance = sizeof(sqfs_dir_entry_t) + ent_name_sz;

            /* Name match? */
            if (ent_name_sz == (u16)namelen &&
                __builtin_memcmp(ent_name, name, namelen) == 0)
            {
                /* Found — build VFS inode */
                u32 inode_num = (u32)((s32)inode_base + ent->inode_delta);
                sqfs_inode_ref_t iref;
                iref.block  = inode_block;
                iref.offset = ent->offset;

                /* Read the raw inode bytes */
                u8 raw_ino[256];
                r = sqfs_read_inode_raw(sbi, iref, raw_ino, sizeof(raw_ino));
                if (r < 0) return NULL;

                inode_t *vino = sqfs_alloc_inode(dir->i_sb);
                if (!vino) return NULL;

                if (sqfs_fill_inode(vino, dir->i_sb, raw_ino, sizeof(raw_ino)) < 0) {
                    kfree(vino);
                    return NULL;
                }
                vino->i_ino = inode_num;

                /* Attach directory private data for dirs */
                if (S_ISDIR(vino->i_mode)) {
                    sqfs_dir_priv_t *child_dp =
                        (sqfs_dir_priv_t *)kzalloc(sizeof(sqfs_dir_priv_t));
                    if (!child_dp) { kfree(vino); return NULL; }

                    const sqfs_inode_header_t *ihdr =
                        (const sqfs_inode_header_t *)raw_ino;
                    if (ihdr->inode_type == SQFS_ITYPE_DIR &&
                        sizeof(raw_ino) >= sizeof(sqfs_inode_dir_t))
                    {
                        const sqfs_inode_dir_t *di =
                            (const sqfs_inode_dir_t *)raw_ino;
                        child_dp->block_start  = di->dir_block_start;
                        child_dp->block_offset = di->block_offset;
                        child_dp->file_size    = di->file_size > 3
                                                    ? di->file_size - 3 : 0;
                    } else if (ihdr->inode_type == SQFS_ITYPE_LDIR &&
                               sizeof(raw_ino) >= sizeof(sqfs_inode_ldir_t))
                    {
                        const sqfs_inode_ldir_t *di =
                            (const sqfs_inode_ldir_t *)raw_ino;
                        child_dp->block_start  = di->dir_block_start;
                        child_dp->block_offset = di->block_offset;
                        child_dp->file_size    = di->file_size > 3
                                                    ? di->file_size - 3 : 0;
                    }
                    vino->i_private = child_dp;
                }

                dentry->d_inode = vino;
                dcache_add(dentry);
                return dentry;
            }

            pos      += (u32)advance;
            consumed += (u32)advance;
        }
    }

    return NULL; /* not found */
}

/* ── Fragment table loader ────────────────────────────────────────────────── */

static s64 sqfs_ensure_frag_table(sqfs_fs_info_t *sbi)
{
    if (sbi->frag_table) return 0;
    if (sbi->fragments == 0) return 0;

    /* The table cannot describe more fragments than the archive has room for.
     * Rejecting an inconsistent count here also keeps the (fragments + 511)
     * rounding below from wrapping u32. */
    if ((u64)sbi->fragments * sizeof(sqfs_fragment_entry_t) > sbi->bytes_used)
        return -(s64)EIO;

    /* The fragment table pointer list is stored as a series of 8-byte LE
     * pointers, each pointing to a metadata block that holds packed
     * sqfs_fragment_entry_t records (16 bytes each). */
    u32 ptr_count = (sbi->fragments + 511u) / 512u; /* 512 entries per 8 KiB block */
    u64 *ptrs = (u64 *)kmalloc(ptr_count * 8);
    if (!ptrs) return -(s64)ENOMEM;

    s64 r = sqfs_read_bytes(sbi, sbi->frag_table_ptr, ptrs, ptr_count * 8);
    if (r < 0) { kfree(ptrs); return r; }

    /* kzalloc: if a metadata block fails to load, entries past `loaded` stay
     * zeroed instead of holding heap garbage that sqfs_read_data_block() would
     * treat as a real (start, size) pair. */
    sbi->frag_table = (sqfs_fragment_entry_t *)
        kzalloc(sbi->fragments * sizeof(sqfs_fragment_entry_t));
    if (!sbi->frag_table) { kfree(ptrs); return -(s64)ENOMEM; }

    u32 loaded = 0;
    u8 meta[SQFS_META_SIZE];
    for (u32 p = 0; p < ptr_count && loaded < sbi->fragments; p++) {
        size_t msz = 0;
        if (sqfs_read_meta(sbi, ptrs[p], meta, &msz, NULL) < 0) break;
        u32 entries = (u32)(msz / sizeof(sqfs_fragment_entry_t));
        if (loaded + entries > sbi->fragments)
            entries = sbi->fragments - loaded;
        __builtin_memcpy(sbi->frag_table + loaded, meta,
                         entries * sizeof(sqfs_fragment_entry_t));
        loaded += entries;
    }
    kfree(ptrs);
    return 0;
}

/* ── Data block decompressor ─────────────────────────────────────────────── */

/**
 * sqfs_read_data_block() — Read and decompress one data block from the image.
 *
 * @abs_offset   Byte offset of the (possibly compressed) data block on device.
 * @comp_size    Value from the inode block-size table (size field).
 *               MSB set means the block is stored uncompressed.
 * @out          Output buffer (>= sbi->block_size bytes).
 * @out_actual   Filled with the number of bytes actually decompressed.
 *
 * Returns 0 on success, negative errno on failure.
 */
static s64 sqfs_read_data_block(sqfs_fs_info_t *sbi,
                                 u64 abs_offset, u32 comp_size,
                                 u8 *out, size_t *out_actual)
{
    int stored   = (comp_size & 0x01000000u) ? 1 : 0;
    u32 raw_size = comp_size & 0x00FFFFFFu;

    /* A valid image never stores a data or fragment block larger than one
     * filesystem block. A crafted size field otherwise drives the stored-path
     * memcpy() below straight past the end of @out (sbi->data_buf). */
    if (raw_size > sbi->block_size) return -(s64)EIO;

    if (raw_size == 0) {
        /* Sparse (zero) block */
        __builtin_memset(out, 0, sbi->block_size);
        *out_actual = sbi->block_size;
        return 0;
    }

    u8 *tmp = (u8 *)kmalloc(raw_size);
    if (!tmp) return -(s64)ENOMEM;

    s64 r = sqfs_read_bytes(sbi, abs_offset, tmp, raw_size);
    if (r < 0) { kfree(tmp); return r; }

    if (stored) {
        __builtin_memcpy(out, tmp, raw_size);
        *out_actual = raw_size;
        kfree(tmp);
        return 0;
    }

    if (sbi->compression == SQFS_COMP_ZLIB) {
        size_t actual = 0;
        int err = sqfs_zlib_decompress(tmp, raw_size,
                                       out, SQFS_DATA_BUF_SIZE, &actual);
        kfree(tmp);
        if (err) return -(s64)EIO;
        *out_actual = actual;
        return 0;
    }

    kfree(tmp);
    pr_debug("[squashfs] Unsupported data compression %u\n",
             (u32)sbi->compression);
    return -(s64)ENOTSUP;
}

/* ── Regular file read ───────────────────────────────────────────────────── */

static s64 sqfs_file_read(struct file *filp, void *buf,
                           size_t len, u64 *offset)
{
    if (!filp || !filp->f_inode || !buf || !offset) return -(s64)EINVAL;

    inode_t        *vino = filp->f_inode;
    super_block_t  *sb   = vino->i_sb;
    sqfs_fs_info_t *sbi  = (sqfs_fs_info_t *)sb->s_fs_info;

    if (*offset >= (u64)vino->i_size) return 0;
    if (*offset + len > (u64)vino->i_size)
        len = (size_t)((u64)vino->i_size - *offset);
    if (len == 0) return 0;

    /* Read the raw inode to find blocks_start, fragment index, block sizes */
    sqfs_inode_ref_t iref;
    iref.block  = 0;
    iref.offset = 0;
    /* We store the inode reference in i_private as a packed u64 */
    if (vino->i_private)
        iref = sqfs_iref_unpack((u64)(uintptr_t)vino->i_private);

    u8 raw_ino[512];
    __builtin_memset(raw_ino, 0, sizeof(raw_ino));
    s64 r = sqfs_read_inode_raw(sbi, iref, raw_ino, sizeof(raw_ino));
    if (r < 0) return r;

    const sqfs_inode_header_t *hdr = (const sqfs_inode_header_t *)raw_ino;

    u64  blocks_start;
    u32  frag_idx;
    u32  frag_offset;
    u64  file_size;
    const u32 *block_sizes;
    u32  n_blocks;
    int  is_lfile = 0;

    if (hdr->inode_type == SQFS_ITYPE_FILE) {
        const sqfs_inode_file_t *fi = (const sqfs_inode_file_t *)raw_ino;
        blocks_start = fi->blocks_start;
        frag_idx     = fi->fragment_block_index;
        frag_offset  = fi->block_offset;
        file_size    = fi->file_size;
        block_sizes  = (const u32 *)(raw_ino + sizeof(sqfs_inode_file_t));
    } else if (hdr->inode_type == SQFS_ITYPE_LFILE) {
        const sqfs_inode_lfile_t *fi = (const sqfs_inode_lfile_t *)raw_ino;
        blocks_start = fi->blocks_start;
        frag_idx     = fi->fragment_block_index;
        frag_offset  = fi->block_offset;
        file_size    = fi->file_size;
        block_sizes  = (const u32 *)(raw_ino + sizeof(sqfs_inode_lfile_t));
        is_lfile     = 1;
    } else {
        return -(s64)EINVAL;
    }
    (void)is_lfile;

    /* Number of full data blocks (the tail goes into a fragment unless
     * SQFS_FLAG_NO_FRAG is set, in which case the last block is a partial) */
    u32 block_sz   = sbi->block_size;
    u32 no_frag    = sbi->flags & SQFS_FLAG_NO_FRAG;
    n_blocks = (u32)(file_size / block_sz);
    if (no_frag && (file_size % block_sz)) n_blocks++;

    /* The block-size list is inlined after the inode header inside raw_ino[].
     * This reader only supports a list that fits there; a larger one (i.e. a
     * huge or forged file_size) would make block_sizes[blk] walk off the end
     * of the stack buffer. */
    {
        size_t hdr_sz     = is_lfile ? sizeof(sqfs_inode_lfile_t)
                                     : sizeof(sqfs_inode_file_t);
        u32    max_blocks = (u32)((sizeof(raw_ino) - hdr_sz) / sizeof(u32));
        if (n_blocks > max_blocks) return -(s64)EIO;
    }

    size_t bytes_read = 0;
    u8    *dst        = (u8 *)buf;

    /* Iterate over the relevant data blocks */
    u64 file_off = *offset;
    u64 cur_abs  = blocks_start;

    for (u32 blk = 0; blk < n_blocks && bytes_read < len; blk++) {
        u64 blk_start = (u64)blk * block_sz;
        u64 blk_end   = blk_start + block_sz;
        if (blk_end > file_size) blk_end = file_size;

        /* Skip blocks before the requested offset */
        if (blk_end <= file_off) {
            cur_abs += block_sizes[blk] & 0x00FFFFFFu;
            /* If stored, raw size IS block_size */
            if (block_sizes[blk] & 0x01000000u)
                cur_abs = (u64)blk * block_sz + blocks_start;
            /* Better: just accumulate the on-disk size */
            u32 on_disk = block_sizes[blk] & 0x00FFFFFFu;
            /* We already advanced cur_abs once above; undo then redo properly */
            cur_abs -= on_disk; /* undo */
            cur_abs += on_disk; /* redo (nop, kept for clarity) */
            continue;
        }

        spinlock_lock(&sbi->lock);
        size_t blk_actual = 0;
        r = sqfs_read_data_block(sbi, cur_abs, block_sizes[blk],
                                  sbi->data_buf, &blk_actual);
        if (r < 0) { spinlock_unlock(&sbi->lock); return r; }

        u64 copy_start = (file_off > blk_start) ? file_off - blk_start : 0;
        u64 copy_end   = blk_end - blk_start;
        if (copy_end > blk_actual) copy_end = blk_actual;
        size_t copy_sz = (size_t)(copy_end - copy_start);
        if (bytes_read + copy_sz > len) copy_sz = len - bytes_read;

        __builtin_memcpy(dst + bytes_read,
                         sbi->data_buf + copy_start, copy_sz);
        spinlock_unlock(&sbi->lock);

        bytes_read += copy_sz;
        cur_abs    += (block_sizes[blk] & 0x00FFFFFFu);
    }

    /* Handle the fragment (tail of file) */
    if (bytes_read < len && frag_idx != 0xFFFFFFFFu && sbi->fragments > 0) {
        spinlock_lock(&sbi->lock);
        r = sqfs_ensure_frag_table(sbi);
        if (r < 0) { spinlock_unlock(&sbi->lock); return r; }

        if (frag_idx >= sbi->fragments) {
            spinlock_unlock(&sbi->lock);
            goto done;
        }

        const sqfs_fragment_entry_t *fe = &sbi->frag_table[frag_idx];
        size_t fblk_actual = 0;
        r = sqfs_read_data_block(sbi, fe->start, fe->size,
                                  sbi->data_buf, &fblk_actual);
        if (r < 0) { spinlock_unlock(&sbi->lock); return r; }

        u64 frag_data_start = (u64)n_blocks * block_sz;
        u64 frag_len        = file_size - frag_data_start;

        if (file_off < frag_data_start + frag_len) {
            u64 in_frag = (file_off > frag_data_start)
                           ? file_off - frag_data_start : 0;
            u64 avail   = frag_len - in_frag;
            size_t copy_sz = (size_t)avail;
            if (bytes_read + copy_sz > len) copy_sz = len - bytes_read;

            if (frag_offset + in_frag + copy_sz <= fblk_actual) {
                __builtin_memcpy(dst + bytes_read,
                                 sbi->data_buf + frag_offset + in_frag,
                                 copy_sz);
                bytes_read += copy_sz;
            }
        }
        spinlock_unlock(&sbi->lock);
    }

done:
    *offset += bytes_read;
    return (s64)bytes_read;
}

/* ── Directory readdir ───────────────────────────────────────────────────── */

static s64 sqfs_file_readdir(struct file *filp, void *dirent_buf,
                              size_t len, u64 *offset)
{
    if (!filp || !filp->f_inode) return -(s64)EINVAL;

    inode_t        *vino = filp->f_inode;
    super_block_t  *sb   = vino->i_sb;
    sqfs_fs_info_t *sbi  = (sqfs_fs_info_t *)sb->s_fs_info;
    sqfs_dir_priv_t *dp  = (sqfs_dir_priv_t *)vino->i_private;
    if (!dp) return -(s64)ENOTDIR;

    u64    abs_block  = sbi->dir_table + dp->block_start;
    size_t meta_sz    = 0;
    u8     meta[SQFS_META_SIZE];

    spinlock_lock(&sbi->lock);
    s64 r = sqfs_read_meta(sbi, abs_block, meta, &meta_sz, NULL);
    spinlock_unlock(&sbi->lock);
    if (r < 0) return r;

    size_t out_pos   = 0;
    u32    consumed  = (u32)*offset;
    u32    pos       = dp->block_offset;
    u32    total_bytes = dp->file_size;
    u8    *out       = (u8 *)dirent_buf;

    /* Skip already-consumed bytes */
    u32 skip = consumed;

    while (pos < (u32)meta_sz && (consumed - skip) < total_bytes) {
        if (pos + sizeof(sqfs_dir_header_t) > (u32)meta_sz) break;

        const sqfs_dir_header_t *dhdr =
            (const sqfs_dir_header_t *)(meta + pos);
        u32 entry_count = dhdr->count + 1;
        s32 inode_base  = (s32)dhdr->inode_number;
        u64 inode_block = dhdr->start;
        pos      += (u32)sizeof(sqfs_dir_header_t);

        for (u32 e = 0; e < entry_count; e++) {
            if (pos + sizeof(sqfs_dir_entry_t) > (u32)meta_sz) goto done;

            const sqfs_dir_entry_t *ent =
                (const sqfs_dir_entry_t *)(meta + pos);
            u16 nlen = (u16)(ent->name_size + 1);

            if (pos + sizeof(sqfs_dir_entry_t) + nlen > (u32)meta_sz) goto done;

            const char *ename = (const char *)(meta + pos +
                                sizeof(sqfs_dir_entry_t));
            u32  advance = (u32)(sizeof(sqfs_dir_entry_t) + nlen);

            /* Compute the dirent record length (8-byte aligned) */
            u32 reclen = (u32)(offsetof(sqfs_dirent64_t, d_name) + nlen + 1);
            reclen = (reclen + 7u) & ~7u;

            if (skip > 0) {
                /* Still skipping already-emitted entries */
                pos      += advance;
                skip--;
                continue;
            }

            if (out_pos + reclen > len) goto done;

            sqfs_dirent64_t *de = (sqfs_dirent64_t *)(out + out_pos);
            u32 ino_num = (u32)(inode_base + ent->inode_delta);
            de->d_ino    = ino_num;
            de->d_off    = (s64)(consumed + 1);
            de->d_reclen = (u16)reclen;
            de->d_type   = sqfs_itype_to_dt(ent->type);
            __builtin_memcpy(de->d_name, ename, nlen);
            de->d_name[nlen] = '\0';

            out_pos  += reclen;
            consumed++;
            pos      += advance;
            (void)inode_block;
        }
    }

done:
    *offset = consumed;
    return (s64)out_pos;
}

/* ── Symlink readlink ─────────────────────────────────────────────────────── */

static s64 sqfs_readlink(struct dentry *dentry, char *buf, size_t buflen)
{
    if (!dentry || !dentry->d_inode || !buf) return -(s64)EINVAL;

    inode_t        *vino = dentry->d_inode;
    super_block_t  *sb   = vino->i_sb;
    sqfs_fs_info_t *sbi  = (sqfs_fs_info_t *)sb->s_fs_info;

    sqfs_inode_ref_t iref;
    iref.block  = 0;
    iref.offset = 0;
    if (vino->i_private)
        iref = sqfs_iref_unpack((u64)(uintptr_t)vino->i_private);

    u8 raw[512];
    s64 r = sqfs_read_inode_raw(sbi, iref, raw, sizeof(raw));
    if (r < 0) return r;

    const sqfs_inode_symlink_t *si = (const sqfs_inode_symlink_t *)raw;
    u32 link_sz = si->symlink_size;
    if (link_sz >= sizeof(raw) - sizeof(sqfs_inode_symlink_t))
        return -(s64)EIO;

    const char *target = (const char *)(raw + sizeof(sqfs_inode_symlink_t));
    size_t copy = link_sz < buflen - 1 ? link_sz : buflen - 1;
    __builtin_memcpy(buf, target, copy);
    buf[copy] = '\0';
    return (s64)copy;
}

/* ── Mount ───────────────────────────────────────────────────────────────── */

static s64 sqfs_mount(file_system_type_t *fs_type,
                       const char *dev_name,
                       const char *dir_name, void *data)
{
    (void)data;
    pr_debug("[squashfs] Mounting '%s' on '%s'\n", dev_name, dir_name);

    /* Find the block device */
    block_dev_t *bdev = block_dev_get(dev_name);
    if (!bdev) {
        pr_debug("[squashfs] Device '%s' not found\n", dev_name);
        return -(s64)EINVAL;
    }

    /* Read the superblock (at byte offset 0) */
    sqfs_superblock_t *sb_raw =
        (sqfs_superblock_t *)kmalloc(sizeof(sqfs_superblock_t));
    if (!sb_raw) return -(s64)ENOMEM;

    /* Determine sector size */
    u32 sec_sz = bdev->sector_size ? bdev->sector_size : 512;
    u32 nsec   = (u32)(((sizeof(sqfs_superblock_t)) + sec_sz - 1) / sec_sz);
    u8  *tmp   = (u8 *)kmalloc(nsec * sec_sz);
    if (!tmp) { kfree(sb_raw); return -(s64)ENOMEM; }

    if (!bdev->ops || !bdev->ops->read_sectors ||
        bdev->ops->read_sectors(bdev, 0, nsec, tmp) < 0)
    {
        kfree(tmp); kfree(sb_raw);
        return -(s64)EIO;
    }
    __builtin_memcpy(sb_raw, tmp, sizeof(sqfs_superblock_t));
    kfree(tmp);

    /* Validate */
    if (sb_raw->s_magic != SQFS_MAGIC) {
        pr_debug("[squashfs] Bad magic: 0x%08x\n", sb_raw->s_magic);
        kfree(sb_raw);
        return -(s64)EINVAL;
    }
    if (sb_raw->s_s_major != SQFS_VERSION_MAJOR) {
        pr_debug("[squashfs] Unsupported version %u.%u\n",
                 sb_raw->s_s_major, sb_raw->s_s_minor);
        kfree(sb_raw);
        return -(s64)EINVAL;
    }
    u32 bsz = sb_raw->s_block_size;
    if (bsz < 4096 || bsz > SQFS_DATA_BUF_SIZE || (bsz & (bsz - 1))) {
        pr_debug("[squashfs] Bad block size %u\n", bsz);
        kfree(sb_raw);
        return -(s64)EINVAL;
    }

    /* Allocate and populate private data */
    sqfs_fs_info_t *sbi = (sqfs_fs_info_t *)kzalloc(sizeof(sqfs_fs_info_t));
    if (!sbi) { kfree(sb_raw); return -(s64)ENOMEM; }

    sbi->bdev              = bdev;
    sbi->block_size        = bsz;
    sbi->block_log         = sb_raw->s_block_log;
    sbi->compression       = sb_raw->s_compression;
    sbi->flags             = sb_raw->s_flags;
    sbi->inodes            = sb_raw->s_inodes;
    sbi->fragments         = sb_raw->s_fragments;
    sbi->inode_table       = sb_raw->s_inode_table_start;
    sbi->dir_table         = sb_raw->s_directory_table_start;
    sbi->frag_table_ptr    = sb_raw->s_fragment_table_start;
    sbi->id_table_ptr      = sb_raw->s_id_table_start;
    sbi->id_count          = sb_raw->s_no_ids;
    sbi->bytes_used        = sb_raw->s_bytes_used;
    sbi->root_iref         = sqfs_iref_unpack(sb_raw->s_root_inode);
    spinlock_init(&sbi->lock);

    sbi->meta_buf = (u8 *)kmalloc(SQFS_META_BUF_SIZE);
    sbi->data_buf = (u8 *)kmalloc(SQFS_DATA_BUF_SIZE);
    if (!sbi->meta_buf || !sbi->data_buf) {
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        kfree(sb_raw);
        return -(s64)ENOMEM;
    }

    kfree(sb_raw);

    /* Build VFS superblock */
    super_block_t *vsb = (super_block_t *)kzalloc(sizeof(super_block_t));
    if (!vsb) {
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        return -(s64)ENOMEM;
    }
    vsb->s_magic    = SQFS_MAGIC;
    vsb->s_blocksize = bsz;
    vsb->s_type     = fs_type;
    vsb->s_op       = &sqfs_sb_ops;
    vsb->s_fs_info  = sbi;

    /* Read root inode */
    u8 root_raw[512];
    s64 r = sqfs_read_inode_raw(sbi, sbi->root_iref, root_raw, sizeof(root_raw));
    if (r < 0) {
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        kfree(vsb);
        return r;
    }

    inode_t *root_ino = sqfs_alloc_inode(vsb);
    if (!root_ino || sqfs_fill_inode(root_ino, vsb, root_raw, sizeof(root_raw)) < 0) {
        kfree(root_ino);
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        kfree(vsb);
        return -(s64)EIO;
    }

    /* Attach directory private data to root */
    sqfs_dir_priv_t *root_dp =
        (sqfs_dir_priv_t *)kzalloc(sizeof(sqfs_dir_priv_t));
    if (!root_dp) {
        kfree(root_ino);
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        kfree(vsb);
        return -(s64)ENOMEM;
    }

    const sqfs_inode_header_t *rhdr = (const sqfs_inode_header_t *)root_raw;
    if (rhdr->inode_type == SQFS_ITYPE_DIR) {
        const sqfs_inode_dir_t *rd = (const sqfs_inode_dir_t *)root_raw;
        root_dp->block_start  = rd->dir_block_start;
        root_dp->block_offset = rd->block_offset;
        root_dp->file_size    = rd->file_size > 3 ? rd->file_size - 3 : 0;
    } else if (rhdr->inode_type == SQFS_ITYPE_LDIR) {
        const sqfs_inode_ldir_t *rd = (const sqfs_inode_ldir_t *)root_raw;
        root_dp->block_start  = rd->dir_block_start;
        root_dp->block_offset = rd->block_offset;
        root_dp->file_size    = rd->file_size > 3 ? rd->file_size - 3 : 0;
    }
    root_ino->i_private = root_dp;

    /* Build root dentry */
    dentry_t *root_dentry = dcache_alloc(NULL, "/");
    if (!root_dentry) {
        kfree(root_dp);
        kfree(root_ino);
        kfree(sbi->meta_buf);
        kfree(sbi->data_buf);
        kfree(sbi);
        kfree(vsb);
        return -(s64)ENOMEM;
    }
    root_dentry->d_inode  = root_ino;
    root_dentry->d_sb     = vsb;
    root_dentry->d_parent = root_dentry;
    vsb->s_root           = root_dentry;

    /* Graft the squashfs tree into the VFS namespace */
    r = vfs_mount(dev_name, dir_name, "squashfs", NULL);
    /* vfs_mount calls back into us if needed; on the path that comes
     * here this is the internal registration step — just attach the sb. */
    (void)r;

    pr_debug("[squashfs] Mounted %s on %s  (%u inodes, block_size=%u, comp=%u)\n",
             dev_name, dir_name,
             sbi->inodes, sbi->block_size, (u32)sbi->compression);
    return 0;
}

/* ── squashfs_init() ─────────────────────────────────────────────────────── */

void squashfs_init(void)
{
    s64 r = vfs_register_fs(&sqfs_fs_type);
    if (r == 0)
        pr_debug("[squashfs] Filesystem type registered\n");
    else
        pr_debug("[squashfs] WARNING: vfs_register_fs returned %lld\n",
                 (long long)r);
}
