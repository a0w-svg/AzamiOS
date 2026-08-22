/* ============================================================================
 * AzamiOS — Virtual File System (VFS) Header
 * File: fs/vfs.h
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"

#define VFS_NAME_MAX  255

/* File open flags (matching standard POSIX / Linux mostly) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_NOCTTY    0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

/* File descriptor flags */
#define FD_CLOEXEC  1

/* lseek origins */
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

/* stat.st_mode flags */
#define S_IFMT      0170000
#define S_IFSOCK    0140000
#define S_IFLNK     0120000
#define S_IFREG     0100000
#define S_IFBLK     0060000
#define S_IFDIR     0040000
#define S_IFCHR     0020000
#define S_IFIFO     0010000

#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

/* Linux x86_64 compatible struct stat */
struct stat {
    u64 st_dev;
    u64 st_ino;
    u64 st_nlink;
    u32 st_mode;
    u32 st_uid;
    u32 st_gid;
    u32 __pad0;
    u64 st_rdev;
    s64 st_size;
    s64 st_blksize;
    s64 st_blocks;
    u64 st_atime;
    u64 st_atime_nsec;
    u64 st_mtime;
    u64 st_mtime_nsec;
    u64 st_ctime;
    u64 st_ctime_nsec;
    s64 _unused[3];
};

struct statfs {
    u64 f_type;
    u64 f_bsize;
    u64 f_blocks;
    u64 f_bfree;
    u64 f_bavail;
    u64 f_files;
    u64 f_ffree;
    u64 f_fsid[2];
    u64 f_namelen;
    u64 f_frsize;
    u64 f_flags;
    u64 f_spare[4];
};

struct inode;
struct dentry;
struct file;
struct super_block;
struct inode_operations;
struct file_operations;
struct super_operations;
struct file_system_type;

/* --------------------------------------------------------------------------
 * Superblock Operations
 * -------------------------------------------------------------------------- */
typedef struct super_operations {
    struct inode *(*alloc_inode)(struct super_block *sb);
    void (*destroy_inode)(struct inode *inode);
    void (*write_inode)(struct inode *inode);
    void (*put_super)(struct super_block *sb);
    s64 (*statfs)(struct super_block *sb, struct statfs *buf);
} super_operations_t;

/* --------------------------------------------------------------------------
 * Superblock
 * -------------------------------------------------------------------------- */
typedef struct super_block {
    u32 s_magic;
    u32 s_blocksize;
    struct file_system_type *s_type;
    super_operations_t *s_op;
    struct dentry *s_root;
    void *s_fs_info; /* Filesystem specific private data */
    struct super_block *next;
} super_block_t;

/* --------------------------------------------------------------------------
 * Inode Operations
 * -------------------------------------------------------------------------- */
typedef struct inode_operations {
    struct dentry *(*lookup)(struct inode *dir, struct dentry *dentry);
    s64 (*create)(struct inode *dir, struct dentry *dentry, u32 mode);
    s64 (*mkdir)(struct inode *dir, struct dentry *dentry, u32 mode);
    s64 (*unlink)(struct inode *dir, struct dentry *dentry);
    s64 (*rmdir)(struct inode *dir, struct dentry *dentry);
    s64 (*rename)(struct inode *old_dir, struct dentry *old_dentry, struct inode *new_dir, struct dentry *new_dentry);
    s64 (*symlink)(struct inode *dir, struct dentry *dentry, const char *symname);
    s64 (*readlink)(struct dentry *dentry, char *buf, size_t buflen);
} inode_operations_t;

/* --------------------------------------------------------------------------
 * Inode
 * -------------------------------------------------------------------------- */
typedef struct inode {
    u64 i_ino;
    u32 i_mode;
    u32 i_uid;
    u32 i_gid;
    u64 i_size;
    u64 i_blocks;
    u64 i_atime;
    u64 i_mtime;
    u64 i_ctime;
    
    super_block_t *i_sb;
    inode_operations_t *i_op;
    struct file_operations *i_fop;
    
    void *i_private; /* Filesystem specific private data */
} inode_t;

/* --------------------------------------------------------------------------
 * Dentry (Directory Entry Cache)
 * -------------------------------------------------------------------------- */
typedef struct dentry {
    struct dentry *d_parent;
    char d_name[VFS_NAME_MAX];
    inode_t *d_inode;     /* NULL if negative dentry */
    struct dentry *d_subdirs; /* Child list head */
    struct dentry *d_sibling; /* Next child of our parent */
    super_block_t *d_sb;
} dentry_t;

