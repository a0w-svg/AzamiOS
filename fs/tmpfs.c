/* ============================================================================
 * AzamiOS — tmpfs: RAM-backed volatile filesystem
 * File: fs/tmpfs.c
 *
 * Design overview
 * ───────────────
 * Every directory entry and file is a tmpfs_node_t allocated on the kernel
 * heap.  File data lives in a single kmalloc'd buffer that is grown (doubled)
 * on demand up to TMPFS_MAX_FILE_SIZE.  Directory entries are stored as a
 * singly-linked list of tmpfs_node_t children hanging off the parent node.
 *
 * The VFS inode's i_private pointer always points at the owning tmpfs_node_t,
 * which makes all operations O(children) at worst — fine for /tmp.
 *
 * Capabilities
 * ────────────
 *   • read, write (growing), lseek, truncate
 *   • mkdir, rmdir (only if empty), unlink, rename
 *   • symlink, readlink
 *   • stat / statfs  (type = TMPFS_MAGIC = 0x01021994)
 *   • readdir (getdents64)
 *   • open / release
 *
 * Thread safety
 * ─────────────
 * The kernel's global big-lock protects all VFS calls for now, so no
 * additional locking is added here.
 * ============================================================================ */

#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../kernel/mm/pmm.h"
#include "../kernel/sched/sched.h"
#include "../kernel/uaccess.h"
#include "vfs.h"
#include "tmpfs.h"

/* ── tunables ─────────────────────────────────────────────────────────────── */
#define TMPFS_MAGIC          0x01021994UL   /* matches Linux's value          */
#define TMPFS_NAME_MAX       255
#define TMPFS_MAX_FILE_SIZE  (64UL * 1024 * 1024)  /* 64 MiB per file        */
#define TMPFS_INITIAL_BUF    256            /* initial data buffer size       */
#define TMPFS_INO_ROOT       1              /* inode number of root directory */

/* ── in-memory node ───────────────────────────────────────────────────────── */
typedef struct tmpfs_node {
    char    name[TMPFS_NAME_MAX + 1];
    u32     mode;       /* S_IFDIR / S_IFREG / S_IFLNK | permissions */
    u32     uid, gid;
    u64     ino;
    u64     atime, mtime, ctime;
    u64     size;       /* file: bytes used; dir: 0; symlink: link len */

    /* Regular file data */
    u8     *data;       /* heap buffer; NULL for dirs / symlinks       */
    u64     capacity;   /* allocated bytes                             */

    /* Symlink target */
    char   *link;       /* heap buffer; only for S_IFLNK               */

    /* Directory children (singly-linked list) */
    struct tmpfs_node *children;  /* first child (for dirs)             */
    struct tmpfs_node *next;      /* next sibling in parent's list      */

    /* Hard link target (NULL if this is the primary node) */
    struct tmpfs_node *target;

    /* VFS inode back-pointer */
    inode_t *inode;

    /* Reference count: directory link + open file handles */
    u32     refcount;
} tmpfs_node_t;

/* ── superblock private data ─────────────────────────────────────────────── */
typedef struct {
    tmpfs_node_t *root;
    u64           next_ino;
    u64           file_count;
    u64           total_bytes;
} tmpfs_sb_t;

/* ── inode counter ────────────────────────────────────────────────────────── */
static u64 s_next_ino = TMPFS_INO_ROOT + 1;

/* ── forward declarations ──────────────────────────────────────────────────── */
static inode_t *tmpfs_alloc_inode(super_block_t *sb);
static void     tmpfs_destroy_inode(inode_t *inode);
static void     tmpfs_write_inode(inode_t *inode);
static void     tmpfs_put_super(super_block_t *sb);
static s64      tmpfs_statfs_op(super_block_t *sb, struct statfs *buf);

static struct dentry *tmpfs_lookup(inode_t *dir, dentry_t *dentry);
static s64      tmpfs_create(inode_t *dir, dentry_t *dentry, u32 mode);
static s64      tmpfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode);
static s64      tmpfs_unlink(inode_t *dir, dentry_t *dentry);
static s64      tmpfs_rmdir(inode_t *dir, dentry_t *dentry);
static s64      tmpfs_rename(inode_t *old_dir, dentry_t *old_dentry,
                             inode_t *new_dir, dentry_t *new_dentry);
static s64      tmpfs_symlink(inode_t *dir, dentry_t *dentry, const char *sym);
static s64      tmpfs_readlink(dentry_t *dentry, char *buf, size_t buflen);
static s64      tmpfs_link(inode_t *dir, dentry_t *old_dentry, dentry_t *dentry);

