/* ============================================================================
 * AzamiOS — Devpts Virtual Filesystem Implementation
 * File: fs/devpts.c
 *
 * Provides the /dev/pts virtual filesystem for UNIX98 slave pseudo-terminals.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "devpts.h"
#include "vfs.h"
#include "../drivers/char/pty.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../include/azami/defs.h"
#include "../userland/libc/include/sys/dirent.h"

#define DEVPTS_SUPER_MAGIC 0x1cd1

static struct dentry *devpts_lookup(struct inode *dir, struct dentry *dentry);
static s64 devpts_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);

static inode_operations_t g_devpts_dir_inode_ops = {
    .lookup = devpts_lookup,
};

static file_operations_t g_devpts_dir_file_ops = {
    .readdir = devpts_dir_readdir,
};

static struct dentry *devpts_lookup(struct inode *dir, struct dentry *dentry)
{
    if (!dir || !dentry) return dentry;

    const char *name = dentry->d_name;
    if (name[0] >= '0' && name[0] <= '9') {
        int id = 0;
        const char *p = name;
        while (*p >= '0' && *p <= '9') { id = id * 10 + (*p - '0'); p++; }
        pty_pair_t *pty = pty_get_pair(id);
        if (pty) {
            inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
            if (inode) {
                inode->i_ino = (u64)(id + 2);
                inode->i_mode = S_IFCHR | 0620;
                inode->i_sb = dir->i_sb;
                inode->i_fop = pty_get_slave_fops();
                inode->i_private = pty;
                dentry->d_inode = inode;
            }
        }
    }

    return dentry;
}

static s64 devpts_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!filp || !dirent_buf || len == 0 || !offset) return -(s64)EINVAL;

    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    /* Build entry list: ., .., then all active PTYs */
    char names[PTY_MAX_PAIRS + 2][8];
    u64 inos[PTY_MAX_PAIRS + 2];
    u8 types[PTY_MAX_PAIRS + 2];
    u64 total = 0;

    strncpy(names[0], ".", sizeof(names[0]) - 1); inos[0] = 1; types[0] = DT_DIR; total++;
    strncpy(names[1], "..", sizeof(names[1]) - 1); inos[1] = 1; types[1] = DT_DIR; total++;

    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (pty_get_pair(i)) {
            snprintf(names[total], sizeof(names[total]), "%d", i);
            inos[total] = (u64)(i + 2);
            types[total] = DT_CHR;
            total++;
        }
    }

    while (idx < total) {
        size_t nlen = strlen(names[idx]);
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + nlen + 1, 8);
        if (written + reclen > len) {
            if (written == 0) return -(s64)EINVAL;
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
        d->d_ino = inos[idx];
        d->d_off = idx + 1;
        d->d_reclen = (unsigned short)reclen;
        d->d_type = types[idx];
        memcpy(d->d_name, names[idx], nlen + 1);

        written += reclen;
        idx++;
    }

    *offset = idx;
    return (s64)written;
}

static s64 devpts_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)fs_type; (void)dev_name; (void)dir_name; (void)data;

    dentry_t *mountpoint = NULL;
    s64 err = vfs_path_lookup(dir_name, &mountpoint);
    if (err < 0 || !mountpoint || !mountpoint->d_inode) {
        if (mountpoint && !mountpoint->d_inode) kfree(mountpoint);
        return -(s64)ENOENT;
    }

    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    if (!sb) return -(s64)ENOMEM;

    sb->s_magic = DEVPTS_SUPER_MAGIC;
    sb->s_blocksize = 4096;

    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!root_inode) {
        kfree(sb);
        return -(s64)ENOMEM;
    }

    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &g_devpts_dir_inode_ops;
    root_inode->i_fop = &g_devpts_dir_file_ops;

    mountpoint->d_inode = root_inode;
    mountpoint->d_sb = sb;
    sb->s_root = mountpoint;

    pr_debug("[DEVPTS] Mounted devpts on %s successfully.\n", dir_name);
    return 0;
}

static file_system_type_t g_devpts_type = {
    .name = "devpts",
    .mount = devpts_mount,
};

void devpts_init(void)
{
    vfs_register_fs(&g_devpts_type);
}
