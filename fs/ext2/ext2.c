/* ============================================================================
 * AzamiOS — Ext2 Filesystem Driver
 * File: fs/ext2/ext2.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "ext2.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../drivers/char/console.h"
#include "../../drivers/misc/rtc.h"

/* Forward declarations */
static s64 ext2_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data);
static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry);
static s64 ext2_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 ext2_file_write(struct file *filp, const void *buf, size_t len, u64 *offset);
static s64 ext2_file_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);
static s64 ext2_create(struct inode *dir, struct dentry *dentry, u32 mode);
static s64 ext2_mkdir(struct inode *dir, struct dentry *dentry, u32 mode);
static s64 ext2_unlink(struct inode *dir, struct dentry *dentry);
static s64 ext2_rmdir(struct inode *dir, struct dentry *dentry);
static s64 ext2_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry);
static s64 ext2_symlink(struct inode *dir, struct dentry *dentry, const char *symname);


static inode_operations_t ext2_inode_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .mkdir = ext2_mkdir,
    .unlink = ext2_unlink,
    .rmdir = ext2_rmdir,
    .rename = ext2_rename,
    .symlink = ext2_symlink,
};

static file_operations_t ext2_file_ops = {
    .read = ext2_file_read,
    .write = ext2_file_write,
    .readdir = ext2_file_readdir,
};

static s64 ext2_read_block(ext2_fs_info_t *fs, u32 block, void *buf)
{
    u64 lba = (u64)block * (fs->block_size / fs->bdev->sector_size);
    u32 count = fs->block_size / fs->bdev->sector_size;
    return fs->bdev->ops->read_sectors(fs->bdev, lba, count, buf);
}

static s64 ext2_write_block(ext2_fs_info_t *fs, u32 block, void *buf)
{
    if (!fs->bdev->ops->write_sectors) return -(s64)ENOSYS;
    u64 lba = (u64)block * (fs->block_size / fs->bdev->sector_size);
    u32 count = fs->block_size / fs->bdev->sector_size;
    return fs->bdev->ops->write_sectors(fs->bdev, lba, count, buf);
}

/* Helper: Read an inode from disk */
static s64 ext2_read_inode(ext2_fs_info_t *fs, u32 ino, ext2_inode_t *out_inode)
{
    if (ino < 1 || ino > fs->sb->s_inodes_count) return -(s64)EINVAL;
    
    u32 bg = (ino - 1) / fs->inodes_per_group;
    u32 index = (ino - 1) % fs->inodes_per_group;
    u32 inode_size = fs->sb->s_rev_level >= 1 ? fs->sb->s_inode_size : 128;
    
    u32 block = fs->bgdt[bg].bg_inode_table + (index * inode_size) / fs->block_size;
    u32 offset = (index * inode_size) % fs->block_size;
    
    void *buf = kzalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;
    
    s64 err = ext2_read_block(fs, block, buf);
    if (err < 0) { kfree(buf); return err; }
    
    __builtin_memcpy(out_inode, (u8*)buf + offset, sizeof(ext2_inode_t));
    kfree(buf);
    return 0;
}