static s64      tmpfs_file_read(file_t *filp, void *buf, size_t len, u64 *off);
static s64      tmpfs_file_write(file_t *filp, const void *buf, size_t len, u64 *off);
static s64      tmpfs_file_readdir(file_t *filp, void *dirent_buf, size_t len, u64 *off);
static s64      tmpfs_file_open(inode_t *inode, file_t *filp);
static s64      tmpfs_file_release(inode_t *inode, file_t *filp);
static s64      tmpfs_file_mmap(file_t *filp, virt_addr_t vaddr, size_t len,
                                u32 prot, u32 flags, u64 offset);
static s64      tmpfs_file_ioctl(file_t *filp, u32 cmd, u64 arg);
static int      tmpfs_file_poll(file_t *filp);
static s64      tmpfs_mount(file_system_type_t *fs_type, const char *dev,
                            const char *dir, void *data);

/* ── vtables ─────────────────────────────────────────────────────────────── */

static super_operations_t s_tmpfs_super_ops = {
    .alloc_inode   = tmpfs_alloc_inode,
    .destroy_inode = tmpfs_destroy_inode,
    .write_inode   = tmpfs_write_inode,
    .put_super     = tmpfs_put_super,
    .statfs        = tmpfs_statfs_op,
};

static inode_operations_t s_tmpfs_dir_iops = {
    .lookup   = tmpfs_lookup,
    .create   = tmpfs_create,
    .mkdir    = tmpfs_mkdir,
    .unlink   = tmpfs_unlink,
    .rmdir    = tmpfs_rmdir,
    .rename   = tmpfs_rename,
    .symlink  = tmpfs_symlink,
    .readlink = NULL,
    .link     = tmpfs_link,
};

static inode_operations_t s_tmpfs_file_iops = {
    .lookup   = NULL,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .symlink  = NULL,
    .readlink = NULL,
    .link     = NULL,
};

static inode_operations_t s_tmpfs_link_iops = {
    .lookup   = NULL,
    .create   = NULL,
    .mkdir    = NULL,
    .unlink   = NULL,
    .rmdir    = NULL,
    .rename   = NULL,
    .symlink  = NULL,
    .readlink = tmpfs_readlink,
    .link     = NULL,
};

static file_operations_t s_tmpfs_file_fops = {
    .read    = tmpfs_file_read,
    .write   = tmpfs_file_write,
    .readdir = NULL,
    .ioctl   = tmpfs_file_ioctl,
    .mmap    = tmpfs_file_mmap,
    .open    = tmpfs_file_open,
    .release = tmpfs_file_release,
    .poll    = tmpfs_file_poll,
};

static file_operations_t s_tmpfs_dir_fops = {
    .read    = NULL,
    .write   = NULL,
    .readdir = tmpfs_file_readdir,
    .ioctl   = NULL,
    .mmap    = NULL,
    .open    = tmpfs_file_open,
    .release = tmpfs_file_release,
    .poll    = tmpfs_file_poll,
};

