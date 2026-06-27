/**
 * lib/fs/vfs.c  –  AzamiOS Virtual Filesystem dispatch layer
 *
 * Kernel-independent: dispatches all FS operations through function pointers
 * stored in fs_node_t.  The only output is via an optional log callback;
 * no direct kprintf or VGA access.
 *
 * Compiles with: i686-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "vfs.h"

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

void vfs_register_device(block_device_t *dev) {
    if (device_count < MAX_DEVICES) {
        device_list[device_count++] = dev;
        VFS_LOG("Device [%s] has been registered successfully\n", dev->name);
    }
}
