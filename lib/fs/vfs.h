/**
 * lib/fs/vfs.h  –  AzamiOS Virtual Filesystem Interface
 *
 * Kernel-independent: only depends on <stdint.h>.
 * Canonical copy — kernel/filesystem/include/vfs.h delegates here.
 */
#ifndef LIB_VFS_H
#define LIB_VFS_H

#include <stdint.h>

/* ── Node type flags ──────────────────────────────────────────────── */
#define FS_FILE         0x10
#define FS_DIRECTORY    0x02
#define FS_CHARDEVICE   0x03
#define FS_BLOCKDEVICE  0x04
#define FS_PIPE         0x05
#define FS_SYMLINK      0x06
#define FS_MOUNTPOINT   0x08

/* ── Block device abstraction ─────────────────────────────────────── */
typedef struct block_device {
    char name[32];
    uint32_t block_size;
    uint32_t (*read)(struct block_device *dev,
                     uint32_t lba, uint32_t count, void *buffer);
    uint32_t (*write)(struct block_device *dev,
                      uint32_t lba, uint32_t count, void *buffer);
    void *impl_data;
} block_device_t;

struct fs_node;

/* ── Function-pointer typedefs ────────────────────────────────────── */
typedef uint32_t (*read_type_t)(block_device_t *dev, struct fs_node*,
                                uint32_t offset, uint32_t size, uint8_t *buf);
typedef uint32_t (*write_type_t)(block_device_t *dev, struct fs_node*,
                                 uint32_t offset, uint32_t size, uint8_t *buf);
typedef int     (*open_type_t)(struct fs_node*);
typedef int     (*close_type_t)(struct fs_node*);
typedef struct directory_entry *(*readdir_type_t)(struct fs_node*, uint32_t);
typedef struct fs_node         *(*finddir_type_t)(struct fs_node*, char*);

/* ── Filesystem node ──────────────────────────────────────────────── */
typedef struct fs_node {
    char     name[128];
    uint32_t mask;
    uint32_t uid;
    uint32_t gid;
    uint32_t flags;
    uint32_t inode;
    uint32_t length;
    uint32_t impl;

    read_type_t    read;
    write_type_t   write;
    open_type_t    open;
    close_type_t   close;
    readdir_type_t readdir;
    finddir_type_t finddir;

    struct fs_node *ptr;
} fs_node_t;

/* Global root of the mounted filesystem */
extern fs_node_t *fs_root;

/* Directory entry returned by readdir */
typedef struct directory_entry {
    char     name[128];
    uint32_t inode;
} directory_entry_t;

/* ── Public VFS API ───────────────────────────────────────────────── */
uint32_t          read_fs(block_device_t *dev, fs_node_t *node,
                           uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t          write_fs(block_device_t *dev, fs_node_t *node,
                            uint32_t offset, uint32_t size, uint8_t *buffer);
int               open_fs(fs_node_t *node);
int               close_fs(fs_node_t *node);
directory_entry_t *readdir_fs(fs_node_t *node, uint32_t index);
fs_node_t         *finddir_fs(fs_node_t *node, char *name);
void               vfs_register_device(block_device_t *dev);

#endif /* LIB_VFS_H */