static file_system_type_t s_tmpfs_type = {
    .name  = "tmpfs",
    .mount = tmpfs_mount,
    .next  = NULL,
};

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Allocate and zero-initialise a new tmpfs_node_t */
static tmpfs_node_t *tmpfs_new_node(const char *name, u32 mode)
{
    tmpfs_node_t *n = (tmpfs_node_t *)kmalloc(sizeof(tmpfs_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(tmpfs_node_t));

    size_t nlen = strlen(name);
    if (nlen > TMPFS_NAME_MAX) nlen = TMPFS_NAME_MAX;
    memcpy(n->name, name, nlen);
    n->name[nlen] = '\0';
    n->mode = mode;
    n->ino      = s_next_ino++;
    n->refcount = 1;

    /* Record creation time as zero — we have no clock here but timestamps
     * will be refreshed by the VFS on each write anyway. */
    return n;
}

/* Free a node and its data / link buffers (NOT its children — caller must
 * handle the list first to avoid leaks). */
static void tmpfs_free_node(tmpfs_node_t *n)
{
    if (!n) return;
    if (n->data) kfree(n->data);
    if (n->link) kfree(n->link);
    kfree(n);
}

/* Decrement refcount and free node + inode when no directory entry and no
 * open file handles remain. */
static void tmpfs_node_put(tmpfs_node_t *n)
{
    if (!n) return;
    if (n->refcount > 0) {
        n->refcount--;
    }
    if (n->refcount == 0) {
        if (n->target) {
            tmpfs_node_put(n->target);
            n->target = NULL;
        } else if (n->inode) {
            kfree(n->inode);
            n->inode = NULL;
        }
        tmpfs_free_node(n);
    }
}

/* Link child into parent's children list */
static void tmpfs_dir_add(tmpfs_node_t *parent, tmpfs_node_t *child)
{
    child->next      = parent->children;
    parent->children = child;
    parent->size++;
}

/* Detach child from parent's children list.  Returns 1 if found, 0 if not. */
static int tmpfs_dir_remove(tmpfs_node_t *parent, tmpfs_node_t *child)
{
    tmpfs_node_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next;
            child->next = NULL;
            if (parent->size > 0) parent->size--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

/* Find a child by name */
static tmpfs_node_t *tmpfs_dir_find(tmpfs_node_t *dir, const char *name)
{
    tmpfs_node_t *c = dir->children;
    while (c) {
        if (strcmp(c->name, name) == 0) return c;
        c = c->next;
    }
    return NULL;
}

/* Grow the file's data buffer to at least `needed` bytes */
static int tmpfs_grow(tmpfs_node_t *n, u64 needed)
{
    if (needed <= n->capacity) return 0;
    if (needed > TMPFS_MAX_FILE_SIZE) return -1;

    u64 nc = n->capacity ? n->capacity : TMPFS_INITIAL_BUF;
    while (nc < needed) nc *= 2;
    if (nc > TMPFS_MAX_FILE_SIZE) nc = TMPFS_MAX_FILE_SIZE;

    u8 *nb = (u8 *)kmalloc((size_t)nc);
    if (!nb) return -1;
    if (n->data && n->size > 0)
        memcpy(nb, n->data, (size_t)n->size);
    if (nc > n->size)
        memset(nb + n->size, 0, (size_t)(nc - n->size));
    if (n->data) kfree(n->data);
    n->data     = nb;
    n->capacity = nc;
    return 0;
}

/* Build a VFS inode backed by a tmpfs node */
static inode_t *tmpfs_make_inode(super_block_t *sb, tmpfs_node_t *node)
{
    inode_t *ino = (inode_t *)kmalloc(sizeof(inode_t));
    if (!ino) return NULL;
    memset(ino, 0, sizeof(inode_t));

    ino->i_ino     = node->ino;
    ino->i_mode    = node->mode;
    ino->i_uid     = node->uid;
    ino->i_gid     = node->gid;
    ino->i_size    = (s64)node->size;
    ino->i_blocks  = (node->size + 511) / 512;
    ino->i_atime   = node->atime;
    ino->i_mtime   = node->mtime;
    ino->i_ctime   = node->ctime;
    ino->i_sb      = sb;
    ino->i_private = node;
    ino->i_nlink   = S_ISDIR(node->mode) ? 2 : 1;

    if (S_ISDIR(node->mode)) {
        ino->i_op  = &s_tmpfs_dir_iops;
        ino->i_fop = &s_tmpfs_dir_fops;
    } else if (S_ISLNK(node->mode)) {
        ino->i_op  = &s_tmpfs_link_iops;
        ino->i_fop = &s_tmpfs_file_fops;
    } else {
        ino->i_op  = &s_tmpfs_file_iops;
        ino->i_fop = &s_tmpfs_file_fops;
    }
    node->inode = ino;
    return ino;
}

/* ── super_operations ─────────────────────────────────────────────────────── */

static inode_t *tmpfs_alloc_inode(super_block_t *sb)
{
    inode_t *ino = (inode_t *)kmalloc(sizeof(inode_t));
    if (!ino) return NULL;
    memset(ino, 0, sizeof(inode_t));
    ino->i_sb = sb;
    return ino;
}

static void tmpfs_destroy_inode(inode_t *inode)
{
    if (!inode) return;
    kfree(inode);
}

/* Flush VFS inode metadata back into the underlying tmpfs_node_t so that
 * stat() always returns consistent values after chmod/chown/utimes. */
static void tmpfs_write_inode(inode_t *inode)
{
    if (!inode) return;
    tmpfs_node_t *node = (tmpfs_node_t *)inode->i_private;
    if (!node) return;

    node->mode  = inode->i_mode;
    node->uid   = inode->i_uid;
    node->gid   = inode->i_gid;
    node->atime = inode->i_atime;
    node->mtime = inode->i_mtime;
    node->ctime = inode->i_ctime;
    /* i_size / i_blocks are kept in sync by read/write/truncate already. */
}

/* Recursively free every tmpfs_node_t reachable from @n (depth-first). */
static void tmpfs_free_tree(tmpfs_node_t *n)
{
    if (!n) return;
    /* Recurse into children first so no child is orphaned. */
    tmpfs_node_t *c = n->children;
    while (c) {
        tmpfs_node_t *next = c->next;
        tmpfs_free_tree(c);
        c = next;
    }
    /* Hard-link stubs only reference the real node; the data was already
     * freed when the primary node was visited. */
    if (!n->target) {
        if (n->data) { kfree(n->data); n->data = NULL; }
        if (n->link) { kfree(n->link); n->link = NULL; }
        if (n->inode) { kfree(n->inode); n->inode = NULL; }
    }
    kfree(n);
}

/* Release all resources held by a tmpfs superblock. Called by vfs_umount(). */
static void tmpfs_put_super(super_block_t *sb)
{
    if (!sb) return;
    tmpfs_sb_t *priv = (tmpfs_sb_t *)sb->s_fs_info;
    if (priv) {
        if (priv->root) {
            tmpfs_free_tree(priv->root);
            priv->root = NULL;
        }
        kfree(priv);
        sb->s_fs_info = NULL;
    }
    kfree(sb);
}

static s64 tmpfs_statfs_op(super_block_t *sb, struct statfs *buf)
{
    if (!buf) return -22; /* EINVAL */
    memset(buf, 0, sizeof(*buf));
    buf->f_type    = TMPFS_MAGIC;
    buf->f_bsize   = 4096;
    buf->f_blocks  = TMPFS_MAX_FILE_SIZE / 4096;
    buf->f_bfree   = buf->f_blocks; /* rough approximation */
    buf->f_bavail  = buf->f_blocks;
    buf->f_namelen = TMPFS_NAME_MAX;
    (void)sb;
    return 0;
}

/* ── inode_operations ─────────────────────────────────────────────────────── */

static struct dentry *tmpfs_lookup(inode_t *dir, dentry_t *dentry)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return NULL;

    tmpfs_node_t *child = tmpfs_dir_find(parent, dentry->d_name);
    if (!child) {
        dentry->d_inode = NULL;
        return dentry; /* negative dentry */
    }

    if (child->target) {
        dentry->d_inode = child->target->inode;
        return dentry;
    }

    if (!child->inode)
        tmpfs_make_inode(dir->i_sb, child);

    dentry->d_inode = child->inode;
    return dentry;
}

static s64 tmpfs_create(inode_t *dir, dentry_t *dentry, u32 mode)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;

    if (tmpfs_dir_find(parent, dentry->d_name)) return -17; /* EEXIST */

    tmpfs_node_t *node = tmpfs_new_node(dentry->d_name, mode | 0100000 /* S_IFREG */);
    if (!node) return -12; /* ENOMEM */

    inode_t *ino = tmpfs_make_inode(dir->i_sb, node);
    if (!ino) { tmpfs_free_node(node); return -12; }

    tmpfs_dir_add(parent, node);
    dentry->d_inode = ino;
    return 0;
}