/* --------------------------------------------------------------------------
 * File Operations
 * -------------------------------------------------------------------------- */
typedef struct file_operations {
    s64 (*read)(struct file *filp, void *buf, size_t len, u64 *offset);
    s64 (*write)(struct file *filp, const void *buf, size_t len, u64 *offset);
    s64 (*readdir)(struct file *filp, void *dirent_buf, size_t len, u64 *offset); /* For getdents64 */
    s64 (*ioctl)(struct file *filp, u32 cmd, u64 arg);
    s64 (*mmap)(struct file *filp, virt_addr_t vaddr, size_t len, u32 prot, u32 flags, u64 offset);
    s64 (*open)(struct inode *inode, struct file *filp);
    s64 (*release)(struct inode *inode, struct file *filp);
    int (*poll)(struct file *filp);
} file_operations_t;

/* --------------------------------------------------------------------------
 * File (Open File Description)
 * -------------------------------------------------------------------------- */
typedef struct file {
    dentry_t *f_dentry;
    inode_t *f_inode;
    file_operations_t *f_op;
    u64 f_pos;
    u32 f_flags;    /* Open-file-description flags (O_RDONLY/O_RDWR, O_NONBLOCK, ...) */
    u32 f_fd_flags; /* Per-FD flags: FD_CLOEXEC (1) — not shared across dup/fork */
    u32 f_mode;
    u32 f_count; /* Reference count */
    void *private_data; /* Used by drivers/filesystems for state */
} file_t;

/* --------------------------------------------------------------------------
 * File System Type Registration
 * -------------------------------------------------------------------------- */
typedef struct file_system_type {
    const char *name;
    s64 (*mount)(struct file_system_type *fs_type, const char *dev_name, const char *dir_name, void *data);
    struct file_system_type *next;
} file_system_type_t;

/* --------------------------------------------------------------------------
 * VFS Core API
 * -------------------------------------------------------------------------- */

/** vfs_init() — Initialize virtual filesystem and dcache. */
void vfs_init(void);

/** vfs_register_fs() — Register a new filesystem type. */
s64 vfs_register_fs(file_system_type_t *fs);

/** vfs_mount() — Mount a filesystem. */
s64 vfs_mount(const char *source, const char *target, const char *fstype, void *data);

/* Dentry Cache functions */
dentry_t *dcache_alloc(dentry_t *parent, const char *name);
void dcache_add(dentry_t *dentry);
void dcache_remove(dentry_t *dentry);
dentry_t *dcache_lookup(dentry_t *parent, const char *name);

/* Path lookup & resolution */
s64 vfs_resolve_path(const char *cwd, const char *path, char *out_buf, size_t out_len);
s64 vfs_path_lookup(const char *path, dentry_t **out_dentry);

/* System Call Implementations (Internal to fs/vfs.c but exposed to syscall.c) */
file_t *vfs_open(const char *path, u32 flags, u32 mode);
file_t *vfs_open_err(const char *path, u32 flags, u32 mode, s64 *out_errno);
s64 vfs_close(file_t *file);
s64 vfs_read(file_t *file, void *buf, size_t size);
s64 vfs_write(file_t *file, const void *buf, size_t size);
s64 vfs_lseek(file_t *file, s64 offset, int whence);
s64 vfs_stat(const char *path, struct stat *statbuf);
s64 vfs_lstat(const char *path, struct stat *statbuf);
s64 vfs_fstat(file_t *file, struct stat *statbuf);
s64 vfs_ioctl(file_t *file, u32 cmd, u64 arg);
s64 vfs_mkdir(const char *path, u32 mode);
s64 vfs_rmdir(const char *path);
s64 vfs_unlink(const char *path);
s64 vfs_rename(const char *oldpath, const char *newpath);
s64 vfs_truncate(file_t *file, u64 length);
s64 vfs_symlink(const char *target, const char *linkpath);
s64 vfs_readlink(const char *path, char *buf, size_t bufsiz);
s64 vfs_chmod(const char *path, u32 mode);
s64 vfs_fchmod(file_t *file, u32 mode);
s64 vfs_chown(const char *path, u32 uid, u32 gid);
s64 vfs_fchown(file_t *file, u32 uid, u32 gid);
s64 vfs_statfs(const char *path, struct statfs *buf);
s64 vfs_fstatfs(file_t *file, struct statfs *buf);

/* DevFS API */
int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);
int devfs_register_block_device(const char *name, file_operations_t *fops, void *private_data);
void devfs_init(void);
