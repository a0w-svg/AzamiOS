/* ============================================================================
 * AzamiOS — Device Filesystem (devfs)
 * File: fs/devfs.c
 *
 * Provides a virtual filesystem for character and block devices (/dev).
 * ============================================================================ */

#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../include/azami/defs.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../userland/libc/include/sys/dirent.h"

#define MAX_DEVICES 96

typedef struct {
    char name[32];
    file_operations_t *fops;
    void *private_data;
    u32 mode; /* S_IFCHR or S_IFBLK */
    inode_t *inode;
} devfs_node_t;

static spinlock_t g_devfs_lock = SPINLOCK_INIT;
static devfs_node_t g_devices[MAX_DEVICES];
static u32 g_device_count = 0;

/* Global function exposed to drivers */
int devfs_register_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (!name) return -1;
    
    spinlock_lock(&g_devfs_lock);
    if (g_device_count >= MAX_DEVICES) {
        spinlock_unlock(&g_devfs_lock);
        return -1;
    }
    
    /* Check if already registered */
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
            spinlock_unlock(&g_devfs_lock);
            return 0;
        }
    }

    devfs_node_t *node = &g_devices[g_device_count++];
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->fops = fops;
    node->private_data = private_data;
    node->mode = S_IFCHR;

    node->inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (node->inode) {
        node->inode->i_ino = 2 + (g_device_count - 1);
        node->inode->i_mode = S_IFCHR | 0666;
        node->inode->i_fop = fops;
        node->inode->i_private = private_data;
    }
    spinlock_unlock(&g_devfs_lock);
    
    return 0;
}

int devfs_register_block_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (!name) return -1;
    
    spinlock_lock(&g_devfs_lock);
    if (g_device_count >= MAX_DEVICES) {
        spinlock_unlock(&g_devfs_lock);
        return -1;
    }
    
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
            spinlock_unlock(&g_devfs_lock);
            return 0;
        }
    }

    devfs_node_t *node = &g_devices[g_device_count++];
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->fops = fops;
    node->private_data = private_data;
    node->mode = S_IFBLK;

    node->inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (node->inode) {
        node->inode->i_ino = 2 + (g_device_count - 1);
        node->inode->i_mode = S_IFBLK | 0666;
        node->inode->i_fop = fops;
        node->inode->i_private = private_data;
    }
    spinlock_unlock(&g_devfs_lock);
    
    return 0;
}

/*
 * A device may register a name containing '/' — dri/card0, input/event0 —
 * which places it in a subdirectory of /dev.  The VFS resolves a path one
 * component at a time, so those directories have to exist as inodes: a
 * directory inode carries its prefix in i_private, and the root's is NULL,
 * meaning the empty prefix.
 */
#define MAX_DEVFS_DIRS 8

static inode_operations_t devfs_inode_ops;
static file_operations_t  devfs_dir_fops;

static struct {
    char     name[32];
    inode_t *inode;
} g_devfs_dirs[MAX_DEVFS_DIRS];
static u32 g_devfs_dir_count = 0;

/*
 * The prefix of a devfs directory inode, or "" for the root.  The mountpoint's
 * root inode is not one of ours and may carry an unrelated i_private, so a
 * pointer only counts as a prefix when it is one this file handed out.
 */
static const char *devfs_dir_prefix(struct inode *dir)
{
    if (!dir || !dir->i_private) return "";
    for (u32 i = 0; i < g_devfs_dir_count; i++) {
        if ((const char *)dir->i_private == g_devfs_dirs[i].name) {
            return g_devfs_dirs[i].name;
        }
    }
    return "";
}

/* Join a directory prefix and one path component into a registered name. */
static void devfs_join(char *out, size_t cap, const char *prefix, const char *name)
{
    if (prefix[0]) snprintf(out, cap, "%s/%s", prefix, name);
    else           snprintf(out, cap, "%s", name);
}

/* Directory inodes are cached so repeated lookups return the same inode. */
static inode_t *devfs_get_dir_inode(super_block_t *sb, const char *path)
{
    for (u32 i = 0; i < g_devfs_dir_count; i++) {
        if (strcmp(g_devfs_dirs[i].name, path) == 0) {
            g_devfs_dirs[i].inode->i_sb = sb;
            return g_devfs_dirs[i].inode;
        }
    }
    if (g_devfs_dir_count >= MAX_DEVFS_DIRS) return NULL;

    inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!inode) return NULL;

    u32 slot = g_devfs_dir_count++;
    strncpy(g_devfs_dirs[slot].name, path, sizeof(g_devfs_dirs[slot].name) - 1);

    inode->i_ino     = 0x1000 + slot;
    inode->i_mode    = S_IFDIR | 0555;
    inode->i_sb      = sb;
    inode->i_op      = &devfs_inode_ops;
    inode->i_fop     = &devfs_dir_fops;
    inode->i_private = g_devfs_dirs[slot].name;   /* the prefix for lookups */

    g_devfs_dirs[slot].inode = inode;
    return inode;
}