static s64 tmpfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;

    if (tmpfs_dir_find(parent, dentry->d_name)) return -17;

    tmpfs_node_t *node = tmpfs_new_node(dentry->d_name, mode | 0040000 /* S_IFDIR */);
    if (!node) return -12;

    inode_t *ino = tmpfs_make_inode(dir->i_sb, node);
    if (!ino) { tmpfs_free_node(node); return -12; }

    tmpfs_dir_add(parent, node);
    if (dir->i_nlink > 0) dir->i_nlink++;
    dentry->d_inode = ino;
    return 0;
}

static s64 tmpfs_unlink(inode_t *dir, dentry_t *dentry)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;

    tmpfs_node_t *node = tmpfs_dir_find(parent, dentry->d_name);
    if (!node) return -2; /* ENOENT */
    if (S_ISDIR(node->mode)) return -21; /* EISDIR */

    tmpfs_dir_remove(parent, node);
    if (node->inode && node->inode->i_nlink > 0) {
        node->inode->i_nlink--;
    }
    tmpfs_node_put(node);
    dentry->d_inode = NULL;
    return 0;
}

static s64 tmpfs_rmdir(inode_t *dir, dentry_t *dentry)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;

    tmpfs_node_t *node = tmpfs_dir_find(parent, dentry->d_name);
    if (!node) return -2;
    if (!S_ISDIR(node->mode)) return -20; /* ENOTDIR */
    if (node->children) return -39;       /* ENOTEMPTY */

    tmpfs_dir_remove(parent, node);
    if (dir->i_nlink > 2) dir->i_nlink--;
    tmpfs_node_put(node);
    dentry->d_inode = NULL;
    return 0;
}

