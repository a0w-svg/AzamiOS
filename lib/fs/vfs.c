/**
 * lib/fs/vfs.c  –  AzamiOS Virtual Filesystem dispatch layer
 *
 * Kernel-independent: dispatches all FS operations through function pointers
 * stored in fs_node_t.  The only output is via an optional log callback;
 * no direct kprintf or VGA access.
 *
 * Compiles with: x86_64-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "vfs.h"
#include "../string/string.h"

/* Optional log callback — set by the kernel after boot.  NULL = silent. */
static void (*vfs_log)(const char *fmt, ...) = (void*)0;
void vfs_set_log(void (*fn)(const char*, ...)) { vfs_log = fn; }

#define VFS_LOG(...) do { if (vfs_log) vfs_log(__VA_ARGS__); } while(0)

/* Global root pointer */
fs_node_t *fs_root = (void*)0;

#define MAX_DEVICES 16
block_device_t *device_list[MAX_DEVICES];
static int device_count = 0;

/* ── VFS Operations ──────────────────────────────────────────────── */

uint32_t read_fs(block_device_t *dev, fs_node_t *node,
                 uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (dev == (void*)0) {
        VFS_LOG("VFS Error: Read attempt from NULL Device\n");
        return 0;
    }
    if (node->read == (void*)0) {
        VFS_LOG("VFS Error: Driver %s doesn't have read function!\n", dev->name);
        return 0;
    }
    return node->read(dev, node, offset, size, buffer);
}

uint32_t write_fs(block_device_t *dev, fs_node_t *node,
                  uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (dev == (void*)0) {
        VFS_LOG("VFS Error: Write attempt from NULL Device\n");
        return 0;
    }
    if (node->write == (void*)0) {
        VFS_LOG("VFS Error: Driver %s doesn't have write function!\n", dev->name);
        return 0;
    }
    return node->write(dev, node, offset, size, buffer);
}

int open_fs(fs_node_t *node) {
    if (node->open != (void*)0)
        node->open(node);
    return 0;
}

int close_fs(fs_node_t *node) {
    if (node->close != (void*)0)
        node->close(node);
    return 0;
}

directory_entry_t *readdir_fs(fs_node_t *node, uint32_t index) {
    if (((node->flags & 0x07) == FS_DIRECTORY) && (node->readdir != (void*)0))
        return node->readdir(node, index);
    return (void*)0;
}

fs_node_t *finddir_fs(fs_node_t *node, char *name) {
    if (((node->flags & 0x07) == FS_DIRECTORY) && node->finddir != (void*)0)
        return node->finddir(node, name);
    return (void*)0;
}

extern fs_node_t *initrd_create_file(char *name);

static uint32_t block_dev_read_wrapper(block_device_t *dev, struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)dev;
    block_device_t *bdev = (block_device_t*)(uintptr_t)node->impl;
    if (!bdev || !bdev->read || bdev->block_size == 0) return 0;
    uint32_t lba = offset / bdev->block_size;
    uint32_t count = (size + bdev->block_size - 1) / bdev->block_size;
    if (count == 0) count = 1;
    return bdev->read(bdev, lba, count, buf);
}

static uint32_t block_dev_write_wrapper(block_device_t *dev, struct fs_node *node, uint32_t offset, uint32_t size, uint8_t *buf) {
    (void)dev;
    block_device_t *bdev = (block_device_t*)(uintptr_t)node->impl;
    if (!bdev || !bdev->write || bdev->block_size == 0) return 0;
    uint32_t lba = offset / bdev->block_size;
    uint32_t count = (size + bdev->block_size - 1) / bdev->block_size;
    if (count == 0) count = 1;
    return bdev->write(bdev, lba, count, buf);
}

void vfs_register_device(block_device_t *dev) {
    if (device_count < MAX_DEVICES) {
        device_list[device_count++] = dev;
        VFS_LOG("Device [%s] has been registered successfully\n", dev->name);
        char devpath[64] = "dev/";
        int len = 4;
        for (int i = 0; dev->name[i] && len < 63; i++) {
            devpath[len++] = dev->name[i];
        }
        devpath[len] = '\0';
        fs_node_t *node = initrd_create_file(devpath);
        if (node) {
            node->flags = FS_BLOCKDEVICE;
            node->impl = (uint32_t)(uintptr_t)dev;
            node->read = block_dev_read_wrapper;
            node->write = block_dev_write_wrapper;
        }
    }
}

