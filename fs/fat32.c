/* ============================================================================
 * AzamiOS — FAT32 Filesystem Driver Implementation
 * File: fs/fat32.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "fat32.h"
#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../drivers/char/console.h"
#include "../include/azami/defs.h"
#include "../kernel/lib/string.h"
#include "../userland/libc/include/sys/dirent.h"

static spinlock_t g_fat32_lock = SPINLOCK_INIT;

typedef struct {
    fat32_volume_t *vol;
    u32 start_cluster;
    u32 current_cluster;
    u32 file_size;
} fat32_node_data_t;

static u64 lba_for_cluster(fat32_volume_t *vol, u32 cluster)
{
    if (cluster < 2) return vol->first_data_sector;
    return vol->first_data_sector + (cluster - 2) * vol->sectors_per_cluster;
}

static u32 get_next_cluster(fat32_volume_t *vol, u32 cluster)
{
    u32 fat_offset = cluster * 4;
    u32 fat_sector = vol->first_fat_sector + (fat_offset / vol->bytes_per_sector);
    u32 ent_offset = fat_offset % vol->bytes_per_sector;

    u8 sector_buf[512];
    if (vol->bdev->ops->read_sectors(vol->bdev, fat_sector, 1, sector_buf) <= 0) {
        return FAT32_CLUSTER_BAD;
    }

    u32 next = *(u32 *)(sector_buf + ent_offset) & 0x0FFFFFFFU;
    return next;
}

static void set_next_cluster(fat32_volume_t *vol, u32 cluster, u32 next)
{
    u32 fat_offset = cluster * 4;
    u32 fat_sector = vol->first_fat_sector + (fat_offset / vol->bytes_per_sector);
    u32 ent_offset = fat_offset % vol->bytes_per_sector;

    spinlock_lock(&g_fat32_lock);
    u8 sector_buf[512];
    if (vol->bdev->ops->read_sectors(vol->bdev, fat_sector, 1, sector_buf) <= 0) {
        spinlock_unlock(&g_fat32_lock);
        return;
    }

    u32 *val = (u32 *)(sector_buf + ent_offset);
    *val = (*val & 0xF0000000U) | (next & 0x0FFFFFFFU);

    vol->bdev->ops->write_sectors(vol->bdev, fat_sector, 1, sector_buf);
    spinlock_unlock(&g_fat32_lock);
}

static u32 alloc_cluster(fat32_volume_t *vol)
{
    u8 sector_buf[512];
    u32 fat_sectors = (vol->first_data_sector - vol->first_fat_sector);
    u32 entries_per_sector = vol->bytes_per_sector / 4;

    spinlock_lock(&g_fat32_lock);
    for (u32 s = 0; s < fat_sectors; s++) {
        if (vol->bdev->ops->read_sectors(vol->bdev, vol->first_fat_sector + s, 1, sector_buf) <= 0) continue;
        u32 *entries = (u32 *)sector_buf;
        for (u32 i = 0; i < entries_per_sector; i++) {
            u32 cl = s * entries_per_sector + i;
            if (cl < 2) continue;
            if ((entries[i] & 0x0FFFFFFFU) == FAT32_CLUSTER_FREE) {
                entries[i] = (entries[i] & 0xF0000000U) | FAT32_CLUSTER_EOF;
                vol->bdev->ops->write_sectors(vol->bdev, vol->first_fat_sector + s, 1, sector_buf);

                u8 zero_buf[512];
                __builtin_memset(zero_buf, 0, 512);
                u64 lba = lba_for_cluster(vol, cl);
                for (u32 sc = 0; sc < vol->sectors_per_cluster; sc++) {
                    vol->bdev->ops->write_sectors(vol->bdev, lba + sc, 1, zero_buf);
                }
                spinlock_unlock(&g_fat32_lock);
                return cl;
            }
        }
    }
    spinlock_unlock(&g_fat32_lock);
    return 0;
}

static s64 fat32_file_read(file_t *filp, void *buf, size_t size, u64 *offset)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_private || !buf) return -EINVAL;
    fat32_node_data_t *nd = (fat32_node_data_t *)filp->f_inode->i_private;
    fat32_volume_t *vol = nd->vol;

    u64 off = *offset;
    if (off >= nd->file_size) return 0;
    if (off + size > nd->file_size) {
        size = (size_t)(nd->file_size - off);
    }

    u32 cl = nd->start_cluster;
    u32 skip_clusters = (u32)(off / vol->bytes_per_cluster);
    for (u32 i = 0; i < skip_clusters && cl >= 2 && cl < FAT32_CLUSTER_EOF; i++) {
        cl = get_next_cluster(vol, cl);
    }

    size_t total_read = 0;
    u32 cluster_offset = (u32)(off % vol->bytes_per_cluster);
    u8 *sector_buf = (u8 *)kmalloc(vol->bytes_per_sector);
    if (!sector_buf) return -ENOMEM;

    while (total_read < size && cl >= 2 && cl < FAT32_CLUSTER_EOF) {
        u64 base_lba = lba_for_cluster(vol, cl);
        u32 start_sec = cluster_offset / vol->bytes_per_sector;
        u32 sec_offset = cluster_offset % vol->bytes_per_sector;

        for (u32 s = start_sec; s < vol->sectors_per_cluster && total_read < size; s++) {
            if (vol->bdev->ops->read_sectors(vol->bdev, base_lba + s, 1, sector_buf) <= 0) {
                kfree(sector_buf);
                return total_read > 0 ? (s64)total_read : -EIO;
            }

            size_t to_copy = vol->bytes_per_sector - sec_offset;
            if (to_copy > (size - total_read)) to_copy = size - total_read;

            memcpy((u8 *)buf + total_read, sector_buf + sec_offset, to_copy);
            total_read += to_copy;
            sec_offset = 0;
        }

        cluster_offset = 0;
        cl = get_next_cluster(vol, cl);
    }

    kfree(sector_buf);
    *offset += total_read;
    return (s64)total_read;
}

static s64 fat32_file_write(file_t *filp, const void *buf, size_t size, u64 *offset)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_private || !buf) return -EINVAL;
    fat32_node_data_t *nd = (fat32_node_data_t *)filp->f_inode->i_private;
    fat32_volume_t *vol = nd->vol;

    u64 off = *offset;
    if (nd->start_cluster < 2) {
        nd->start_cluster = alloc_cluster(vol);
        if (nd->start_cluster < 2) return -ENOSPC;
    }

    u32 cl = nd->start_cluster;
    u32 skip_clusters = (u32)(off / vol->bytes_per_cluster);
    for (u32 i = 0; i < skip_clusters; i++) {
        u32 next = get_next_cluster(vol, cl);
        if (next < 2 || next >= FAT32_CLUSTER_EOF) {
            next = alloc_cluster(vol);
            if (next < 2) return -ENOSPC;
            set_next_cluster(vol, cl, next);
        }
        cl = next;
    }

    size_t total_written = 0;
    u32 cluster_offset = (u32)(off % vol->bytes_per_cluster);
    u8 *sector_buf = (u8 *)kmalloc(vol->bytes_per_sector);
    if (!sector_buf) return -ENOMEM;

    while (total_written < size) {
        u64 base_lba = lba_for_cluster(vol, cl);
        u32 start_sec = cluster_offset / vol->bytes_per_sector;
        u32 sec_offset = cluster_offset % vol->bytes_per_sector;

        for (u32 s = start_sec; s < vol->sectors_per_cluster && total_written < size; s++) {
            if (sec_offset > 0 || (size - total_written) < vol->bytes_per_sector) {
                vol->bdev->ops->read_sectors(vol->bdev, base_lba + s, 1, sector_buf);
            }

            size_t to_copy = vol->bytes_per_sector - sec_offset;
            if (to_copy > (size - total_written)) to_copy = size - total_written;

            memcpy(sector_buf + sec_offset, (const u8 *)buf + total_written, to_copy);
            if (vol->bdev->ops->write_sectors(vol->bdev, base_lba + s, 1, sector_buf) <= 0) {
                kfree(sector_buf);
                return total_written > 0 ? (s64)total_written : -EIO;
            }

            total_written += to_copy;
            sec_offset = 0;
        }

        cluster_offset = 0;
        if (total_written < size) {
            u32 next = get_next_cluster(vol, cl);
            if (next < 2 || next >= FAT32_CLUSTER_EOF) {
                next = alloc_cluster(vol);
                if (next < 2) {
                    kfree(sector_buf);
                    return total_written > 0 ? (s64)total_written : -ENOSPC;
                }
                set_next_cluster(vol, cl, next);
            }
            cl = next;
        }
    }

    kfree(sector_buf);
    *offset += total_written;
    if (*offset > nd->file_size) {
        nd->file_size = (u32)*offset;
        filp->f_inode->i_size = (s64)nd->file_size;
    }
    return (s64)total_written;
}

static s64 fat32_dir_readdir(file_t *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!filp || !filp->f_inode || !filp->f_inode->i_private || !dirent_buf) return -EINVAL;
    fat32_node_data_t *nd = (fat32_node_data_t *)filp->f_inode->i_private;
    fat32_volume_t *vol = nd->vol;

    u8 *sector_buf = (u8 *)kmalloc(vol->bytes_per_sector);
    if (!sector_buf) return -ENOMEM;

    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 cur_idx = 0;
    u32 cl = nd->start_cluster;

    while (cl >= 2 && cl < FAT32_CLUSTER_EOF) {
        u64 base_lba = lba_for_cluster(vol, cl);
        for (u32 s = 0; s < vol->sectors_per_cluster; s++) {
            if (vol->bdev->ops->read_sectors(vol->bdev, base_lba + s, 1, sector_buf) <= 0) {
                kfree(sector_buf);
                return (s64)written;
            }

            fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;
            u32 count = vol->bytes_per_sector / sizeof(fat32_dirent_t);
            for (u32 i = 0; i < count; i++) {
                if ((u8)entries[i].name[0] == 0x00) {
                    kfree(sector_buf);
                    return (s64)written;
                }
                if ((u8)entries[i].name[0] == 0xE5 || (entries[i].attr & FAT_ATTR_VOLUME_ID)) {
                    continue;
                }

                if (cur_idx < *offset) {
                    cur_idx++;
                    continue;
                }

                char name[16];
                int nlen = 0;
                for (int c = 0; c < 8 && entries[i].name[c] != ' '; c++) {
                    name[nlen++] = entries[i].name[c];
                }
                if (entries[i].ext[0] != ' ') {
                    name[nlen++] = '.';
                    for (int c = 0; c < 3 && entries[i].ext[c] != ' '; c++) {
                        name[nlen++] = entries[i].ext[c];
                    }
                }
                name[nlen] = '\0';

                size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + nlen + 1, 8);
                if (written + reclen > len) {
                    kfree(sector_buf);
                    return written > 0 ? (s64)written : -EINVAL;
                }

                struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
                d->d_ino = (entries[i].cluster_high << 16) | entries[i].cluster_low;
                if (d->d_ino == 0) d->d_ino = 1;
                d->d_off = (long long)(cur_idx + 1);
                d->d_reclen = (unsigned short)reclen;
                d->d_type = (entries[i].attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
                memcpy(d->d_name, name, nlen);
                d->d_name[nlen] = '\0';

                written += reclen;
                cur_idx++;
                *offset = cur_idx;
            }
        }
        cl = get_next_cluster(vol, cl);
    }

    kfree(sector_buf);
    return (s64)written;
}

static file_operations_t g_fat32_file_fops = {
    .read = fat32_file_read,
    .write = fat32_file_write,
};

static file_operations_t g_fat32_dir_fops = {
    .readdir = fat32_dir_readdir,
};

static dentry_t *fat32_lookup(inode_t *dir, dentry_t *dentry)
{
    if (!dir || !dir->i_private || !dentry) return NULL;
    fat32_node_data_t *nd = (fat32_node_data_t *)dir->i_private;
    fat32_volume_t *vol = nd->vol;

    u8 *sector_buf = (u8 *)kmalloc(vol->bytes_per_sector);
    if (!sector_buf) return dentry;

    u32 cl = nd->start_cluster;
    while (cl >= 2 && cl < FAT32_CLUSTER_EOF) {
        u64 base_lba = lba_for_cluster(vol, cl);
        for (u32 s = 0; s < vol->sectors_per_cluster; s++) {
            if (vol->bdev->ops->read_sectors(vol->bdev, base_lba + s, 1, sector_buf) <= 0) {
                kfree(sector_buf);
                return dentry;
            }

            fat32_dirent_t *entries = (fat32_dirent_t *)sector_buf;
            u32 count = vol->bytes_per_sector / sizeof(fat32_dirent_t);
            for (u32 i = 0; i < count; i++) {
                if ((u8)entries[i].name[0] == 0x00) {
                    kfree(sector_buf);
                    return dentry;
                }
                if ((u8)entries[i].name[0] == 0xE5 || (entries[i].attr & FAT_ATTR_VOLUME_ID)) continue;

                char name[16];
                int nlen = 0;
                for (int c = 0; c < 8 && entries[i].name[c] != ' '; c++) name[nlen++] = entries[i].name[c];
                if (entries[i].ext[0] != ' ') {
                    name[nlen++] = '.';
                    for (int c = 0; c < 3 && entries[i].ext[c] != ' '; c++) name[nlen++] = entries[i].ext[c];
                }
                name[nlen] = '\0';

                if (strcmp(dentry->d_name, name) == 0) {
                    inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
                    fat32_node_data_t *child_nd = (fat32_node_data_t *)kzalloc(sizeof(fat32_node_data_t));
                    if (inode && child_nd) {
                        child_nd->vol = vol;
                        child_nd->start_cluster = (entries[i].cluster_high << 16) | entries[i].cluster_low;
                        child_nd->file_size = entries[i].file_size;

                        inode->i_ino = child_nd->start_cluster ? child_nd->start_cluster : 2;
                        inode->i_size = (s64)child_nd->file_size;
                        inode->i_sb = dir->i_sb;
                        inode->i_private = child_nd;

                        if (entries[i].attr & FAT_ATTR_DIRECTORY) {
                            inode->i_mode = S_IFDIR | 0755;
                            inode->i_fop = &g_fat32_dir_fops;
                        } else {
                            inode->i_mode = S_IFREG | 0644;
                            inode->i_fop = &g_fat32_file_fops;
                        }
                        dentry->d_inode = inode;
                    }
                    kfree(sector_buf);
                    return dentry;
                }
            }
        }
        cl = get_next_cluster(vol, cl);
    }

    kfree(sector_buf);
    return dentry;
}

static inode_operations_t g_fat32_inode_ops = {
    .lookup = fat32_lookup,
};

s64 fat32_format_ramdisk(block_dev_t *dev, u32 size_mb)
{
    if (!dev || !dev->ops) return -EINVAL;
    if (size_mb < 33) size_mb = 33; /* FAT32 minimum */

    u8 *buf = (u8 *)kzalloc(512);
    if (!buf) return -ENOMEM;

    fat32_bpb_t *bpb = (fat32_bpb_t *)buf;
    bpb->jmp_boot[0] = 0xEB; bpb->jmp_boot[1] = 0x58; bpb->jmp_boot[2] = 0x90;
    memcpy(bpb->oem_name, "AZAMIOS ", 8);
    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = 8;
    bpb->reserved_sectors = 32;
    bpb->fat_count = 2;
    bpb->media_type = 0xF8;
    bpb->total_sectors_32 = (size_mb * 1024 * 1024) / 512;

    u32 total_clusters = bpb->total_sectors_32 / bpb->sectors_per_cluster;
    bpb->fat_size_32 = ((total_clusters * 4) + 511) / 512;
    bpb->root_cluster = 2;
    bpb->fs_info_sector = 1;
    bpb->backup_boot_sector = 6;
    bpb->boot_sig = 0x29;
    bpb->vol_id = 0x12345678;
    memcpy(bpb->vol_label, "AZAMI_FAT32", 11);
    memcpy(bpb->fs_type, "FAT32   ", 8);

    buf[510] = 0x55; buf[511] = 0xAA;
    dev->ops->write_sectors(dev, 0, 1, buf);

    /* Initialize FAT tables */
    memset(buf, 0, 512);
    u32 *fat = (u32 *)buf;
    fat[0] = 0x0FFFFFF8; /* Media descriptor */
    fat[1] = 0x0FFFFFFF; /* EOC */
    fat[2] = 0x0FFFFFFF; /* Root directory EOF */

    u32 fat1_sec = bpb->reserved_sectors;
    u32 fat2_sec = bpb->reserved_sectors + bpb->fat_size_32;
    dev->ops->write_sectors(dev, fat1_sec, 1, buf);
    dev->ops->write_sectors(dev, fat2_sec, 1, buf);

    kfree(buf);
    pr_debug("[FAT32] Formatted device '%s' as FAT32 volume (%u MB)\n", dev->name, size_mb);
    return 0;
}