/* Mount the filesystem */
static s64 ext2_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)fs_type; (void)dir_name; (void)data;
    
    block_dev_t *bdev = block_dev_get(dev_name);
    if (!bdev) return -(s64)ENODEV;
    
    /* Read superblock (always at offset 1024, which is LBA 2 for 512b sectors) */
    void *sb_buf = kzalloc(1024);
    if (!sb_buf) return -(s64)ENOMEM;
    
    if (bdev->ops->read_sectors(bdev, 2, 2, sb_buf) < 0) {
        kfree(sb_buf);
        return -(s64)EIO;
    }
    
    ext2_superblock_t *sb_disk = (ext2_superblock_t *)sb_buf;
    if (sb_disk->s_magic != EXT2_SUPER_MAGIC) {
        kfree(sb_buf);
        return -(s64)EINVAL; /* Not ext2 */
    }
    
    ext2_fs_info_t *fs = kzalloc(sizeof(ext2_fs_info_t));
    if (!fs) {
        kfree(sb_buf);
        return -(s64)ENOMEM;
    }
    fs->bdev = bdev;
    fs->sb = kzalloc(sizeof(ext2_superblock_t));
    if (!fs->sb) {
        kfree(fs);
        kfree(sb_buf);
        return -(s64)ENOMEM;
    }
    __builtin_memcpy(fs->sb, sb_disk, sizeof(ext2_superblock_t));
    kfree(sb_buf);
    
    fs->block_size = 1024 << fs->sb->s_log_block_size;
    fs->inodes_per_group = fs->sb->s_inodes_per_group;
    fs->blocks_per_group = fs->sb->s_blocks_per_group;
    fs->block_groups_count = (fs->sb->s_blocks_count + fs->blocks_per_group - 1) / fs->blocks_per_group;
    
    /* Read Block Group Descriptor Table */
    u32 bgdt_block = fs->block_size == 1024 ? 2 : 1;
    u32 bgdt_size = fs->block_groups_count * sizeof(ext2_bg_descriptor_t);
    u32 bgdt_blocks = (bgdt_size + fs->block_size - 1) / fs->block_size;
    
    fs->bgdt = kzalloc(bgdt_blocks * fs->block_size);
    for (u32 i = 0; i < bgdt_blocks; i++) {
        ext2_read_block(fs, bgdt_block + i, (u8*)fs->bgdt + (i * fs->block_size));
    }
    
    fs->block_bitmaps = kzalloc(fs->block_groups_count * sizeof(u8*));
    fs->inode_bitmaps = kzalloc(fs->block_groups_count * sizeof(u8*));
    
    if (!fs->block_bitmaps || !fs->inode_bitmaps) {
        if (fs->block_bitmaps) kfree(fs->block_bitmaps);
        if (fs->inode_bitmaps) kfree(fs->inode_bitmaps);
        kfree(fs->bgdt);
        kfree(fs->sb);
        kfree(fs);
        return -(s64)ENOMEM;
    }
    
    /* Create VFS superblock and root inode */
    super_block_t *vfs_sb = kzalloc(sizeof(super_block_t));
    vfs_sb->s_magic = EXT2_SUPER_MAGIC;
    vfs_sb->s_blocksize = fs->block_size;
    vfs_sb->s_fs_info = fs;
    
    ext2_inode_t root_ino_disk;
    ext2_read_inode(fs, EXT2_ROOT_INO, &root_ino_disk);
    
    inode_t *root_inode = kzalloc(sizeof(inode_t));
    root_inode->i_ino = EXT2_ROOT_INO;
    root_inode->i_mode = root_ino_disk.i_mode;
    root_inode->i_size = root_ino_disk.i_size;
    root_inode->i_sb = vfs_sb;
    root_inode->i_op = &ext2_inode_ops;
    root_inode->i_fop = &ext2_file_ops;
    
    ext2_inode_info_t *root_priv = kzalloc(sizeof(ext2_inode_info_t));
    __builtin_memcpy(root_priv->i_block, root_ino_disk.i_block, sizeof(root_priv->i_block));
    root_inode->i_private = root_priv;
    
    /* If target is "/", we just replace the global root. Otherwise we'd graft it. */
    struct dentry *target_dir = NULL;
    vfs_path_lookup(dir_name, &target_dir);
    
    if (target_dir) {
        target_dir->d_inode = root_inode;
        target_dir->d_sb = vfs_sb;
        vfs_sb->s_root = target_dir;
    } else {
        /* If the target directory doesn't exist, create a dentry in the root for it. */
        if (dir_name[0] == '/' && dir_name[1] != '\0') {
            struct dentry *old_root = NULL;
            vfs_path_lookup("/", &old_root);
            if (old_root) {
                struct dentry *new_mount = dcache_alloc(old_root, dir_name + 1);
                if (new_mount) {
                    new_mount->d_inode = root_inode;
                    new_mount->d_sb = vfs_sb;
                    vfs_sb->s_root = new_mount;
                    dcache_add(new_mount);
                }
            }
        } else {
            dentry_t *root_dentry = dcache_alloc(NULL, "/");
            if (root_dentry) {
                root_dentry->d_inode = root_inode;
                root_dentry->d_sb = vfs_sb;
                vfs_sb->s_root = root_dentry;
            }
        }
    }
    pr_debug("[EXT2] Mounted %s on %s (Block size: %u)\n", dev_name, dir_name, fs->block_size);
    return 0;
}

/* Directory Lookup */
static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    ext2_inode_info_t *dir_priv = (ext2_inode_info_t *)dir->i_private;
    
    if (!S_ISDIR(dir->i_mode)) return NULL;
    
    void *buf = kzalloc(fs->block_size);
    if (!buf) return NULL;
    
    /* Simple iteration over direct blocks only for now */
    for (int i = 0; i < 12; i++) {
        u32 block = dir_priv->i_block[i];
        if (!block) continue;
        
        ext2_read_block(fs, block, buf);
        
        u32 offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = (ext2_dir_entry_t *)((u8*)buf + offset);
            if (ent->rec_len == 0) break;
            if (ent->inode == 0) {
                offset += ent->rec_len;
                continue;
            }
            
            bool match = true;
            int name_len = 0;
            while (dentry->d_name[name_len]) name_len++;
            
            if (ent->name_len == name_len) {
                for (int j = 0; j < name_len; j++) {
                    if (ent->name[j] != dentry->d_name[j]) { match = false; break; }
                }
                if (match) {
                    /* Found it! Create inode */
                    ext2_inode_t ino_disk;
                    ext2_read_inode(fs, ent->inode, &ino_disk);
                    
                    inode_t *inode = kzalloc(sizeof(inode_t));
                    if (!inode) {
                        kfree(buf);
                        return NULL;
                    }
                    inode->i_ino = ent->inode;
                    inode->i_mode = ino_disk.i_mode;
                    inode->i_size = ino_disk.i_size;
                    inode->i_sb = dir->i_sb;
                    inode->i_op = &ext2_inode_ops;
                    inode->i_fop = &ext2_file_ops;
                    
                    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
                    if (!priv) {
                        kfree(inode);
                        kfree(buf);
                        return NULL;
                    }
                    __builtin_memcpy(priv->i_block, ino_disk.i_block, sizeof(priv->i_block));
                    inode->i_private = priv;
                    
                    dentry->d_inode = inode;
                    kfree(buf);
                    return dentry;
                }
            }
            offset += ent->rec_len;
        }
    }
    
    kfree(buf);
    extern dentry_t *devfs_lookup(struct inode *dir, struct dentry *dentry);
    return devfs_lookup(dir, dentry); /* Check devfs registered devices if not found in EXT2 */
}