dentry_t *devfs_lookup(struct inode *dir, struct dentry *dentry)
{
    char full[48];
    devfs_join(full, sizeof(full), devfs_dir_prefix(dir), dentry->d_name);

    spinlock_lock(&g_devfs_lock);
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(full, g_devices[i].name) == 0) {
            if (!g_devices[i].inode) {
                g_devices[i].inode = (inode_t *)kzalloc(sizeof(inode_t));
                if (g_devices[i].inode) {
                    g_devices[i].inode->i_ino = 2 + i;
                    g_devices[i].inode->i_mode = g_devices[i].mode | 0666;
                    g_devices[i].inode->i_fop = g_devices[i].fops;
                    g_devices[i].inode->i_private = g_devices[i].private_data;
                }
            }
            if (g_devices[i].inode) {
                g_devices[i].inode->i_sb = dir->i_sb;
            }
            dentry->d_inode = g_devices[i].inode;
            spinlock_unlock(&g_devfs_lock);
            return dentry;
        }
    }

    /* Not a device — but it may be the directory some device lives in. */
    size_t flen = strlen(full);
    for (u32 i = 0; i < g_device_count; i++) {
        const char *n = g_devices[i].name;
        if (strncmp(n, full, flen) == 0 && n[flen] == '/') {
            dentry->d_inode = devfs_get_dir_inode(dir->i_sb, full);
            spinlock_unlock(&g_devfs_lock);
            return dentry;
        }
    }

    spinlock_unlock(&g_devfs_lock);
    return dentry; /* Negative dentry */
}

static s64 devfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!dirent_buf || len == 0) return -(s64)EINVAL;

    const char *prefix = devfs_dir_prefix(filp ? filp->f_inode : NULL);
    size_t plen = strlen(prefix);

    /*
     * Build this directory's entries: the devices directly inside it, plus
     * one entry for each subdirectory a nested device implies, listed once.
     */
    struct { const char *name; size_t len; u64 ino; u8 type; } ents[MAX_DEVICES + MAX_DEVFS_DIRS];
    size_t nents = 0;

    spinlock_lock(&g_devfs_lock);
    for (u32 i = 0; i < g_device_count && nents < ARRAY_SIZE(ents); i++) {
        const char *n = g_devices[i].name;

        if (plen) {
            if (strncmp(n, prefix, plen) != 0 || n[plen] != '/') continue;
            n += plen + 1;
        }

        const char *slash = strchr(n, '/');
        if (!slash) {
            ents[nents].name = n;
            ents[nents].len  = strlen(n);
            ents[nents].ino  = 2 + i;
            ents[nents].type = (g_devices[i].mode == S_IFBLK) ? DT_BLK : DT_CHR;
            nents++;
        } else {
            size_t seglen = (size_t)(slash - n);
            bool seen = false;
            for (size_t k = 0; k < nents; k++) {
                if (ents[k].type == DT_DIR && ents[k].len == seglen &&
                    strncmp(ents[k].name, n, seglen) == 0) { seen = true; break; }
            }
            if (seen) continue;
            ents[nents].name = n;
            ents[nents].len  = seglen;
            ents[nents].ino  = 0x2000 + i;
            ents[nents].type = DT_DIR;
            nents++;
        }
    }
    spinlock_unlock(&g_devfs_lock);

    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    while (idx < (u64)(nents + 2)) {
        const char *name = NULL;
        u64 ino = 1;
        u8 dtype = DT_CHR;
        size_t nlen;

        if (idx == 0) {
            name = "."; ino = 1; dtype = DT_DIR; nlen = 1;
        } else if (idx == 1) {
            name = ".."; ino = 1; dtype = DT_DIR; nlen = 2;
        } else {
            size_t e = (size_t)(idx - 2);
            name  = ents[e].name;
            nlen  = ents[e].len;
            ino   = ents[e].ino;
            dtype = ents[e].type;
        }

        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + nlen + 1, 8);
        if (written + reclen > len) {
            if (written == 0) return -(s64)EINVAL;
            break;
        }
        
        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
        d->d_ino = ino;
        d->d_off = (long long)(idx + 1);
        d->d_reclen = (unsigned short)reclen;
        d->d_type = dtype;
        memcpy(d->d_name, name, nlen);
        d->d_name[nlen] = '\0';
        
        written += reclen;
        idx++;
        *offset = idx;
    }
    
    return (s64)written;
}

static s64 devfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode)
{
    (void)mode;
    char full[48];
    devfs_join(full, sizeof(full), devfs_dir_prefix(dir), dentry->d_name);
    inode_t *ino = devfs_get_dir_inode(dir->i_sb, full);
    if (!ino) return -(s64)ENOMEM;
    dentry->d_inode = ino;
    return 0;
}

static file_operations_t devfs_dir_fops = {
    .readdir = devfs_dir_readdir,
};

static inode_operations_t devfs_inode_ops = {
    .lookup = devfs_lookup,
    .mkdir  = devfs_mkdir,
};

static s64 devfs_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)dev_name;
    (void)data;
    
    dentry_t *mountpoint = NULL;
    s64 err = vfs_path_lookup(dir_name, &mountpoint);
    if (err < 0 || !mountpoint || !mountpoint->d_inode) {
        if (mountpoint && !mountpoint->d_inode) kfree(mountpoint);
        return -(s64)ENOENT;
    }

    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!sb || !root_inode) {
        if (sb) kfree(sb);
        if (root_inode) kfree(root_inode);
        return -(s64)ENOMEM;
    }
    
    sb->s_magic = 0xDE7F5;
    sb->s_type = fs_type;
    
    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &devfs_inode_ops;
    root_inode->i_fop = &devfs_dir_fops;
    
    mountpoint->d_inode = root_inode;
    mountpoint->d_sb = sb;
    sb->s_root = mountpoint;
    
    return 0; /* Success */
}

file_system_type_t g_devfs_type = {
    .name = "devfs",
    .mount = devfs_mount,
    .next = NULL,
};

void devfs_init(void)
{
    vfs_register_fs(&g_devfs_type);
}
