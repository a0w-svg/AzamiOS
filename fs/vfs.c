/* ============================================================================
 * AzamiOS — Virtual File System (VFS) Implementation
 * File: fs/vfs.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../drivers/char/console.h"
#include "../include/azami/defs.h"
#include "../kernel/syscall/syscall.h"


static spinlock_t g_vfs_lock = SPINLOCK_INIT;
static dentry_t *g_vfs_root = NULL;
static file_system_type_t *g_fs_types = NULL;

/* --------------------------------------------------------------------------
 * Dentry Cache
 * -------------------------------------------------------------------------- */

dentry_t *dcache_alloc(dentry_t *parent, const char *name)
{
    dentry_t *d = (dentry_t *)kzalloc(sizeof(dentry_t));
    if (!d) return NULL;
    
    int i = 0;
    while (name[i] && i < VFS_NAME_MAX - 1) {
        d->d_name[i] = name[i];
        i++;
    }
    d->d_name[i] = '\0';
    
    d->d_parent = parent;
    if (parent) {
        d->d_sb = parent->d_sb;
    }
    return d;
}

#define DCACHE_HASH_SIZE 256
static dentry_t *g_dcache_hash[DCACHE_HASH_SIZE];

static inline u32 dcache_hash_fn(dentry_t *parent, const char *name)
{
    u32 hash = (u32)(uintptr_t)parent ^ 0x9e3779b9;
    for (size_t i = 0; name[i] && i < VFS_NAME_MAX; i++) {
        hash = (hash * 31) + (u8)name[i];
    }
    return hash & (DCACHE_HASH_SIZE - 1);
}

void dcache_add(dentry_t *dentry)
{
    if (!dentry || !dentry->d_parent || !dentry->d_inode) return;
    
    spinlock_lock(&g_vfs_lock);
    dentry->d_sibling = dentry->d_parent->d_subdirs;
    dentry->d_parent->d_subdirs = dentry;

    u32 bucket = dcache_hash_fn(dentry->d_parent, dentry->d_name);
    dentry->d_hash_next = g_dcache_hash[bucket];
    g_dcache_hash[bucket] = dentry;
    spinlock_unlock(&g_vfs_lock);
}

void dcache_remove(dentry_t *dentry)
{
    if (!dentry || !dentry->d_parent) return;
    spinlock_lock(&g_vfs_lock);
    dentry_t **curr = &dentry->d_parent->d_subdirs;
    while (*curr) {
        if (*curr == dentry) {
            *curr = dentry->d_sibling;
            dentry->d_sibling = NULL;
            break;
        }
        curr = &(*curr)->d_sibling;
    }

    u32 bucket = dcache_hash_fn(dentry->d_parent, dentry->d_name);
    dentry_t **hcurr = &g_dcache_hash[bucket];
    while (*hcurr) {
        if (*hcurr == dentry) {
            *hcurr = dentry->d_hash_next;
            dentry->d_hash_next = NULL;
            break;
        }
        hcurr = &(*hcurr)->d_hash_next;
    }
    spinlock_unlock(&g_vfs_lock);
}

dentry_t *dcache_lookup(dentry_t *parent, const char *name)
{
    if (!parent) return NULL;
    
    spinlock_lock(&g_vfs_lock);
    u32 bucket = dcache_hash_fn(parent, name);
    dentry_t *entry = g_dcache_hash[bucket];
    while (entry) {
        if (entry->d_parent == parent && strncmp(name, entry->d_name, VFS_NAME_MAX) == 0) {
            spinlock_unlock(&g_vfs_lock);
            return entry;
        }
        entry = entry->d_hash_next;
    }

    /* Fallback: check direct child list */
    dentry_t *child = parent->d_subdirs;
    while (child) {
        if (strncmp(name, child->d_name, VFS_NAME_MAX) == 0) {
            child->d_hash_next = g_dcache_hash[bucket];
            g_dcache_hash[bucket] = child;
            spinlock_unlock(&g_vfs_lock);
            return child;
        }
        child = child->d_sibling;
    }
    spinlock_unlock(&g_vfs_lock);
    return NULL;
}

/* --------------------------------------------------------------------------
 * Filesystem Registration & Mounting
 * -------------------------------------------------------------------------- */