static void ext2_sync_inode(ext2_fs_info_t *fs, struct inode *inode)
{
    u32 ino = inode->i_ino;
    u32 bg = (ino - 1) / fs->inodes_per_group;
    u32 index = (ino - 1) % fs->inodes_per_group;
    u32 inode_size = fs->sb->s_rev_level >= 1 ? fs->sb->s_inode_size : 128;
    
    u32 block = fs->bgdt[bg].bg_inode_table + (index * inode_size) / fs->block_size;
    u32 offset = (index * inode_size) % fs->block_size;
    
    void *buf = kzalloc(fs->block_size);
    if (!buf) return;
    
    ext2_read_block(fs, block, buf);
    
    ext2_inode_t *ino_disk = (ext2_inode_t *)((u8*)buf + offset);
    ino_disk->i_mode = inode->i_mode;
    ino_disk->i_size = inode->i_size;
    ino_disk->i_blocks = inode->i_blocks;
    
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    __builtin_memcpy(ino_disk->i_block, priv->i_block, sizeof(priv->i_block));
    
    ext2_write_block(fs, block, buf);
    kfree(buf);
}

static u32 ext2_alloc_zeroed_block(struct inode *inode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    u32 b = ext2_alloc_block(fs);
    if (b) {
        void *z = kzalloc(fs->block_size);
        if (z) {
            ext2_write_block(fs, b, z);
            kfree(z);
        }
        inode->i_blocks += fs->block_size / 512;
    }
    return b;
}

static u32 ext2_get_pblk(struct inode *inode, u32 lblk, bool allocate)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    u32 ptrs = fs->block_size / 4;

    if (lblk < 12) {
        if (!priv->i_block[lblk] && allocate) {
            priv->i_block[lblk] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        return priv->i_block[lblk];
    }
    lblk -= 12;

    if (lblk < ptrs) {
        if (!priv->i_block[12] && allocate) {
            priv->i_block[12] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[12]) return 0;

        u32 *ind = kzalloc(fs->block_size);
        ext2_read_block(fs, priv->i_block[12], ind);
        u32 pblk = ind[lblk];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind[lblk] = pblk;
                ext2_write_block(fs, priv->i_block[12], ind);
            }
        }
        kfree(ind);
        return pblk;
    }
    lblk -= ptrs;

    if (lblk < ptrs * ptrs) {
        if (!priv->i_block[13] && allocate) {
            priv->i_block[13] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[13]) return 0;

        u32 idx1 = lblk / ptrs;
        u32 idx2 = lblk % ptrs;

        u32 *ind1 = kzalloc(fs->block_size);
        ext2_read_block(fs, priv->i_block[13], ind1);
        u32 pblk1 = ind1[idx1];
        if (!pblk1 && allocate) {
            pblk1 = ext2_alloc_zeroed_block(inode);
            if (pblk1) {
                ind1[idx1] = pblk1;
                ext2_write_block(fs, priv->i_block[13], ind1);
            }
        }
        kfree(ind1);

        if (!pblk1) return 0;

        u32 *ind2 = kzalloc(fs->block_size);
        ext2_read_block(fs, pblk1, ind2);
        u32 pblk = ind2[idx2];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind2[idx2] = pblk;
                ext2_write_block(fs, pblk1, ind2);
            }
        }
        kfree(ind2);
        return pblk;
    }
    lblk -= ptrs * ptrs;

    if (lblk < ptrs * ptrs * ptrs) {
        if (!priv->i_block[14] && allocate) {
            priv->i_block[14] = ext2_alloc_zeroed_block(inode);
            ext2_sync_inode(fs, inode);
        }
        if (!priv->i_block[14]) return 0;

        u32 idx1 = lblk / (ptrs * ptrs);
        u32 idx2 = (lblk / ptrs) % ptrs;
        u32 idx3 = lblk % ptrs;

        u32 *ind1 = kzalloc(fs->block_size);
        ext2_read_block(fs, priv->i_block[14], ind1);
        u32 pblk1 = ind1[idx1];
        if (!pblk1 && allocate) {
            pblk1 = ext2_alloc_zeroed_block(inode);
            if (pblk1) {
                ind1[idx1] = pblk1;
                ext2_write_block(fs, priv->i_block[14], ind1);
            }
        }
        kfree(ind1);
        if (!pblk1) return 0;

        u32 *ind2 = kzalloc(fs->block_size);
        ext2_read_block(fs, pblk1, ind2);
        u32 pblk2 = ind2[idx2];
        if (!pblk2 && allocate) {
            pblk2 = ext2_alloc_zeroed_block(inode);
            if (pblk2) {
                ind2[idx2] = pblk2;
                ext2_write_block(fs, pblk1, ind2);
            }
        }
        kfree(ind2);
        if (!pblk2) return 0;

        u32 *ind3 = kzalloc(fs->block_size);
        ext2_read_block(fs, pblk2, ind3);
        u32 pblk = ind3[idx3];
        if (!pblk && allocate) {
            pblk = ext2_alloc_zeroed_block(inode);
            if (pblk) {
                ind3[idx3] = pblk;
                ext2_write_block(fs, pblk2, ind3);
            }
        }
        kfree(ind3);
        return pblk;
    }

    return 0;
}