static s64 tmpfs_rename(inode_t *old_dir, dentry_t *old_dentry,
                         inode_t *new_dir, dentry_t *new_dentry)
{
    tmpfs_node_t *src_parent = (tmpfs_node_t *)old_dir->i_private;
    tmpfs_node_t *dst_parent = (tmpfs_node_t *)new_dir->i_private;
    if (!src_parent || !dst_parent) return -22;

    tmpfs_node_t *node = tmpfs_dir_find(src_parent, old_dentry->d_name);
    if (!node) return -2;

    /* Remove any existing destination entry */
    tmpfs_node_t *existing = tmpfs_dir_find(dst_parent, new_dentry->d_name);
    if (existing) {
        tmpfs_dir_remove(dst_parent, existing);
        tmpfs_node_put(existing);
    }

    tmpfs_dir_remove(src_parent, node);

    /* Update name */
    size_t nlen = strlen(new_dentry->d_name);
    if (nlen > TMPFS_NAME_MAX) nlen = TMPFS_NAME_MAX;
    memcpy(node->name, new_dentry->d_name, nlen);
    node->name[nlen] = '\0';

    tmpfs_dir_add(dst_parent, node);
    new_dentry->d_inode = node->inode;
    old_dentry->d_inode = NULL;
    return 0;
}

static s64 tmpfs_symlink(inode_t *dir, dentry_t *dentry, const char *sym)
{
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;
    if (tmpfs_dir_find(parent, dentry->d_name)) return -17;

    tmpfs_node_t *node = tmpfs_new_node(dentry->d_name, 0120777 /* S_IFLNK | 0777 */);
    if (!node) return -12;

    size_t slen = strlen(sym);
    node->link = (char *)kmalloc(slen + 1);
    if (!node->link) { tmpfs_free_node(node); return -12; }
    memcpy(node->link, sym, slen + 1);
    node->size = slen;

    inode_t *ino = tmpfs_make_inode(dir->i_sb, node);
    if (!ino) { tmpfs_free_node(node); return -12; }

    tmpfs_dir_add(parent, node);
    dentry->d_inode = ino;
    return 0;
}

static s64 tmpfs_readlink(dentry_t *dentry, char *buf, size_t buflen)
{
    if (!dentry->d_inode) return -22;
    tmpfs_node_t *node = (tmpfs_node_t *)dentry->d_inode->i_private;
    if (!node || !node->link) return -22;
    size_t copy = node->size < buflen - 1 ? node->size : buflen - 1;
    memcpy(buf, node->link, copy);
    buf[copy] = '\0';
    return (s64)copy;
}

static s64 tmpfs_link(inode_t *dir, dentry_t *old_dentry, dentry_t *new_dentry)
{
    /* Hard links in tmpfs: point new_dentry at the same inode and record directory entry */
    if (!old_dentry->d_inode) return -2;
    tmpfs_node_t *parent = (tmpfs_node_t *)dir->i_private;
    if (!parent) return -22;

    if (tmpfs_dir_find(parent, new_dentry->d_name)) return -17; /* EEXIST */

    tmpfs_node_t *old_node = (tmpfs_node_t *)old_dentry->d_inode->i_private;
    if (!old_node) return -2;
    while (old_node->target) old_node = old_node->target;

    tmpfs_node_t *link_node = tmpfs_new_node(new_dentry->d_name, old_node->mode);
    if (!link_node) return -12;

    link_node->ino    = old_node->ino;
    link_node->target = old_node;
    link_node->inode  = old_dentry->d_inode;
    old_node->refcount++;

    tmpfs_dir_add(parent, link_node);
    new_dentry->d_inode = old_dentry->d_inode;
    old_dentry->d_inode->i_nlink++;
    return 0;
}

/* ── file_operations ──────────────────────────────────────────────────────── */

static s64 tmpfs_file_open(inode_t *inode, file_t *filp)
{
    (void)filp;
    if (!inode) return -9;
    tmpfs_node_t *node = (tmpfs_node_t *)inode->i_private;
    if (node) {
        node->refcount++;
    }
    return 0;
}