s64 vfs_register_fs(file_system_type_t *fs)
{
    spinlock_lock(&g_vfs_lock);
    fs->next = g_fs_types;
    g_fs_types = fs;
    spinlock_unlock(&g_vfs_lock);
    pr_debug("[VFS] Registered filesystem type: %s\n", fs->name);
    return 0;
}

file_system_type_t *vfs_find_fs(const char *name)
{
    spinlock_lock(&g_vfs_lock);
    file_system_type_t *curr = g_fs_types;
    while (curr) {
        bool match = true;
        int i;
        for (i = 0; i < 32; i++) {
            if (name[i] != curr->name[i]) { match = false; break; }
            if (name[i] == '\0') break;
        }
        if (match && i < 32) {
            spinlock_unlock(&g_vfs_lock);
            return curr;
        }
        curr = curr->next;
    }
    spinlock_unlock(&g_vfs_lock);
    return NULL;
}

s64 vfs_mount(const char *source, const char *target, const char *fstype, void *data)
{
    file_system_type_t *fs = vfs_find_fs(fstype);
    if (!fs) return -(s64)ENODEV;
    if (!fs->mount) return -(s64)EINVAL;
    
    return fs->mount(fs, source, target, data);
}

/* --------------------------------------------------------------------------
 * Dummy RootFS (to bootstrap the VFS)
 * -------------------------------------------------------------------------- */

static dentry_t *dummy_lookup(struct inode *dir, struct dentry *dentry)
{
    (void)dir;
    return dentry; /* Negative dentry remains negative */
}

static inode_operations_t dummy_inode_ops = {
    .lookup = dummy_lookup,
};

void vfs_init(void)
{
    /* Create a dummy root super_block and dentry to anchor the tree */
    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    dentry_t *root_dentry = dcache_alloc(NULL, "/");
    
    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &dummy_inode_ops;
    
    root_dentry->d_inode = root_inode;
    root_dentry->d_sb = sb;
    
    sb->s_root = root_dentry;
    g_vfs_root = root_dentry;
    
    pr_debug("[VFS] Initialized dummy rootfs.\n");
}

/* --------------------------------------------------------------------------
 * Path Lookup & Resolution
 * -------------------------------------------------------------------------- */

static void path_add_comp(char comps[32][64], int *depth, const char *s, size_t len)
{
    if (len == 0) return;
    if (len == 1 && s[0] == '.') return;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
        if (*depth > 0) (*depth)--;
        return;
    }
    if (*depth < 32) {
        size_t cpy = (len < 63) ? len : 63;
        for (size_t i = 0; i < cpy; i++) comps[*depth][i] = s[i];
        comps[*depth][cpy] = '\0';
        (*depth)++;
    }
}

s64 vfs_resolve_path(const char *cwd, const char *path, char *out_buf, size_t out_len)
{
    if (!path || !out_buf || out_len < 2) return -(s64)EINVAL;

    char comps[32][64];
    int depth = 0;

    if (path[0] != '/') {
        const char *c = (cwd && cwd[0]) ? cwd : "/";
        while (*c) {
            while (*c == '/') c++;
            if (!*c) break;
            const char *start = c;
            while (*c && *c != '/') c++;
            path_add_comp(comps, &depth, start, (size_t)(c - start));
        }
    }

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        path_add_comp(comps, &depth, start, (size_t)(p - start));
    }

    if (depth == 0) {
        out_buf[0] = '/';
        out_buf[1] = '\0';
        return 0;
    }

    size_t out_pos = 0;
    for (int i = 0; i < depth; i++) {
        if (out_pos + 1 >= out_len) return -(s64)ENAMETOOLONG;
        out_buf[out_pos++] = '/';
        size_t clen = 0;
        while (comps[i][clen]) clen++;
        if (out_pos + clen >= out_len) return -(s64)ENAMETOOLONG;
        for (size_t j = 0; j < clen; j++) {
            out_buf[out_pos++] = comps[i][j];
        }
    }
    out_buf[out_pos] = '\0';
    return 0;
}