static void ext2_free_indirect_blocks(ext2_fs_info_t *fs, u32 pblk, int level) {
    if (!pblk) return;
    if (level == 0) {
        ext2_free_block(fs, pblk);
        return;
    }
    u32 *ind = kzalloc(fs->block_size);
    if (!ind) return;
    ext2_read_block(fs, pblk, ind);
    u32 ptrs = fs->block_size / 4;
    for (u32 i = 0; i < ptrs; i++) {
        if (ind[i]) {
            ext2_free_indirect_blocks(fs, ind[i], level - 1);
        }
    }
    kfree(ind);
    ext2_free_block(fs, pblk);
}

void ext2_truncate(struct inode *inode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)inode->i_sb->s_fs_info;
    ext2_inode_info_t *priv = (ext2_inode_info_t *)inode->i_private;
    
    for (int i = 0; i < 12; i++) {
        if (priv->i_block[i]) {
            ext2_free_block(fs, priv->i_block[i]);
            priv->i_block[i] = 0;
        }
    }
    
    if (priv->i_block[12]) {
        ext2_free_indirect_blocks(fs, priv->i_block[12], 1);
        priv->i_block[12] = 0;
    }
    
    if (priv->i_block[13]) {
        ext2_free_indirect_blocks(fs, priv->i_block[13], 2);
        priv->i_block[13] = 0;
    }
    
    if (priv->i_block[14]) {
        ext2_free_indirect_blocks(fs, priv->i_block[14], 3);
        priv->i_block[14] = 0;
    }
    
    inode->i_size = 0;
    ext2_sync_inode(fs, inode);
}

/* File Read */
static s64 ext2_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    
    if (*offset >= filp->f_inode->i_size) return 0;
    if (len > filp->f_inode->i_size - *offset) {
        len = (size_t)(filp->f_inode->i_size - *offset);
    }
    
    void *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -(s64)ENOMEM;
    
    size_t bytes_read = 0;
    while (bytes_read < len) {
        u32 lblk = (*offset + bytes_read) / fs->block_size;
        u32 blk_offset = (*offset + bytes_read) % fs->block_size;
        
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
        
        size_t chunk = fs->block_size - blk_offset;
        if (chunk > len - bytes_read) chunk = len - bytes_read;
        
        if (pblk) {
            ext2_read_block(fs, pblk, block_buf);
            __builtin_memcpy((u8*)buf + bytes_read, (u8*)block_buf + blk_offset, chunk);
        } else {
            /* Hole */
            __builtin_memset((u8*)buf + bytes_read, 0, chunk);
        }
        bytes_read += chunk;
    }
    
    kfree(block_buf);
    *offset += bytes_read;
    return (s64)bytes_read;
}