s64 fat32_mount(block_dev_t *dev, const char *mount_point)
{
    if (!dev || !dev->ops || !mount_point) return -EINVAL;

    u8 sector_buf[512];
    if (dev->ops->read_sectors(dev, 0, 1, sector_buf) <= 0) return -EIO;

    fat32_bpb_t *bpb = (fat32_bpb_t *)sector_buf;
    if (sector_buf[510] != 0x55 || sector_buf[511] != 0xAA) {
        fat32_format_ramdisk(dev, (u32)(dev->sector_count * dev->sector_size / (1024 * 1024)));
        if (dev->ops->read_sectors(dev, 0, 1, sector_buf) <= 0) return -EIO;
        bpb = (fat32_bpb_t *)sector_buf;
    }

    fat32_volume_t *vol = (fat32_volume_t *)kzalloc(sizeof(fat32_volume_t));
    if (!vol) return -EINVAL;

    vol->bdev = dev;
    vol->bytes_per_sector = bpb->bytes_per_sector ? bpb->bytes_per_sector : 512;
    vol->sectors_per_cluster = bpb->sectors_per_cluster ? bpb->sectors_per_cluster : 8;
    vol->bytes_per_cluster = vol->bytes_per_sector * vol->sectors_per_cluster;
    vol->first_fat_sector = bpb->reserved_sectors;
    vol->first_data_sector = bpb->reserved_sectors + (bpb->fat_count * bpb->fat_size_32);
    vol->root_cluster = bpb->root_cluster ? bpb->root_cluster : 2;

    fat32_node_data_t *nd = (fat32_node_data_t *)kzalloc(sizeof(fat32_node_data_t));
    if (!nd) {
        kfree(vol);
        return -EINVAL;
    }
    nd->vol = vol;
    nd->start_cluster = vol->root_cluster;
    nd->file_size = 0;

    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    dentry_t *root_dentry = dcache_alloc(NULL, "/");

    sb->s_magic = 0x4853;
    sb->s_blocksize = vol->bytes_per_cluster;
    sb->s_fs_info = vol;

    root_inode->i_ino = 2;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &g_fat32_inode_ops;
    root_inode->i_fop = &g_fat32_dir_fops;
    root_inode->i_private = nd;

    root_dentry->d_inode = root_inode;
    root_dentry->d_sb = sb;
    sb->s_root = root_dentry;

    s64 ret = vfs_mount(dev->name, mount_point, "fat32", sb);
    if (ret == 0) {
        pr_debug("[FAT32] Successfully mounted FAT32 volume '%s' at '%s'\n", dev->name, mount_point);
    }
    return ret;
}

static s64 fat32_mount_fs(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)fs_type; (void)dir_name;
    if (data) return 0;
    block_dev_t *dev = block_dev_get(dev_name);
    if (!dev) return -ENOENT;
    return fat32_mount(dev, dir_name);
}

file_system_type_t g_fat32_type = {
    .name = "fat32",
    .mount = fat32_mount_fs,
    .next = NULL,
};

void fat32_init(void)
{
    vfs_register_fs(&g_fat32_type);
}