void dentry_build_path(dentry_t *d, char *buf, size_t max)
{
    if (!d || !buf || max < 2) return;
    if (d == g_vfs_root || !d->d_parent) {
        buf[0] = '/';
        buf[1] = '\0';
        return;
    }
    char stack[16][VFS_NAME_MAX];
    int count = 0;
    dentry_t *curr = d;
    while (curr && curr != g_vfs_root && curr->d_parent && count < 16) {
        strncpy(stack[count++], curr->d_name, VFS_NAME_MAX - 1);
        curr = curr->d_parent;
    }
    size_t off = 0;
    buf[off++] = '/';
    for (int i = count - 1; i >= 0; i--) {
        size_t nlen = strlen(stack[i]);
        if (off + nlen + 1 < max) {
            memcpy(buf + off, stack[i], nlen);
            off += nlen;
            if (i > 0) buf[off++] = '/';
        }
    }
    buf[off] = '\0';
}

static s64 vfs_path_lookup_internal(const char *path, dentry_t **out_dentry, int symlink_depth)
{
    if (!path || !g_vfs_root) return -(s64)ENOENT;
    if (symlink_depth > 16) return -(s64)ELOOP;
    
    char resolved[512];
    if (vfs_resolve_path("/", path, resolved, sizeof(resolved)) == 0) {
        path = resolved;
    } else if (path[0] != '/') {
        return -(s64)EINVAL;
    }
    
    dentry_t *curr = g_vfs_root;
    const char *p = path + 1;
    
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        
        char comp[VFS_NAME_MAX];
        int i = 0;
        while (*p && *p != '/' && i < VFS_NAME_MAX - 1) {
            comp[i++] = *p++;
        }
        
        if (*p && *p != '/') {
            return -(s64)ENAMETOOLONG;
        }
        comp[i] = '\0';
        
        dentry_t *next = dcache_lookup(curr, comp);
        if (!next) {
            /* Try to ask the filesystem's inode->lookup */
            if (!curr->d_inode || !curr->d_inode->i_op || !curr->d_inode->i_op->lookup) {
                return -(s64)ENOENT;
            }
            dentry_t *new_dentry = dcache_alloc(curr, comp);
            if (!new_dentry) return -(s64)ENOMEM;
            
            dentry_t *res = curr->d_inode->i_op->lookup(curr->d_inode, new_dentry);
            if (res) {
                /* If filesystem returned a different dentry, use it */
                if (res != new_dentry) {
                    kfree(new_dentry);
                }
                next = res;
                if (next && next->d_inode) {
                    dcache_add(next);
                }
            } else {
                next = new_dentry; /* Might be negative, meaning file doesn't exist */
            }
        }
        
        while (*p == '/') p++; /* Skip trailing slashes to check if this is the last component */
        
        if (!next->d_inode) {
            if (*p) {
                /* Missing intermediate directory */
                kfree(next);
                return -(s64)ENOENT;
            } else {
                *out_dentry = next;
                return -(s64)ENOENT;
            }
        }
        
        /* If component is a symbolic link, resolve it */
        if (S_ISLNK(next->d_inode->i_mode)) {
            char link_target[512];
            s64 read_res = -1;
            if (next->d_inode->i_op && next->d_inode->i_op->readlink) {
                read_res = next->d_inode->i_op->readlink(next, link_target, sizeof(link_target) - 1);
            }
            if (read_res > 0) {
                link_target[read_res] = '\0';
                char target_full[512];
                if (link_target[0] == '/') {
                    if (*p) {
                        snprintf(target_full, sizeof(target_full), "%s/%s", link_target, p);
                    } else {
                        strncpy(target_full, link_target, sizeof(target_full) - 1);
                        target_full[sizeof(target_full) - 1] = '\0';
                    }
                } else {
                    char parent_path[512];
                    dentry_build_path(curr, parent_path, sizeof(parent_path));
                    if (*p) {
                        snprintf(target_full, sizeof(target_full), "%s/%s/%s", parent_path, link_target, p);
                    } else {
                        snprintf(target_full, sizeof(target_full), "%s/%s", parent_path, link_target);
                    }
                }
                return vfs_path_lookup_internal(target_full, out_dentry, symlink_depth + 1);
            }
        }
        
        if (*p && !S_ISDIR(next->d_inode->i_mode)) {
            if (!next->d_parent || dcache_lookup(next->d_parent, next->d_name) != next) {
                kfree(next);
            }
            return -(s64)ENOTDIR;
        }
        
        curr = next;
    }
    
    *out_dentry = curr;
    return 0;
}