static s64 ext2_file_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    
    void *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -(s64)ENOMEM;
    
    size_t bytes_written = 0;
    while (bytes_written < len) {
        u32 lblk = (*offset + bytes_written) / fs->block_size;
        u32 blk_offset = (*offset + bytes_written) % fs->block_size;
        
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, true);
        if (!pblk) break; /* Out of space */
        
        size_t chunk = fs->block_size - blk_offset;
        if (chunk > len - bytes_written) chunk = len - bytes_written;
        
        if (chunk < fs->block_size) {
            ext2_read_block(fs, pblk, block_buf);
        }
        
        __builtin_memcpy((u8*)block_buf + blk_offset, (const u8*)buf + bytes_written, chunk);
        ext2_write_block(fs, pblk, block_buf);
        
        bytes_written += chunk;
    }
    
    kfree(block_buf);
    
    if (bytes_written > 0) {
        *offset += bytes_written;
        if (*offset > filp->f_inode->i_size) {
            filp->f_inode->i_size = *offset;
        }
        rtc_time_t t;
        rtc_read_time(&t);
        u64 unix_t = rtc_to_unix_time(&t);
        filp->f_inode->i_mtime = unix_t;
        filp->f_inode->i_ctime = unix_t;
        ext2_sync_inode(fs, filp->f_inode);
    }
    
    return bytes_written == 0 && len > 0 ? -(s64)ENOSPC : (s64)bytes_written;
}

#include "../../userland/libc/include/sys/dirent.h"


/* Directory Read (getdents) */
static s64 ext2_file_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!S_ISDIR(filp->f_inode->i_mode)) return -(s64)ENOTDIR;
    if (!dirent_buf || len == 0) return -(s64)EINVAL;
    
    ext2_fs_info_t *fs = (ext2_fs_info_t *)filp->f_inode->i_sb->s_fs_info;
    if (*offset >= filp->f_inode->i_size) return 0;
    
    void *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -(s64)ENOMEM;
    
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    
    while (*offset < filp->f_inode->i_size) {
        u32 lblk = (*offset) / fs->block_size;
        u32 blk_offset = (*offset) % fs->block_size;
        
        u32 pblk = ext2_get_pblk(filp->f_inode, lblk, false);
        if (!pblk) {
            *offset = (u64)(lblk + 1) * fs->block_size;
            continue;
        }
        
        ext2_read_block(fs, pblk, block_buf);
        
        while (blk_offset < fs->block_size && *offset < filp->f_inode->i_size) {
            ext2_dir_entry_t *ent = (ext2_dir_entry_t *)((u8*)block_buf + blk_offset);
            if (ent->rec_len == 0) {
                *offset = (u64)(lblk + 1) * fs->block_size;
                break;
            }
            if (ent->inode == 0) {
                *offset += ent->rec_len;
                blk_offset += ent->rec_len;
                continue;
            }
            
            size_t dirent_size = ALIGN_UP(sizeof(struct linux_dirent64) + ent->name_len + 1, 8);
            if (written + dirent_size > len) {
                if (written == 0) {
                    kfree(block_buf);
                    return -(s64)EINVAL;
                }
                kfree(block_buf);
                return (s64)written;
            }
            
            struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
            d->d_ino = ent->inode;
            d->d_off = *offset + ent->rec_len;
            d->d_reclen = (unsigned short)dirent_size;
            
            d->d_type = DT_UNKNOWN;
            if (fs->sb->s_rev_level >= 1 && (fs->sb->s_feature_incompat & 0x2)) {
                switch (ent->file_type) {
                    case EXT2_FT_REG_FILE: d->d_type = DT_REG;     break;
                    case EXT2_FT_DIR:      d->d_type = DT_DIR;     break;
                    case EXT2_FT_CHRDEV:   d->d_type = DT_CHR;     break;
                    case EXT2_FT_BLKDEV:   d->d_type = DT_BLK;     break;
                    case EXT2_FT_FIFO:     d->d_type = DT_FIFO;    break;
                    case EXT2_FT_SOCK:     d->d_type = DT_SOCK;    break;
                    case EXT2_FT_SYMLINK:  d->d_type = DT_LNK;     break;
                    default:               d->d_type = DT_UNKNOWN; break;
                }
            }
            
            for (int i = 0; i < ent->name_len; i++) {
                d->d_name[i] = ent->name[i];
            }
            d->d_name[ent->name_len] = '\0';
            
            written += dirent_size;
            *offset += ent->rec_len;
            blk_offset += ent->rec_len;
        }
    }
    
    kfree(block_buf);
    return (s64)written;
}

static file_system_type_t ext2_fs_type = {
    .name = "ext2",
    .mount = ext2_mount,
    .next = NULL
};


/* ============================================================================
 * Directory Operations (Phase 3)
 * ============================================================================ */