static s64 tmpfs_file_release(inode_t *inode, file_t *filp)
{
    (void)filp;
    if (!inode) return 0;
    tmpfs_node_t *node = (tmpfs_node_t *)inode->i_private;
    if (node) {
        tmpfs_node_put(node);
    }
    return 0;
}

/* ── FIONREAD ioctl command value (Linux/x86-64) ─────────────────────────── */
#define TMPFS_FIONREAD  0x541Bu   /* bytes available to read at current offset */
#define TMPFS_FIOCLEX   0x5451u   /* set close-on-exec flag (no-op here)       */
#define TMPFS_FIONCLEX  0x5450u   /* clear close-on-exec flag (no-op here)     */
#define TMPFS_FIONBIO   0x5421u   /* set/clear non-blocking (no-op for files)  */

/* ioctl for regular tmpfs files.  Only FIONREAD has a useful answer; all
 * other tty/socket commands are not applicable and return -ENOTTY. */
static s64 tmpfs_file_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->f_inode) return -9; /* EBADF */

    switch (cmd) {
    case TMPFS_FIONREAD: {
        /* Report bytes remaining from the current file position to EOF. */
        tmpfs_node_t *node = (tmpfs_node_t *)filp->f_inode->i_private;
        if (!node) return -9;
        s64 avail = 0;
        if (filp->f_pos < node->size)
            avail = (s64)(node->size - filp->f_pos);
        if (copy_to_user((void *)(uintptr_t)arg, &avail, sizeof(int)) != 0)
            return -14; /* EFAULT */
        return 0;
    }
    case TMPFS_FIOCLEX:   /* fall-through: per-fd flag managed by the FD table */
    case TMPFS_FIONCLEX:
    case TMPFS_FIONBIO:
        return 0;         /* accepted, no-op at the filesystem level */
    default:
        return -25;       /* ENOTTY — not a tty/socket-capable device */
    }
}

/* Regular files are always ready for both reading and writing; return the
 * full POLLIN | POLLOUT mask so that select/poll never blocks on them. */
static int tmpfs_file_poll(file_t *filp)
{
    (void)filp;
    /* 0x0001 = POLLIN, 0x0004 = POLLOUT (matches syscall.c defines) */
    return 0x0001 | 0x0004;
}

/* Map a tmpfs file into the calling process's address space.
 *
 * For MAP_SHARED the on-disk data is the tmpfs_node buffer itself; we copy
 * each requested page out of that buffer into a freshly-allocated physical
 * frame and map it into the process PML4.  True page-level sharing (multiple
 * processes mapping the same frame) would require a frame reference-count
 * table that does not yet exist, so MAP_SHARED degrades to MAP_PRIVATE here —
 * writes to the mapping are *not* reflected back to the file.  This matches
 * the behaviour of the anonymous-mmap fallback already used by sys_mmap_impl()
 * when no ->mmap handler is present, but now the initial content is correct. */
static s64 tmpfs_file_mmap(file_t *filp, virt_addr_t vaddr, size_t len,
                           u32 prot, u32 flags, u64 offset)
{
    if (!filp || !filp->f_inode) return -9;  /* EBADF  */
    if (len == 0)                 return -22; /* EINVAL */

    tmpfs_node_t *node = (tmpfs_node_t *)filp->f_inode->i_private;
    if (!node || !S_ISREG(node->mode)) return -22; /* EINVAL */

    /* offset must be page-aligned (POSIX). */
    if (offset & (PAGE_SIZE - 1)) return -22;

    process_t *proc = sched_current_process();
    if (!proc) return -1; /* EPERM */

    /* Build PTE flags from PROT_* bits (matching sys_mmap_impl). */
    u64 vmm_fl = VMM_F_PRESENT | VMM_F_USER | VMM_F_NX;
    if (prot & 0x2 /* PROT_WRITE */) vmm_fl |= VMM_F_WRITE;
    if (prot & 0x4 /* PROT_EXEC  */) vmm_fl &= ~VMM_F_NX;
    if (prot == 0  /* PROT_NONE  */) vmm_fl &= ~VMM_F_PRESENT;

    size_t aligned_len = (len + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE - 1);

    for (size_t done = 0; done < aligned_len; done += PAGE_SIZE) {
        phys_addr_t phys = pmm_alloc_page();
        if (!phys) {
            /* On OOM, unmap the frames already installed in this call. */
            if (done > 0)
                vmm_unmap_range(proc->pml4_phys, vaddr, done / PAGE_SIZE, true);
            return -12; /* ENOMEM */
        }

        /* Zero the frame first so gaps beyond EOF are clean. */
        void *kpage = (void *)PHYS_TO_VIRT(phys);
        memset(kpage, 0, PAGE_SIZE);

        /* Copy file data for pages that overlap the file content. */
        u64 file_pos = offset + done;
        if (node->data && file_pos < node->size) {
            size_t copy = node->size - file_pos;
            if (copy > PAGE_SIZE) copy = PAGE_SIZE;
            memcpy(kpage, node->data + file_pos, copy);
        }

        if (vmm_map(proc->pml4_phys, vaddr + done, phys, vmm_fl) != 0) {
            pmm_free_page(phys);
            if (done > 0)
                vmm_unmap_range(proc->pml4_phys, vaddr, done / PAGE_SIZE, true);
            return -12; /* ENOMEM */
        }
    }
    return 0;
}