s64 vfs_path_lookup(const char *path, dentry_t **out_dentry)
{
    return vfs_path_lookup_internal(path, out_dentry, 0);
}


/* --------------------------------------------------------------------------
 * Syscall Implementations
 * -------------------------------------------------------------------------- */

/* vfs_open_err: like vfs_open but sets *out_errno on failure so callers
 * can distinguish ENOENT from EEXIST (BUG-10). */
file_t *vfs_open_err(const char *path, u32 flags, u32 mode, s64 *out_errno)
{
    if (out_errno) *out_errno = -(s64)ENOENT; /* default */

    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    
    if (err == 0 && dentry && dentry->d_inode && (flags & O_CREAT) && (flags & O_EXCL)) {
        /* BUG-10 fix: file exists and O_EXCL was requested → EEXIST */
        if (out_errno) *out_errno = -(s64)EEXIST;
        return NULL;
    }

    if (err == -(s64)ENOENT && dentry && (flags & O_CREAT)) {
        if (!dentry->d_parent || !dentry->d_parent->d_inode || !dentry->d_parent->d_inode->i_op || !dentry->d_parent->d_inode->i_op->create) {
            if (!dentry->d_inode) kfree(dentry);
            if (out_errno) *out_errno = -(s64)ENOENT;
            return NULL;
        }
        err = dentry->d_parent->d_inode->i_op->create(dentry->d_parent->d_inode, dentry, mode);
        if (err < 0) {
            if (!dentry->d_inode) kfree(dentry);
            if (out_errno) *out_errno = err;
            return NULL;
        }
        dcache_add(dentry);
    } else if (err < 0) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        if (out_errno) *out_errno = err;
        return NULL;
    }
    
    if (!dentry || !dentry->d_inode) { if (out_errno) *out_errno = -(s64)ENOENT; return NULL; }

    if ((flags & O_DIRECTORY) && !S_ISDIR(dentry->d_inode->i_mode)) {
        if (out_errno) *out_errno = -(s64)ENOTDIR;
        return NULL;
    }
    if (S_ISDIR(dentry->d_inode->i_mode) && ((flags & 3) == O_WRONLY || (flags & 3) == O_RDWR)) {
        if (out_errno) *out_errno = -(s64)EISDIR;
        return NULL;
    }
    
    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) { if (out_errno) *out_errno = -(s64)ENOMEM; return NULL; }
    
    f->f_dentry = dentry;
    f->f_inode  = dentry->d_inode;
    f->f_op     = dentry->d_inode->i_fop;
    f->f_flags  = flags;
    f->f_mode   = mode;
    f->f_pos    = (flags & O_APPEND) ? dentry->d_inode->i_size : 0;
    f->f_count  = 1;

    if ((flags & O_TRUNC) && ((flags & 3) == O_WRONLY || (flags & 3) == O_RDWR) && S_ISREG(dentry->d_inode->i_mode)) {
        vfs_truncate(f, 0);
        f->f_pos = 0;
    }
    
    if (f->f_op && f->f_op->open) {
        if (f->f_op->open(f->f_inode, f) < 0) {
            kfree(f);
            if (out_errno) *out_errno = -(s64)EIO;
            return NULL;
        }
    }
    if (out_errno) *out_errno = 0;
    return f;
}

file_t *vfs_open(const char *path, u32 flags, u32 mode)
{
    return vfs_open_err(path, flags, mode, NULL);
}

static inline bool is_valid_vfs_file(const file_t *file)
{
    if (!file) return false;
    uintptr_t addr = (uintptr_t)file;
    return (addr >= 0xFFFF800000000000ULL && addr < 0xFFFFFFFF80000000ULL);
}

static inline bool is_valid_vfs_fop(const file_operations_t *fop)
{
    if (!fop) return false;
    uintptr_t addr = (uintptr_t)fop;
    return (addr >= 0xFFFF800000000000ULL);
}