static s64 ext2_add_dir_entry(struct inode *dir, u32 ino, const char *name, u8 file_type) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    int name_len = 0;
    while(name[name_len]) name_len++;
    
    u16 rec_len = ALIGN_UP(8 + name_len, 4);
    
    u32 lblk = 0;
    void *buf = kzalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;
    
    while (lblk < dir->i_size / fs->block_size) {
        u32 pblk = ext2_get_pblk(dir, lblk, false);
        if (!pblk) { lblk++; continue; }
        
        ext2_read_block(fs, pblk, buf);
        u32 offset = 0;
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = (ext2_dir_entry_t *)((u8*)buf + offset);
            if (ent->rec_len == 0) break;
            
            u16 real_len = 0;
            if (ent->inode != 0) {
                real_len = ALIGN_UP(8 + ent->name_len, 4);
            }
            
            u16 free_space = ent->rec_len - real_len;
            if (free_space >= rec_len) {
                if (ent->inode != 0) {
                    ent->rec_len = real_len;
                    ent = (ext2_dir_entry_t *)((u8*)buf + offset + real_len);
                    ent->rec_len = free_space;
                }
                
                ent->inode = ino;
                ent->name_len = name_len;
                ent->file_type = file_type;
                for (int i=0; i<name_len; i++) ent->name[i] = name[i];
                
                ext2_write_block(fs, pblk, buf);
                kfree(buf);
                return 0;
            }
            offset += ent->rec_len;
        }
        lblk++;
    }
    
    u32 new_lblk = dir->i_size / fs->block_size;
    u32 pblk = ext2_get_pblk(dir, new_lblk, true);
    if (!pblk) {
        kfree(buf);
        return -(s64)ENOSPC;
    }
    
    __builtin_memset(buf, 0, fs->block_size);
    ext2_dir_entry_t *ent = (ext2_dir_entry_t *)buf;
    ent->inode = ino;
    ent->rec_len = fs->block_size;
    ent->name_len = name_len;
    ent->file_type = file_type;
    for (int i=0; i<name_len; i++) ent->name[i] = name[i];
    
    ext2_write_block(fs, pblk, buf);
    kfree(buf);
    
    dir->i_size += fs->block_size;
    ext2_sync_inode(fs, dir);
    
    return 0;
}

static s64 ext2_remove_dir_entry(struct inode *dir, const char *name) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    int name_len = 0;
    while(name[name_len]) name_len++;
    
    u32 lblk = 0;
    void *buf = kzalloc(fs->block_size);
    if (!buf) return -(s64)ENOMEM;
    
    while (lblk < dir->i_size / fs->block_size) {
        u32 pblk = ext2_get_pblk(dir, lblk, false);
        if (!pblk) { lblk++; continue; }
        
        ext2_read_block(fs, pblk, buf);
        u32 offset = 0;
        ext2_dir_entry_t *prev = NULL;
        
        while (offset < fs->block_size) {
            ext2_dir_entry_t *ent = (ext2_dir_entry_t *)((u8*)buf + offset);
            if (ent->rec_len == 0) break;
            
            if (ent->inode != 0 && ent->name_len == name_len) {
                bool match = true;
                for (int i=0; i<name_len; i++) {
                    if (ent->name[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    ent->inode = 0; 
                    if (prev) {
                        prev->rec_len += ent->rec_len;
                    }
                    ext2_write_block(fs, pblk, buf);
                    kfree(buf);
                    return 0;
                }
            }
            prev = ent;
            offset += ent->rec_len;
        }
        lblk++;
    }
    
    kfree(buf);
    return -(s64)ENOENT;
}

static s64 ext2_create(struct inode *dir, struct dentry *dentry, u32 mode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;
    
    struct inode *inode = kzalloc(sizeof(struct inode));
    inode->i_ino = ino;
    inode->i_mode = mode | S_IFREG;
    inode->i_size = 0;
    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;
    
    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_t = rtc_to_unix_time(&t);
    inode->i_atime = unix_t;
    inode->i_mtime = unix_t;
    inode->i_ctime = unix_t;
    
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    inode->i_private = priv;
    
    ext2_sync_inode(fs, inode);
    
    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_REG_FILE);
    if (err < 0) {
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    
    dentry->d_inode = inode;
    return 0;
}

static s64 ext2_mkdir(struct inode *dir, struct dentry *dentry, u32 mode) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;
    
    struct inode *inode = kzalloc(sizeof(struct inode));
    inode->i_ino = ino;
    inode->i_mode = mode | S_IFDIR;
    inode->i_size = fs->block_size;
    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;
    
    rtc_time_t t;
    rtc_read_time(&t);
    u64 unix_t = rtc_to_unix_time(&t);
    inode->i_atime = unix_t;
    inode->i_mtime = unix_t;
    inode->i_ctime = unix_t;
    
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    inode->i_private = priv;
    
    u32 dir_blk = ext2_alloc_zeroed_block(inode);
    if (!dir_blk) {
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return -(s64)ENOSPC;
    }
    priv->i_block[0] = dir_blk;
    
    void *buf = kzalloc(fs->block_size);
    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)buf;
    dot->inode = ino;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';
    dot->rec_len = 12;
    
    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)((u8*)buf + 12);
    dotdot->inode = dir->i_ino;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->rec_len = fs->block_size - 12;
    
    ext2_write_block(fs, dir_blk, buf);
    kfree(buf);
    
    ext2_sync_inode(fs, inode);
    
    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_DIR);
    if (err < 0) {
        ext2_free_block(fs, dir_blk);
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    
    dentry->d_inode = inode;
    return 0;
}