static s64 tmpfs_file_read(file_t *filp, void *buf, size_t len, u64 *off)
{
    if (!filp->f_inode) return -9; /* EBADF */
    tmpfs_node_t *node = (tmpfs_node_t *)filp->f_inode->i_private;
    if (!node) return -9;
    if (!node->data || *off >= node->size) return 0;

    u64 avail = node->size - *off;
    if ((u64)len > avail) len = (size_t)avail;
    memcpy(buf, node->data + *off, len);
    *off += len;
    return (s64)len;
}

static s64 tmpfs_file_write(file_t *filp, const void *buf, size_t len, u64 *off)
{
    if (!filp->f_inode) return -9;
    tmpfs_node_t *node = (tmpfs_node_t *)filp->f_inode->i_private;
    if (!node) return -9;
    if (len == 0) return 0;

    /* Check for integer overflow */
    if (*off > TMPFS_MAX_FILE_SIZE || (u64)len > TMPFS_MAX_FILE_SIZE ||
        *off + (u64)len < *off || *off + (u64)len > TMPFS_MAX_FILE_SIZE) {
        if (*off >= TMPFS_MAX_FILE_SIZE) return -28; /* ENOSPC */
        len = (size_t)(TMPFS_MAX_FILE_SIZE - *off);
    }

    u64 end = *off + (u64)len;
    if (tmpfs_grow(node, end) != 0) return -28;

    /* Zero any gap between old EOF and new write offset */
    if (*off > node->size && node->data) {
        memset(node->data + node->size, 0, (size_t)(*off - node->size));
    }

    memcpy(node->data + *off, buf, len);
    *off += len;
    if (*off > node->size) {
        node->size = *off;
        filp->f_inode->i_size = (s64)node->size;
        filp->f_inode->i_blocks = (node->size + 511) / 512;
    }
    return (s64)len;
}

s64 tmpfs_truncate(inode_t *inode, u64 length)
{
    if (!inode) return -22;
    tmpfs_node_t *node = (tmpfs_node_t *)inode->i_private;
    if (!node) return -22;
    if (S_ISDIR(node->mode)) return -21; /* EISDIR */
    if (length > TMPFS_MAX_FILE_SIZE) return -27; /* EFBIG */

    if (length > node->size) {
        if (tmpfs_grow(node, length) != 0) return -28;
        if (node->data) {
            memset(node->data + node->size, 0, (size_t)(length - node->size));
        }
    }
    node->size = length;
    inode->i_size = (s64)length;
    inode->i_blocks = (length + 511) / 512;
    return 0;
}