s64 vfs_close(file_t *file)
{
    if (!is_valid_vfs_file(file)) return -(s64)EBADF;
    
    if (__atomic_sub_fetch(&file->f_count, 1, __ATOMIC_SEQ_CST) != 0) {
        return 0; /* Still referenced by another handle, or already zero */
    }
    
    if (is_valid_vfs_fop(file->f_op) && file->f_op->release) {
        file->f_op->release(file->f_inode, file);
    }
    kfree(file);
    return 0;
}

s64 vfs_read(file_t *file, void *buf, size_t size)
{
    if (!is_valid_vfs_file(file) || !buf) return -(s64)EBADF;
    if (is_valid_vfs_fop(file->f_op) && file->f_op->read) {
        return file->f_op->read(file, buf, size, &file->f_pos);
    }
    return -(s64)EINVAL;
}

s64 vfs_write(file_t *file, const void *buf, size_t size)
{
    if (!is_valid_vfs_file(file) || !buf) return -(s64)EBADF;
    if (file->f_flags & O_APPEND && file->f_inode) {
        file->f_pos = file->f_inode->i_size;
    }
    if (is_valid_vfs_fop(file->f_op) && file->f_op->write) {
        return file->f_op->write(file, buf, size, &file->f_pos);
    }
    return -(s64)EINVAL;
}

s64 vfs_lseek(file_t *file, s64 offset, int whence)
{
    if (!is_valid_vfs_file(file) || !file->f_inode) return -(s64)EBADF;

    /* POSIX-01: lseek on a FIFO/pipe must return ESPIPE */
    if (S_ISFIFO(file->f_inode->i_mode)) return -(s64)ESPIPE;
    
    s64 new_pos = 0;
    switch (whence) {
        case SEEK_SET: new_pos = offset; break;
        case SEEK_CUR: new_pos = file->f_pos + offset; break;
        case SEEK_END: new_pos = file->f_inode->i_size + offset; break;
        default:       return -(s64)EINVAL;
    }
    if (new_pos < 0) return -(s64)EINVAL;
    file->f_pos = new_pos;
    return new_pos;
}

s64 vfs_stat(const char *path, struct stat *statbuf)
{
    if (!statbuf) return -(s64)EINVAL;
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return err;
    }
    if (!dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    
    inode_t *i = dentry->d_inode;
    __builtin_memset(statbuf, 0, sizeof(struct stat));
    statbuf->st_dev = 0;
    statbuf->st_ino = i->i_ino;
    statbuf->st_mode = i->i_mode;
    statbuf->st_uid = i->i_uid;
    statbuf->st_gid = i->i_gid;
    statbuf->st_size = i->i_size;
    statbuf->st_blocks = i->i_blocks;
    statbuf->st_atime = i->i_atime;
    statbuf->st_mtime = i->i_mtime;
    statbuf->st_ctime = i->i_ctime;
    
    return 0;
}

s64 vfs_fstat(file_t *file, struct stat *statbuf)
{
    if (!is_valid_vfs_file(file) || !file->f_inode) return -(s64)EBADF;
    if (!statbuf) return -(s64)EINVAL;
    
    inode_t *i = file->f_inode;
    __builtin_memset(statbuf, 0, sizeof(struct stat));
    statbuf->st_dev = 0;
    statbuf->st_ino = i->i_ino;
    statbuf->st_mode = i->i_mode;
    statbuf->st_uid = i->i_uid;
    statbuf->st_gid = i->i_gid;
    statbuf->st_size = i->i_size;
    statbuf->st_blocks = i->i_blocks;
    statbuf->st_atime = i->i_atime;
    statbuf->st_mtime = i->i_mtime;
    statbuf->st_ctime = i->i_ctime;
    
    return 0;
}

s64 vfs_ioctl(file_t *file, u32 cmd, u64 arg)
{
    if (!is_valid_vfs_file(file)) return -(s64)EBADF;
    if (is_valid_vfs_fop(file->f_op) && file->f_op->ioctl) {
        return file->f_op->ioctl(file, cmd, arg);
    }
    return -(s64)EINVAL;
}