static s64 ext2_unlink(struct inode *dir, struct dentry *dentry) {
    if (!dentry->d_inode) return -(s64)ENOENT;
    if (S_ISDIR(dentry->d_inode->i_mode)) return -(s64)EISDIR;
    
    s64 err = ext2_remove_dir_entry(dir, dentry->d_name);
    if (err < 0) return err;
    
    ext2_truncate(dentry->d_inode);
    ext2_free_inode((ext2_fs_info_t *)dir->i_sb->s_fs_info, dentry->d_inode->i_ino);
    
    if (dentry->d_inode->i_private) kfree(dentry->d_inode->i_private);
    kfree(dentry->d_inode);
    dentry->d_inode = NULL; /* Turn into negative dentry */
    
    return 0;
}

static s64 ext2_rmdir(struct inode *dir, struct dentry *dentry) {
    if (!dentry->d_inode) return -(s64)ENOENT;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -(s64)ENOTDIR;
    
    // In a real OS we should check if directory is empty. For now assume it is.
    s64 err = ext2_remove_dir_entry(dir, dentry->d_name);
    if (err < 0) return err;
    
    ext2_truncate(dentry->d_inode);
    ext2_free_inode((ext2_fs_info_t *)dir->i_sb->s_fs_info, dentry->d_inode->i_ino);
    
    if (dentry->d_inode->i_private) kfree(dentry->d_inode->i_private);
    kfree(dentry->d_inode);
    dentry->d_inode = NULL; /* Turn into negative dentry */
    
    return 0;
}

static s64 ext2_rename(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry) {
    if (!old_dentry->d_inode) return -(s64)ENOENT;
    
    u8 ftype = S_ISDIR(old_dentry->d_inode->i_mode) ? EXT2_FT_DIR : EXT2_FT_REG_FILE;
    s64 err = ext2_add_dir_entry(new_dir, old_dentry->d_inode->i_ino, new_dentry->d_name, ftype);
    if (err < 0) return err;

    ext2_remove_dir_entry(old_dir, old_dentry->d_name);

    /* Update the dcache so the new dentry points to the moved inode */
    new_dentry->d_inode = old_dentry->d_inode;
    return 0;
}

static s64 ext2_symlink(struct inode *dir, struct dentry *dentry, const char *symname) {
    ext2_fs_info_t *fs = (ext2_fs_info_t *)dir->i_sb->s_fs_info;
    
    u32 ino = ext2_alloc_inode(fs);
    if (!ino) return -(s64)ENOSPC;
    
    struct inode *inode = kzalloc(sizeof(struct inode));
    inode->i_ino = ino;
    inode->i_mode = 0777 | S_IFLNK;
    
    int sym_len = 0;
    while(symname[sym_len]) sym_len++;
    inode->i_size = sym_len;
    
    inode->i_sb = dir->i_sb;
    inode->i_op = &ext2_inode_ops;
    inode->i_fop = &ext2_file_ops;
    
    ext2_inode_info_t *priv = kzalloc(sizeof(ext2_inode_info_t));
    inode->i_private = priv;
    
    if (sym_len <= 60) {
        // Fast symlink
        for (int i = 0; i < sym_len; i++) {
            ((char*)priv->i_block)[i] = symname[i];
        }
    } else {
        // Slow symlink
        u32 blk = ext2_alloc_zeroed_block(inode);
        if (!blk) {
            kfree(priv);
            kfree(inode);
            ext2_free_inode(fs, ino);
            return -(s64)ENOSPC;
        }
        priv->i_block[0] = blk;
        void *buf = kzalloc(fs->block_size);
        if (!buf) {
            ext2_truncate(inode);
            kfree(priv);
            kfree(inode);
            ext2_free_inode(fs, ino);
            return -(s64)ENOMEM;
        }
        for (int i = 0; i < sym_len; i++) ((char*)buf)[i] = symname[i];
        ext2_write_block(fs, blk, buf);
        kfree(buf);
    }
    
    ext2_sync_inode(fs, inode);
    s64 err = ext2_add_dir_entry(dir, ino, dentry->d_name, EXT2_FT_SYMLINK);
    if (err < 0) {
        ext2_truncate(inode);
        ext2_free_inode(fs, ino);
        kfree(priv);
        kfree(inode);
        return err;
    }
    dentry->d_inode = inode;
    return 0;
}

void ext2_init(void)
{
    vfs_register_fs(&ext2_fs_type);
}

/* ============================================================================
 * Allocation Primitives
 * ============================================================================ */

static inline void ext2_set_bit(u8 *bitmap, u32 index) {
    bitmap[index / 8] |= (1 << (index % 8));
}

static inline void ext2_clear_bit(u8 *bitmap, u32 index) {
    bitmap[index / 8] &= ~(1 << (index % 8));
}

static inline bool ext2_test_bit(u8 *bitmap, u32 index) {
    return (bitmap[index / 8] & (1 << (index % 8))) != 0;
}