block_device_t *vfs_get_device(const char *name) {
    if (!name) return (void*)0;
    const char *n = name;
    if (n[0] == '/' && n[1] == 'd' && n[2] == 'e' && n[3] == 'v' && n[4] == '/') n += 5;
    else if (n[0] == 'd' && n[1] == 'e' && n[2] == 'v' && n[3] == '/') n += 4;
    for (int i = 0; i < device_count; i++) {
        if (strcmp(device_list[i]->name, n) == 0) return device_list[i];
    }
    return (void*)0;
}

#define MAX_MOUNTS 8
typedef struct {
    char path[64];
    fs_node_t *root_node;
} vfs_mount_entry_t;

static vfs_mount_entry_t g_mounts[MAX_MOUNTS];
static int g_mount_count = 0;

extern fs_node_t *fat32_mount(block_device_t *dev) __attribute__((weak));

int vfs_mount(const char *dev_name, const char *mount_point, const char *fs_type) {
    if (!dev_name || !mount_point) return -1;
    block_device_t *dev = vfs_get_device(dev_name);
    if (!dev) {
        VFS_LOG("vfs_mount: device not found [%s]\n", dev_name);
        return -1;
    }
    if (g_mount_count >= MAX_MOUNTS) return -1;

    fs_node_t *mroot = (void*)0;
    if (fs_type && (strcmp(fs_type, "fat32") == 0 || strcmp(fs_type, "vfat") == 0)) {
        if (fat32_mount != (void*)0) {
            mroot = fat32_mount(dev);
        }
    }
    if (!mroot) {
        mroot = initrd_create_file((char*)mount_point);
        if (mroot) mroot->flags = FS_DIRECTORY | FS_MOUNTPOINT;
    }

    if (mroot) {
        strncpy(g_mounts[g_mount_count].path, mount_point, 63);
        g_mounts[g_mount_count].root_node = mroot;
        g_mount_count++;
        VFS_LOG("vfs_mount: mounted [%s] on [%s] (%s)\n", dev_name, mount_point, fs_type ? fs_type : "auto");
        return 0;
    }
    return -1;
}

int vfs_unmount(const char *mount_point) {
    if (!mount_point) return -1;
    for (int i = 0; i < g_mount_count; i++) {
        if (strcmp(g_mounts[i].path, mount_point) == 0) {
            for (int j = i; j < g_mount_count - 1; j++) {
                g_mounts[j] = g_mounts[j + 1];
            }
            g_mount_count--;
            VFS_LOG("vfs_unmount: unmounted [%s]\n", mount_point);
            return 0;
        }
    }
    return -1;
}

/* ── Unix-Style VFS Implementation & Legacy Bridge ────────────────── */

#define VFS_MAX_FILES    32
#define VFS_MAX_INODES   64
#define VFS_MAX_DENTRIES 64

static vfs_file_t   g_vfs_files[VFS_MAX_FILES];
static vfs_inode_t  g_vfs_inodes[VFS_MAX_INODES];
static vfs_dentry_t g_vfs_dentries[VFS_MAX_DENTRIES];

extern fs_node_t *initrd_create_file(char *name);

static uint32_t legacy_bridge_read(vfs_file_t *file, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!file || !file->f_inode || !file->f_inode->legacy_node) return 0;
    fs_node_t *node = file->f_inode->legacy_node;
    if (!node->read) {
        if (((node->flags & 0x07) == FS_DIRECTORY) && node->readdir) {
            uint32_t idx = offset / 128;
            directory_entry_t *entry = node->readdir(node, idx);
            if (!entry) return 0;
            uint32_t copy_len = size > 128 ? 128 : size;
            for (uint32_t i = 0; i < copy_len; i++) buffer[i] = 0;
            for (int i = 0; entry->name[i] && i < 127 && (uint32_t)i < copy_len; i++) {
                buffer[i] = entry->name[i];
            }
            return copy_len;
        }
        return 0;
    }
    return node->read((void*)0, node, offset, size, buffer);
}

static uint32_t legacy_bridge_write(vfs_file_t *file, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!file || !file->f_inode || !file->f_inode->legacy_node) return 0;
    fs_node_t *node = file->f_inode->legacy_node;
    if (!node->write) return 0;
    return node->write((void*)0, node, offset, size, buffer);
}

static file_operations_t legacy_bridge_fops = {
    legacy_bridge_read,
    legacy_bridge_write,
    (void*)0,
    (void*)0
};