s64 vfs_mkdir(const char *path, u32 mode)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err == -(s64)ENOENT && dentry) {
        if (!dentry->d_parent || !dentry->d_parent->d_inode ||
            !dentry->d_parent->d_inode->i_op || !dentry->d_parent->d_inode->i_op->mkdir) {
            if (!dentry->d_inode) kfree(dentry);
            return -(s64)ENOTDIR;
        }
        err = dentry->d_parent->d_inode->i_op->mkdir(dentry->d_parent->d_inode, dentry, mode);
        if (err < 0) {
            if (!dentry->d_inode) kfree(dentry);
            return err;
        }
        dcache_add(dentry);
        return 0;
    }
    if (err == 0) {
        return -(s64)EEXIST;
    }
    if (dentry && !dentry->d_inode) kfree(dentry);
    return err;
}

s64 vfs_unlink(const char *path)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return err;
    }
    if (!dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (S_ISDIR(dentry->d_inode->i_mode)) {
        return -(s64)EISDIR;
    }
    if (!dentry->d_parent || !dentry->d_parent->d_inode ||
        !dentry->d_parent->d_inode->i_op || !dentry->d_parent->d_inode->i_op->unlink) {
        return -(s64)EPERM;
    }
    err = dentry->d_parent->d_inode->i_op->unlink(dentry->d_parent->d_inode, dentry);
    if (err == 0) {
        dcache_remove(dentry);
    }
    return err;
}

s64 vfs_rmdir(const char *path)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return err;
    }
    if (!dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (!S_ISDIR(dentry->d_inode->i_mode)) {
        return -(s64)ENOTDIR;
    }
    if (!dentry->d_parent || !dentry->d_parent->d_inode ||
        !dentry->d_parent->d_inode->i_op || !dentry->d_parent->d_inode->i_op->rmdir) {
        return -(s64)EPERM;
    }
    err = dentry->d_parent->d_inode->i_op->rmdir(dentry->d_parent->d_inode, dentry);
    if (err == 0) {
        dcache_remove(dentry);
    }
    return err;
}

s64 vfs_rename(const char *oldpath, const char *newpath)
{
    dentry_t *old_dentry = NULL, *new_dentry = NULL;
    s64 err = vfs_path_lookup(oldpath, &old_dentry);
    if (err < 0 || !old_dentry || !old_dentry->d_inode) {
        if (old_dentry && !old_dentry->d_inode) kfree(old_dentry);
        return -(s64)ENOENT;
    }
    err = vfs_path_lookup(newpath, &new_dentry);
    if (err < 0 && err != -(s64)ENOENT) {
        if (new_dentry && !new_dentry->d_inode) kfree(new_dentry);
        return err;
    }
    if (!new_dentry) return -(s64)ENOENT;
    
    if (!old_dentry->d_parent || !old_dentry->d_parent->d_inode ||
        !old_dentry->d_parent->d_inode->i_op || !old_dentry->d_parent->d_inode->i_op->rename ||
        !new_dentry->d_parent || !new_dentry->d_parent->d_inode) {
        if (!new_dentry->d_inode) kfree(new_dentry);
        return -(s64)EPERM;
    }
    
    err = old_dentry->d_parent->d_inode->i_op->rename(
        old_dentry->d_parent->d_inode, old_dentry,
        new_dentry->d_parent->d_inode, new_dentry
    );
    if (err == 0) {
        dcache_remove(old_dentry);
        if (new_dentry->d_inode) {
            dcache_add(new_dentry);
        }
    } else if (!new_dentry->d_inode) {
        kfree(new_dentry);
    }
    return err;
}

s64 vfs_truncate(file_t *file, u64 length)
{
    if (!file || !file->f_inode) return -(s64)EBADF;
    if (length == 0) {
        extern void ext2_truncate(struct inode *inode);
        if (file->f_inode->i_sb && file->f_inode->i_sb->s_magic == 0xEF53) {
            ext2_truncate(file->f_inode);
        } else {
            file->f_inode->i_size = 0;
        }
    } else {
        file->f_inode->i_size = length;
    }
    return 0;
}

s64 vfs_symlink(const char *target, const char *linkpath)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(linkpath, &dentry);
    if (err == -(s64)ENOENT && dentry) {
        if (!dentry->d_parent || !dentry->d_parent->d_inode ||
            !dentry->d_parent->d_inode->i_op || !dentry->d_parent->d_inode->i_op->symlink) {
            if (!dentry->d_inode) kfree(dentry);
            return -(s64)ENOTDIR;
        }
        err = dentry->d_parent->d_inode->i_op->symlink(dentry->d_parent->d_inode, dentry, target);
        if (err < 0) {
            if (!dentry->d_inode) kfree(dentry);
            return err;
        }
        dcache_add(dentry);
        return 0;
    }
    if (err == 0) return -(s64)EEXIST;
    if (dentry && !dentry->d_inode) kfree(dentry);
    return err;
}