static u8 *ext2_get_block_bitmap(ext2_fs_info_t *fs, u32 bg) {
    if (!fs->block_bitmaps[bg]) {
        fs->block_bitmaps[bg] = kzalloc(fs->block_size);
        ext2_read_block(fs, fs->bgdt[bg].bg_block_bitmap, fs->block_bitmaps[bg]);
    }
    return fs->block_bitmaps[bg];
}

static u8 *ext2_get_inode_bitmap(ext2_fs_info_t *fs, u32 bg) {
    if (!fs->inode_bitmaps[bg]) {
        fs->inode_bitmaps[bg] = kzalloc(fs->block_size);
        ext2_read_block(fs, fs->bgdt[bg].bg_inode_bitmap, fs->inode_bitmaps[bg]);
    }
    return fs->inode_bitmaps[bg];
}

static void ext2_sync_bg(ext2_fs_info_t *fs, u32 bg) {
    if (fs->block_bitmaps[bg]) {
        ext2_write_block(fs, fs->bgdt[bg].bg_block_bitmap, fs->block_bitmaps[bg]);
    }
    if (fs->inode_bitmaps[bg]) {
        ext2_write_block(fs, fs->bgdt[bg].bg_inode_bitmap, fs->inode_bitmaps[bg]);
    }
    
    u32 bg_desc_per_block = fs->block_size / sizeof(ext2_bg_descriptor_t);
    u32 bg_block_offset = bg / bg_desc_per_block;
    u32 bgdt_block = (fs->block_size == 1024 ? 2 : 1) + bg_block_offset;
    ext2_write_block(fs, bgdt_block, (u8*)fs->bgdt + (bg_block_offset * fs->block_size));
    
    u32 sb_block = fs->block_size == 1024 ? 1 : 0;
    ext2_write_block(fs, sb_block, fs->sb);
}

u32 ext2_alloc_block(ext2_fs_info_t *fs) {
    for (u32 bg = 0; bg < fs->block_groups_count; bg++) {
        if (fs->bgdt[bg].bg_free_blocks_count == 0) continue;
        
        u8 *bitmap = ext2_get_block_bitmap(fs, bg);
        for (u32 bit = 0; bit < fs->blocks_per_group; bit++) {
            if (!ext2_test_bit(bitmap, bit)) {
                ext2_set_bit(bitmap, bit);
                fs->bgdt[bg].bg_free_blocks_count--;
                fs->sb->s_free_blocks_count--;
                ext2_sync_bg(fs, bg);
                return (bg * fs->blocks_per_group) + fs->sb->s_first_data_block + bit;
            }
        }
    }
    return 0; // Out of space
}

void ext2_free_block(ext2_fs_info_t *fs, u32 block) {
    if (block < fs->sb->s_first_data_block || block >= fs->sb->s_blocks_count) return;
    
    u32 rel_block = block - fs->sb->s_first_data_block;
    u32 bg = rel_block / fs->blocks_per_group;
    u32 bit = rel_block % fs->blocks_per_group;
    
    u8 *bitmap = ext2_get_block_bitmap(fs, bg);
    if (ext2_test_bit(bitmap, bit)) {
        ext2_clear_bit(bitmap, bit);
        fs->bgdt[bg].bg_free_blocks_count++;
        fs->sb->s_free_blocks_count++;
        ext2_sync_bg(fs, bg);
    }
}

u32 ext2_alloc_inode(ext2_fs_info_t *fs) {
    for (u32 bg = 0; bg < fs->block_groups_count; bg++) {
        if (fs->bgdt[bg].bg_free_inodes_count == 0) continue;
        
        u8 *bitmap = ext2_get_inode_bitmap(fs, bg);
        for (u32 bit = 0; bit < fs->inodes_per_group; bit++) {
            if (!ext2_test_bit(bitmap, bit)) {
                ext2_set_bit(bitmap, bit);
                fs->bgdt[bg].bg_free_inodes_count--;
                fs->sb->s_free_inodes_count--;
                ext2_sync_bg(fs, bg);
                return (bg * fs->inodes_per_group) + 1 + bit;
            }
        }
    }
    return 0; // Out of inodes
}

void ext2_free_inode(ext2_fs_info_t *fs, u32 ino) {
    if (ino < 1 || ino > fs->sb->s_inodes_count) return;
    
    u32 bg = (ino - 1) / fs->inodes_per_group;
    u32 bit = (ino - 1) % fs->inodes_per_group;
    
    u8 *bitmap = ext2_get_inode_bitmap(fs, bg);
    if (ext2_test_bit(bitmap, bit)) {
        ext2_clear_bit(bitmap, bit);
        fs->bgdt[bg].bg_free_inodes_count++;
        fs->sb->s_free_inodes_count++;
        ext2_sync_bg(fs, bg);
    }
}
