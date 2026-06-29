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

/* ── Unix-Style VFS Core Structures ───────────────────────────────── */
struct vfs_file;
struct vfs_inode;
struct vfs_dentry;
struct vfs_super_block;
struct vfs_fs_type;

typedef struct file_operations {
    uint32_t (*read)(struct vfs_file *file, uint32_t offset, uint32_t size, uint8_t *buffer);
    uint32_t (*write)(struct vfs_file *file, uint32_t offset, uint32_t size, uint8_t *buffer);
    int      (*open)(struct vfs_inode *inode, struct vfs_file *file);
    int      (*close)(struct vfs_file *file);
} file_operations_t;

typedef struct inode_operations {
    struct vfs_dentry *(*lookup)(struct vfs_inode *dir, const char *name);
    int                (*create)(struct vfs_inode *dir, const char *name, uint32_t mode);
    int                (*mkdir)(struct vfs_inode *dir, const char *name, uint32_t mode);
} inode_operations_t;

typedef struct super_operations {
    struct vfs_inode *(*alloc_inode)(struct vfs_super_block *sb);
    void              (*destroy_inode)(struct vfs_inode *inode);
    int               (*write_super)(struct vfs_super_block *sb);
} super_operations_t;

typedef struct vfs_inode {
    uint32_t ino;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t size;
    uint32_t nlink;
    struct vfs_super_block *sb;
    inode_operations_t     *i_op;
    file_operations_t      *f_op;
    void                   *i_private;
    struct fs_node         *legacy_node; /* Legacy bridge pointer */
} vfs_inode_t;

typedef struct vfs_dentry {
    char name[128];
    vfs_inode_t *d_inode;
    struct vfs_dentry *d_parent;
    struct vfs_super_block *d_sb;
} vfs_dentry_t;

typedef struct vfs_super_block {
    block_device_t     *s_dev;
    uint32_t            s_blocksize;
    super_operations_t *s_op;
    vfs_dentry_t       *s_root;
    struct vfs_fs_type *s_type;
    void               *s_fs_info;
} vfs_super_block_t;

typedef struct vfs_fs_type {
    char name[32];
    int (*mount)(struct vfs_fs_type *type, block_device_t *dev, vfs_super_block_t *sb);
    struct vfs_fs_type *next;
} vfs_fs_type_t;

typedef struct vfs_file {
    vfs_inode_t       *f_inode;
    vfs_dentry_t      *f_dentry;
    file_operations_t *f_op;
    uint32_t           f_pos;
    uint32_t           f_flags;
    void              *private_data;
} vfs_file_t;

/* Public Unix-style VFS lifecycle API */
vfs_file_t *vfs_open_file(const char *path, uint32_t flags);
uint32_t    vfs_file_read(vfs_file_t *file, uint32_t size, uint8_t *buffer);
uint32_t    vfs_file_write(vfs_file_t *file, uint32_t size, uint8_t *buffer);
int         vfs_file_close(vfs_file_t *file);

#endif /* LIB_VFS_H */