s64 vfs_lstat(const char *path, struct stat *statbuf)
{
    /* Without symlink following, lstat is identical to stat for now */
    return vfs_stat(path, statbuf);
}

s64 vfs_readlink(const char *path, char *buf, size_t bufsiz)
{
    if (!path || !buf || bufsiz == 0) return -(s64)EINVAL;
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (!S_ISLNK(dentry->d_inode->i_mode)) {
        return -(s64)EINVAL;
    }
    if (dentry->d_inode->i_op && dentry->d_inode->i_op->readlink) {
        return dentry->d_inode->i_op->readlink(dentry, buf, bufsiz);
    }
    return -(s64)ENOSYS;
}

s64 vfs_chmod(const char *path, u32 mode)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    dentry->d_inode->i_mode = (dentry->d_inode->i_mode & S_IFMT) | (mode & ~S_IFMT);
    return 0;
}

s64 vfs_fchmod(file_t *file, u32 mode)
{
    if (!file || !file->f_inode) return -(s64)EBADF;
    file->f_inode->i_mode = (file->f_inode->i_mode & S_IFMT) | (mode & ~S_IFMT);
    return 0;
}

s64 vfs_chown(const char *path, u32 uid, u32 gid)
{
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0 || !dentry || !dentry->d_inode) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    if (uid != (u32)-1) dentry->d_inode->i_uid = uid;
    if (gid != (u32)-1) dentry->d_inode->i_gid = gid;
    return 0;
}

s64 vfs_fchown(file_t *file, u32 uid, u32 gid)
{
    if (!file || !file->f_inode) return -(s64)EBADF;
    if (uid != (u32)-1) file->f_inode->i_uid = uid;
    if (gid != (u32)-1) file->f_inode->i_gid = gid;
    return 0;
}

s64 vfs_statfs(const char *path, struct statfs *buf)
{
    if (!buf) return -(s64)EINVAL;
    dentry_t *dentry = NULL;
    s64 err = vfs_path_lookup(path, &dentry);
    if (err < 0 || !dentry || !dentry->d_sb) {
        if (dentry && !dentry->d_inode) kfree(dentry);
        return -(s64)ENOENT;
    }
    super_block_t *sb = dentry->d_sb;
    if (sb->s_op && sb->s_op->statfs) {
        return sb->s_op->statfs(sb, buf);
    }
    /* Fallback generic stats */
    __builtin_memset(buf, 0, sizeof(struct statfs));
    buf->f_type = sb->s_magic;
    buf->f_bsize = sb->s_blocksize ? sb->s_blocksize : 4096;
    buf->f_blocks = 262144;
    buf->f_bfree = 131072;
    buf->f_bavail = 131072;
    buf->f_files = 65536;
    buf->f_ffree = 32768;
    buf->f_namelen = VFS_NAME_MAX;
    buf->f_frsize = buf->f_bsize;
    return 0;
}

s64 vfs_fstatfs(file_t *file, struct statfs *buf)
{
    if (!file || !file->f_inode || !file->f_inode->i_sb) return -(s64)EBADF;
    if (!buf) return -(s64)EINVAL;
    super_block_t *sb = file->f_inode->i_sb;
    if (sb->s_op && sb->s_op->statfs) {
        return sb->s_op->statfs(sb, buf);
    }
    __builtin_memset(buf, 0, sizeof(struct statfs));
    buf->f_type = sb->s_magic;
    buf->f_bsize = sb->s_blocksize ? sb->s_blocksize : 4096;
    buf->f_blocks = 262144;
    buf->f_bfree = 131072;
    buf->f_bavail = 131072;
    buf->f_files = 65536;
    buf->f_ffree = 32768;
    buf->f_namelen = VFS_NAME_MAX;
    buf->f_frsize = buf->f_bsize;
    return 0;
}