vfs_file_t *vfs_open_file(const char *path, uint32_t flags) {
    if (!path || !fs_root || !fs_root->finddir) return (void*)0;

    fs_node_t *node = (void*)0;
    for (int m = 0; m < g_mount_count; m++) {
        size_t mlen = strlen(g_mounts[m].path);
        if (strncmp(path, g_mounts[m].path, mlen) == 0) {
            const char *subpath = path + mlen;
            if (subpath[0] == '/') subpath++;
            if (subpath[0] == '\0') {
                node = g_mounts[m].root_node;
            } else if (g_mounts[m].root_node && g_mounts[m].root_node->finddir) {
                node = g_mounts[m].root_node->finddir(g_mounts[m].root_node, (char*)subpath);
            }
            break;
        }
    }
    if (!node) {
        node = fs_root->finddir(fs_root, (char*)path);
    }
    if (!node && (flags & 0x0100)) {
        node = initrd_create_file((char*)path);
    }
    if (!node) return (void*)0;

    if (flags & 0x0200) { /* O_TRUNC */
        node->length = 0;
    }

    vfs_inode_t *inode = (void*)0;
    for (int i = 0; i < VFS_MAX_INODES; i++) {
        if (g_vfs_inodes[i].legacy_node == node) {
            inode = &g_vfs_inodes[i];
            inode->size = node->length;
            break;
        }
    }
    if (!inode) {
        for (int i = 0; i < VFS_MAX_INODES; i++) {
            if (g_vfs_inodes[i].legacy_node == (void*)0 && g_vfs_inodes[i].ino == 0) {
                inode = &g_vfs_inodes[i];
                inode->ino = i + 1;
                inode->size = node->length;
                inode->legacy_node = node;
                inode->f_op = &legacy_bridge_fops;
                break;
            }
        }
    }
    if (!inode) return (void*)0;

    vfs_dentry_t *dentry = (void*)0;
    for (int i = 0; i < VFS_MAX_DENTRIES; i++) {
        if (g_vfs_dentries[i].d_inode == inode) {
            dentry = &g_vfs_dentries[i];
            break;
        }
    }
    if (!dentry) {
        for (int i = 0; i < VFS_MAX_DENTRIES; i++) {
            if (g_vfs_dentries[i].d_inode == (void*)0) {
                dentry = &g_vfs_dentries[i];
                dentry->d_inode = inode;
                int j = 0;
                while (path[j] && j < 127) { dentry->name[j] = path[j]; j++; }
                dentry->name[j] = '\0';
                break;
            }
        }
    }

    for (int i = 0; i < VFS_MAX_FILES; i++) {
        if (g_vfs_files[i].f_inode == (void*)0) {
            vfs_file_t *file = &g_vfs_files[i];
            file->f_inode = inode;
            file->f_dentry = dentry;
            file->f_op = inode->f_op;
            file->f_pos = 0;
            file->f_flags = flags;
            if (node->open) node->open(node);
            return file;
        }
    }
    return (void*)0;
}

uint32_t vfs_file_read(vfs_file_t *file, uint32_t size, uint8_t *buffer) {
    if (!file || !file->f_op || !file->f_op->read || size == 0 || !buffer) return 0;
    uint32_t n = file->f_op->read(file, file->f_pos, size, buffer);
    file->f_pos += n;
    return n;
}

uint32_t vfs_file_write(vfs_file_t *file, uint32_t size, uint8_t *buffer) {
    if (!file || !file->f_op || !file->f_op->write || size == 0 || !buffer) return 0;
    uint32_t n = file->f_op->write(file, file->f_pos, size, buffer);
    file->f_pos += n;
    if (file->f_inode && file->f_inode->legacy_node) {
        file->f_inode->size = file->f_inode->legacy_node->length;
    }
    return n;
}

int vfs_file_close(vfs_file_t *file) {
    if (!file) return -1;
    if (file->f_op && file->f_op->close) {
        file->f_op->close(file);
    } else if (file->f_inode && file->f_inode->legacy_node && file->f_inode->legacy_node->close) {
        file->f_inode->legacy_node->close(file->f_inode->legacy_node);
    }
    file->f_inode = (void*)0;
    file->f_dentry = (void*)0;
    file->f_op = (void*)0;
    file->f_pos = 0;
    file->f_flags = 0;
    return 0;
}

