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
#include "../userland/libc/include/sys/dirent.h"

#define MAX_DEVICES 64

typedef struct {
    char name[32];
    file_operations_t *fops;
    void *private_data;
    u32 mode; /* S_IFCHR or S_IFBLK */
    inode_t *inode;
} devfs_node_t;

static devfs_node_t g_devices[MAX_DEVICES];
static u32 g_device_count = 0;

/* Global function exposed to drivers */
int devfs_register_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (g_device_count >= MAX_DEVICES || !name) return -1;
    
    /* Check if already registered */
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
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
    
    return 0;
}

int devfs_register_block_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (g_device_count >= MAX_DEVICES || !name) return -1;
    
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
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
    
    return 0;
}

dentry_t *devfs_lookup(struct inode *dir, struct dentry *dentry)
{
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(dentry->d_name, g_devices[i].name) == 0) {
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
            return dentry;
        }
    }
    return dentry; /* Negative dentry */
}

static s64 devfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!dirent_buf || len == 0) return -(s64)EINVAL;
    
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;
    
    while (idx < (u64)(g_device_count + 2)) {
        const char *name = NULL;
        u64 ino = 1;
        u8 dtype = DT_CHR;
        
        if (idx == 0) {
            name = ".";
            ino = 1;
            dtype = DT_DIR;
        } else if (idx == 1) {
            name = "..";
            ino = 1;
            dtype = DT_DIR;
        } else {
            u32 dev_idx = (u32)(idx - 2);
            name = g_devices[dev_idx].name;
            ino = 2 + dev_idx;
            dtype = (g_devices[dev_idx].mode == S_IFBLK) ? DT_BLK : DT_CHR;
        }
        
        size_t nlen = strlen(name);
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

static file_operations_t devfs_dir_fops = {
    .readdir = devfs_dir_readdir,
};

static inode_operations_t devfs_inode_ops = {
    .lookup = devfs_lookup,
};

static s64 devfs_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)dev_name;
    (void)dir_name;
    (void)data;
    
    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    dentry_t *root_dentry = dcache_alloc(NULL, "/");
    
    sb->s_magic = 0xDE7F5;
    sb->s_type = fs_type;
    
    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &devfs_inode_ops;
    root_inode->i_fop = &devfs_dir_fops;
    
    root_dentry->d_inode = root_inode;
    root_dentry->d_sb = sb;
    sb->s_root = root_dentry;
    
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