/* readdir: emit Linux-compatible dirent64 records for all children */
static s64 tmpfs_file_readdir(file_t *filp, void *dirent_buf, size_t len, u64 *off)
{
    if (!filp->f_inode) return -9;
    tmpfs_node_t *dir = (tmpfs_node_t *)filp->f_inode->i_private;
    if (!dir) return -9;

    /* Linux linux_dirent64 layout (without d_name[] field) */
    typedef struct {
        u64  d_ino;
        s64  d_off;
        u16  d_reclen;
        u8   d_type;
        char d_name[1];
    } linux_dirent64_t;

    u8   *dst      = (u8 *)dirent_buf;
    u64   written  = 0;
    u64   entry_idx = 0;
    u64   skip      = *off;

    /* Emit "." */
    if (entry_idx >= skip) {
        const char *nm = ".";
        u16 rec = (u16)((sizeof(linux_dirent64_t) - 1 + strlen(nm) + 1 + 7) & ~7u);
        if (written + rec > len) goto done;
        linux_dirent64_t *d = (linux_dirent64_t *)(dst + written);
        d->d_ino    = dir->ino;
        d->d_off    = (s64)(entry_idx + 1);
        d->d_reclen = rec;
        d->d_type   = 4; /* DT_DIR */
        memcpy(d->d_name, nm, strlen(nm) + 1);
        written += rec;
    }
    entry_idx++;

    /* Emit ".." */
    if (entry_idx >= skip) {
        const char *nm = "..";
        u16 rec = (u16)((sizeof(linux_dirent64_t) - 1 + strlen(nm) + 1 + 7) & ~7u);
        if (written + rec > len) goto done;
        linux_dirent64_t *d = (linux_dirent64_t *)(dst + written);
        d->d_ino    = dir->ino; /* simplified: parent ino not tracked */
        d->d_off    = (s64)(entry_idx + 1);
        d->d_reclen = rec;
        d->d_type   = 4;
        memcpy(d->d_name, nm, strlen(nm) + 1);
        written += rec;
    }
    entry_idx++;

    /* Emit children */
    tmpfs_node_t *c = dir->children;
    while (c) {
        if (entry_idx >= skip) {
            size_t nlen = strlen(c->name);
            u16    rec  = (u16)((sizeof(linux_dirent64_t) - 1 + nlen + 1 + 7) & ~7u);
            if (written + rec > len) goto done;
            linux_dirent64_t *d = (linux_dirent64_t *)(dst + written);
            d->d_ino    = c->ino;
            d->d_off    = (s64)(entry_idx + 1);
            d->d_reclen = rec;
            d->d_type   = S_ISDIR(c->mode)  ? 4 :
                           S_ISLNK(c->mode)  ? 10 : 8; /* DT_DIR/LNK/REG */
            memcpy(d->d_name, c->name, nlen + 1);
            written += rec;
        }
        entry_idx++;
        c = c->next;
    }

done:
    *off = entry_idx;
    return (s64)written;
}

/* ── mount ───────────────────────────────────────────────────────────────── */

static s64 tmpfs_mount(file_system_type_t *fs_type, const char *dev,
                        const char *dir, void *data)
{
    (void)dev; (void)data; (void)fs_type;

    /* Allocate superblock */
    super_block_t *sb = (super_block_t *)kmalloc(sizeof(super_block_t));
    if (!sb) return -12;
    memset(sb, 0, sizeof(*sb));
    sb->s_magic     = (u32)TMPFS_MAGIC;
    sb->s_blocksize = 4096;
    sb->s_type      = fs_type;
    sb->s_op        = &s_tmpfs_super_ops;

    /* Allocate private superblock data */
    tmpfs_sb_t *priv = (tmpfs_sb_t *)kmalloc(sizeof(tmpfs_sb_t));
    if (!priv) { kfree(sb); return -12; }
    memset(priv, 0, sizeof(*priv));
    sb->s_fs_info = priv;

    /* Create root node */
    tmpfs_node_t *root = tmpfs_new_node("/", 0040755 /* S_IFDIR | 0755 */);
    if (!root) { kfree(priv); kfree(sb); return -12; }
    root->ino = TMPFS_INO_ROOT;
    priv->root = root;

    /* Build root inode */
    inode_t *root_ino = tmpfs_make_inode(sb, root);
    if (!root_ino) { tmpfs_free_node(root); kfree(priv); kfree(sb); return -12; }

    dentry_t *mountpoint = NULL;
    s64 err = vfs_path_lookup(dir, &mountpoint);
    if (err == 0 && mountpoint && mountpoint->d_inode) {
        mountpoint->d_inode = root_ino;
        mountpoint->d_sb    = sb;
        sb->s_root          = mountpoint;
    } else {
        dentry_t *root_dentry = dcache_alloc(NULL, dir);
        if (!root_dentry) { kfree(root_ino); tmpfs_free_node(root); kfree(priv); kfree(sb); return -12; }
        root_dentry->d_inode = root_ino;
        root_dentry->d_sb    = sb;
        sb->s_root = root_dentry;
        dcache_add(root_dentry);
    }
    return 0;
}

/* ── public init ─────────────────────────────────────────────────────────── */

void tmpfs_init(void)
{
    vfs_register_fs(&s_tmpfs_type);
}
