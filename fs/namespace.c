/* ============================================================================
 * AzamiOS — Mount Namespace (the mount table)
 * File: fs/namespace.c
 *
 * See fs/namespace.h for why this exists.  The short version: a filesystem's
 * ->mount() here grafts itself by overwriting the mountpoint dentry's
 * d_inode/d_sb in place.  That is a fine way to *attach* a filesystem and a
 * hopeless way to detach one, because after it runs nothing remembers what
 * the directory used to be.  So vfs_mount() takes that snapshot itself,
 * before the filesystem gets a chance to clobber it, and keeps it in a
 * vfsmount.  umount(2) is then just "put the snapshot back".
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "namespace.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "../include/azami/defs.h"
#include "../kernel/syscall/syscall.h"
#include "../kernel/sched/sched.h"

/* Implemented in fs/vfs.c; declared here rather than in vfs.h because the
 * dcache surgery a mount needs is not something any other caller should be
 * reaching for. */
void dcache_unhash_subtree(dentry_t *root);
void dcache_rehash_subtree(dentry_t *root);
void neg_dcache_invalidate(dentry_t *parent, const char *name);

static spinlock_t  g_mnt_lock = SPINLOCK_INIT;
static vfsmount_t *g_mounts = NULL;
static u32         g_next_mnt_id = 1;

void mnt_init(void)
{
    spinlock_lock(&g_mnt_lock);
    g_mounts = NULL;
    g_next_mnt_id = 1;
    spinlock_unlock(&g_mnt_lock);
}

vfsmount_t *mnt_list_head(void) { return g_mounts; }

/* ── Path helpers ─────────────────────────────────────────────────────────
 *
 * Every mountpoint is stored canonicalized and without a trailing slash
 * (except "/" itself), so exact and prefix comparisons are plain strcmp().
 */
static bool mnt_canon(const char *in, char *out, size_t out_len)
{
    if (!in || !out || out_len < 2) return false;
    if (vfs_resolve_path("/", in, out, out_len) != 0) return false;
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    return true;
}

/* True when @path is @mountpoint or lies underneath it. */
static bool path_under(const char *mountpoint, const char *path)
{
    size_t mlen = strlen(mountpoint);
    if (mlen == 1 && mountpoint[0] == '/') return true;     /* "/" covers all */
    if (strncmp(path, mountpoint, mlen) != 0) return false;
    return path[mlen] == '\0' || path[mlen] == '/';
}

/* The most deeply nested mount that contains @path — i.e. the mount @path
 * actually lives on.  Caller holds g_mnt_lock. */
static vfsmount_t *mnt_enclosing_locked(const char *path)
{
    vfsmount_t *best = NULL;
    size_t best_len = 0;
    for (vfsmount_t *m = g_mounts; m; m = m->next) {
        if (!path_under(m->mnt_path, path)) continue;
        size_t l = strlen(m->mnt_path);
        if (!best || l > best_len) { best = m; best_len = l; }
    }
    return best;
}

vfsmount_t *mnt_find_by_path(const char *path)
{
    char canon[MOUNT_PATH_MAX];
    if (!mnt_canon(path, canon, sizeof(canon))) return NULL;

    spinlock_lock(&g_mnt_lock);
    for (vfsmount_t *m = g_mounts; m; m = m->next) {
        if (strcmp(m->mnt_path, canon) == 0) {
            spinlock_unlock(&g_mnt_lock);
            return m;
        }
    }
    spinlock_unlock(&g_mnt_lock);
    return NULL;
}

vfsmount_t *mnt_find_for_sb(const super_block_t *sb)
{
    if (!sb) return NULL;
    spinlock_lock(&g_mnt_lock);
    for (vfsmount_t *m = g_mounts; m; m = m->next) {
        if (m->mnt_sb == sb) {
            spinlock_unlock(&g_mnt_lock);
            return m;
        }
    }
    spinlock_unlock(&g_mnt_lock);
    return NULL;
}

/* ── Option strings ──────────────────────────────────────────────────────── */

size_t mnt_format_opts(char *buf, size_t max, unsigned long flags)
{
    if (!buf || max == 0) return 0;
    size_t off = 0;
    off += (size_t)scnprintf(buf + off, max - off, "%s", (flags & MS_RDONLY) ? "ro" : "rw");
    if (flags & MS_NOSUID)       off += (size_t)scnprintf(buf + off, max - off, ",nosuid");
    if (flags & MS_NODEV)        off += (size_t)scnprintf(buf + off, max - off, ",nodev");
    if (flags & MS_NOEXEC)       off += (size_t)scnprintf(buf + off, max - off, ",noexec");
    if (flags & MS_SYNCHRONOUS)  off += (size_t)scnprintf(buf + off, max - off, ",sync");
    if (flags & MS_DIRSYNC)      off += (size_t)scnprintf(buf + off, max - off, ",dirsync");
    if (flags & MS_MANDLOCK)     off += (size_t)scnprintf(buf + off, max - off, ",mand");
    if (flags & MS_NOSYMFOLLOW)  off += (size_t)scnprintf(buf + off, max - off, ",nosymfollow");
    if (flags & MS_LAZYTIME)     off += (size_t)scnprintf(buf + off, max - off, ",lazytime");
    /* Exactly one of the three atime policies is reported, the way Linux
     * does it: noatime wins, then strictatime, then the relatime default. */
    if (flags & MS_NOATIME)          off += (size_t)scnprintf(buf + off, max - off, ",noatime");
    else if (flags & MS_STRICTATIME) off += (size_t)scnprintf(buf + off, max - off, ",strictatime");
    else                             off += (size_t)scnprintf(buf + off, max - off, ",relatime");
    if (flags & MS_NODIRATIME)   off += (size_t)scnprintf(buf + off, max - off, ",nodiratime");
    return off;
}

/* ── Busy detection ───────────────────────────────────────────────────────
 *
 * umount(2) must fail with EBUSY rather than yank a filesystem out from
 * under an open file or a process's working directory.  There is no
 * per-superblock open-file count here, so this asks the only authority that
 * knows: every process's fd table and cwd/root.
 */
static bool mnt_sb_busy(const super_block_t *sb, const char *mountpoint)
{
    bool busy = false;

    sched_lock();
    for (process_t *p = sched_get_process_list(); p && !busy; p = p->next) {
        if (p->is_zombie) continue;

        /* A working directory or chroot inside the mount pins it. */
        if (p->cwd[0] && path_under(mountpoint, p->cwd)) { busy = true; break; }
        if (p->root[0] && path_under(mountpoint, p->root)) { busy = true; break; }

        /* Under the process's own fd lock: a concurrent close(2) in another
         * thread frees the file_t, and reading f->f_inode after that is a
         * use-after-free. */
        irqflags_t ff = spinlock_lock_irqsave(&p->fd_lock);
        for (int fd = 0; fd < PROC_MAX_FDS; fd++) {
            file_t *f = (file_t *)p->handle_table[fd];
            /* handle_table also holds non-file_t handles (sockets, epoll,
             * object-manager handles).  They are all kernel-heap pointers of
             * the same shape, and every one that is a file_t has f_inode set
             * by the open path, so the sb comparison is what discriminates. */
            if (!f || (uintptr_t)f < 0xFFFF800000000000ULL) continue;
            if (f->f_inode && f->f_inode->i_sb == sb) { busy = true; break; }
        }
        spinlock_unlock_irqrestore(&p->fd_lock, ff);
    }
    sched_unlock();

    return busy;
}

/* ── Attaching ────────────────────────────────────────────────────────────── */

/* Record a completed graft.  @mp is the mountpoint dentry as it looked
 * *before* the filesystem overwrote it (caller saved the three fields). */
static vfsmount_t *mnt_record(const char *source, const char *canon_target,
                              const char *fstype, unsigned long flags,
                              const void *data, super_block_t *sb,
                              dentry_t *mp, inode_t *old_inode,
                              super_block_t *old_sb, dentry_t *old_subdirs,
                              bool is_bind)
{
    vfsmount_t *m = (vfsmount_t *)kzalloc(sizeof(vfsmount_t));
    if (!m) return NULL;

    snprintf(m->mnt_devname, sizeof(m->mnt_devname), "%s", source ? source : "none");
    snprintf(m->mnt_path,    sizeof(m->mnt_path),    "%s", canon_target);
    snprintf(m->mnt_fstype,  sizeof(m->mnt_fstype),  "%s", fstype ? fstype : "none");
    if (data && ((const char *)data)[0])
        snprintf(m->mnt_opts, sizeof(m->mnt_opts), "%s", (const char *)data);

    m->mnt_flags          = flags;
    m->mnt_sb             = sb;
    m->mnt_mountpoint     = mp;
    m->mnt_covered_inode  = old_inode;
    m->mnt_covered_sb     = old_sb;
    m->mnt_covered_subdirs = old_subdirs;
    m->mnt_is_bind        = is_bind;

    spinlock_lock(&g_mnt_lock);
    m->mnt_id = g_next_mnt_id++;
    vfsmount_t *parent = mnt_enclosing_locked(canon_target);
    m->mnt_parent_id = parent ? parent->mnt_id : m->mnt_id;
    m->next = g_mounts;
    g_mounts = m;
    spinlock_unlock(&g_mnt_lock);

    return m;
}

/* MS_REMOUNT: change the flags of an existing mount in place. */
static s64 do_remount(const char *canon_target, unsigned long flags, const void *data)
{
    spinlock_lock(&g_mnt_lock);
    vfsmount_t *m = NULL;
    for (vfsmount_t *c = g_mounts; c; c = c->next) {
        if (strcmp(c->mnt_path, canon_target) == 0) { m = c; break; }
    }
    if (!m) { spinlock_unlock(&g_mnt_lock); return -(s64)EINVAL; }

    /* MS_REMOUNT and MS_BIND together is the "change this bind mount's flags"
     * form.  This kernel keeps the enforced flags on the superblock, which a
     * bind shares with its source, so honouring it would silently change the
     * source mount too.  Refusing is the honest answer. */
    if ((flags & MS_BIND) && m->mnt_is_bind) {
        spinlock_unlock(&g_mnt_lock);
        return -(s64)EINVAL;
    }

    m->mnt_flags = flags & ~(MS_REMOUNT | MS_BIND | MS_MGC_MSK);
    if (data && ((const char *)data)[0])
        snprintf(m->mnt_opts, sizeof(m->mnt_opts), "%s", (const char *)data);
    if (m->mnt_sb) m->mnt_sb->s_flags = (u32)m->mnt_flags;
    spinlock_unlock(&g_mnt_lock);

    pr_debug("[MOUNT] remounted %s (flags 0x%lx)\n", canon_target, m->mnt_flags);
    return 0;
}

/* MS_MOVE: detach a mount from one mountpoint and attach it at another,
 * without asking the filesystem to do anything.  This is what pivot_root(8)
 * and initramfs scripts use to relocate /dev, /proc and /sys after switching
 * the real root into place. */
static s64 do_move(const char *canon_source, const char *canon_target)
{
    vfsmount_t *m = NULL;
    spinlock_lock(&g_mnt_lock);
    for (vfsmount_t *c = g_mounts; c; c = c->next) {
        if (strcmp(c->mnt_path, canon_source) == 0) { m = c; break; }
    }
    spinlock_unlock(&g_mnt_lock);
    if (!m) return -(s64)EINVAL;

    dentry_t *newmp = NULL;
    if (vfs_path_lookup(canon_target, &newmp) != 0 || !newmp || !newmp->d_inode)
        return -(s64)ENOENT;
    if (!S_ISDIR(newmp->d_inode->i_mode)) return -(s64)ENOTDIR;
    if (newmp == m->mnt_mountpoint) return -(s64)EINVAL;

    /* Put the old mountpoint back... */
    dentry_t *oldmp = m->mnt_mountpoint;
    inode_t  *mounted_inode = oldmp->d_inode;
    super_block_t *mounted_sb = oldmp->d_sb;
    dentry_t *mounted_subdirs = oldmp->d_subdirs;

    dcache_unhash_subtree(oldmp);
    oldmp->d_inode   = m->mnt_covered_inode;
    oldmp->d_sb      = m->mnt_covered_sb;
    oldmp->d_subdirs = m->mnt_covered_subdirs;
    dcache_rehash_subtree(oldmp);

    /* ...and graft the same filesystem onto the new one. */
    dcache_unhash_subtree(newmp);
    m->mnt_covered_inode   = newmp->d_inode;
    m->mnt_covered_sb      = newmp->d_sb;
    m->mnt_covered_subdirs = newmp->d_subdirs;

    newmp->d_inode   = mounted_inode;
    newmp->d_sb      = mounted_sb;
    newmp->d_subdirs = mounted_subdirs;
    /* Reparent the moved tree's top level. A dentry is hashed under its
     * d_parent, so without this the children would be re-hashed under the
     * *old* mountpoint and the moved files would still resolve at the path
     * they were moved away from. Only the direct children need it — their
     * own descendants already point at them. */
    for (dentry_t *c = mounted_subdirs; c; c = c->d_sibling) c->d_parent = newmp;
    dcache_rehash_subtree(newmp);

    if (mounted_sb) mounted_sb->s_root = newmp;

    spinlock_lock(&g_mnt_lock);
    m->mnt_mountpoint = newmp;
    snprintf(m->mnt_path, sizeof(m->mnt_path), "%s", canon_target);
    vfsmount_t *parent = mnt_enclosing_locked(canon_target);
    m->mnt_parent_id = (parent && parent != m) ? parent->mnt_id : m->mnt_id;
    spinlock_unlock(&g_mnt_lock);

    pr_debug("[MOUNT] moved %s -> %s\n", canon_source, canon_target);
    return 0;
}

/* MS_BIND: make the directory tree at @canon_source visible at
 * @canon_target as well.  Both names then reach the same inodes on the same
 * superblock — no second ->mount(), no second copy of the filesystem. */
static s64 do_bind(const char *canon_source, const char *canon_target,
                   unsigned long flags)
{
    dentry_t *src = NULL, *dst = NULL;
    if (vfs_path_lookup(canon_source, &src) != 0 || !src || !src->d_inode)
        return -(s64)ENOENT;
    if (vfs_path_lookup(canon_target, &dst) != 0 || !dst || !dst->d_inode)
        return -(s64)ENOENT;
    if (src == dst) return -(s64)EINVAL;
    /* Linux allows binding a single file onto a single file; a directory
     * source still requires a directory target. */
    if (S_ISDIR(src->d_inode->i_mode) && !S_ISDIR(dst->d_inode->i_mode))
        return -(s64)ENOTDIR;
    if (!S_ISDIR(src->d_inode->i_mode) && S_ISDIR(dst->d_inode->i_mode))
        return -(s64)EISDIR;
    /* Binding a directory onto something inside itself would make the tree
     * infinitely deep. */
    if (S_ISDIR(src->d_inode->i_mode) && path_under(canon_source, canon_target))
        return -(s64)EINVAL;

    inode_t       *old_inode   = dst->d_inode;
    super_block_t *old_sb      = dst->d_sb;
    dentry_t      *old_subdirs = dst->d_subdirs;

    dcache_unhash_subtree(dst);
    dst->d_inode   = src->d_inode;
    dst->d_sb      = src->d_sb;
    /* The bind target starts with an empty child list: lookups under it go
     * back to the filesystem, which walks the *source* inode and caches the
     * results under this dentry.  Sharing src->d_subdirs directly would put
     * one dentry on two parents' sibling chains. */
    dst->d_subdirs = NULL;
    dcache_rehash_subtree(dst);
    neg_dcache_invalidate(dst, NULL);

    /* MS_BIND on its own carries no flags of its own in Linux either — the
     * new mount sees exactly what the old one sees. */
    unsigned long eff = flags & ~(MS_BIND | MS_REC | MS_MGC_MSK);
    if (src->d_sb) eff = src->d_sb->s_flags;

    const char *fstype = "none";
    vfsmount_t *srcmnt = mnt_find_for_sb(src->d_sb);
    if (srcmnt) fstype = srcmnt->mnt_fstype;

    if (!mnt_record(canon_source, canon_target, fstype, eff, NULL, src->d_sb,
                    dst, old_inode, old_sb, old_subdirs, true)) {
        dcache_unhash_subtree(dst);
        dst->d_inode   = old_inode;
        dst->d_sb      = old_sb;
        dst->d_subdirs = old_subdirs;
        dcache_rehash_subtree(dst);
        return -(s64)ENOMEM;
    }

    pr_debug("[MOUNT] bound %s -> %s\n", canon_source, canon_target);
    return 0;
}

/* ── mount(2) ─────────────────────────────────────────────────────────────── */

s64 vfs_mount_flags(const char *source, const char *target, const char *fstype,
                    unsigned long flags, const void *data)
{
    if (!target || !target[0]) return -(s64)EINVAL;

    flags &= ~MS_MGC_MSK;   /* strip the legacy magic word, if present */

    char canon_target[MOUNT_PATH_MAX];
    if (!mnt_canon(target, canon_target, sizeof(canon_target)))
        return -(s64)ENAMETOOLONG;

    if (flags & MS_REMOUNT) return do_remount(canon_target, flags, data);

    if (flags & (MS_MOVE | MS_BIND)) {
        if (!source || !source[0]) return -(s64)EINVAL;
        char canon_source[MOUNT_PATH_MAX];
        if (!mnt_canon(source, canon_source, sizeof(canon_source)))
            return -(s64)ENAMETOOLONG;
        return (flags & MS_MOVE) ? do_move(canon_source, canon_target)
                                 : do_bind(canon_source, canon_target, flags);
    }

    if (!fstype || !fstype[0]) return -(s64)EINVAL;
    file_system_type_t *fs = vfs_find_fs(fstype);
    if (!fs) return -(s64)ENODEV;
    if (!fs->mount) return -(s64)EINVAL;

    /* Snapshot the mountpoint before the filesystem overwrites it.  At boot
     * the very first mount ("/" over the bootstrap rootfs) resolves fine;
     * a mountpoint that does not exist yet is left to the filesystem, which
     * creates a detached root dentry for it — there is nothing to restore in
     * that case and the fields stay NULL. */
    dentry_t *mp = NULL;
    inode_t  *old_inode = NULL;
    super_block_t *old_sb = NULL;
    dentry_t *old_subdirs = NULL;

    if (vfs_path_lookup(canon_target, &mp) == 0 && mp && mp->d_inode) {
        if (!S_ISDIR(mp->d_inode->i_mode)) return -(s64)ENOTDIR;
        old_inode   = mp->d_inode;
        old_sb      = mp->d_sb;
        old_subdirs = mp->d_subdirs;
        /* Anything cached under the directory belongs to the filesystem
         * about to be covered.  Unhash it so lookups miss and go to the new
         * filesystem instead, but keep the links so umount can put it back. */
        dcache_unhash_subtree(mp);
        mp->d_subdirs = NULL;
        neg_dcache_invalidate(mp, NULL);
    } else {
        mp = NULL;
    }

    s64 err = fs->mount(fs, source, canon_target, (void *)data);
    if (err < 0) {
        if (mp) {
            mp->d_inode   = old_inode;
            mp->d_sb      = old_sb;
            mp->d_subdirs = old_subdirs;
            dcache_rehash_subtree(mp);
        }
        return err;
    }

    /* Whatever the filesystem grafted, its superblock is reachable from the
     * mountpoint dentry it used.  Re-resolve rather than trusting the stale
     * pointer: a filesystem that could not find the mountpoint made its own
     * detached root dentry and that is the one now in the dcache. */
    dentry_t *now = NULL;
    if (vfs_path_lookup(canon_target, &now) != 0 || !now || !now->d_inode) {
        pr_debug("[MOUNT] %s mounted on %s but the path does not resolve\n",
                 fstype, canon_target);
        return -(s64)EIO;
    }

    super_block_t *sb = now->d_sb;
    if (sb) sb->s_flags = (u32)flags;

    if (!mnt_record(source, canon_target, fstype, flags, data, sb, now,
                    old_inode, old_sb, old_subdirs, false))
        return -(s64)ENOMEM;

    pr_debug("[MOUNT] %s on %s type %s (flags 0x%lx)\n",
             source ? source : "none", canon_target, fstype, flags);
    return 0;
}

s64 vfs_mount(const char *source, const char *target, const char *fstype, void *data)
{
    return vfs_mount_flags(source, target, fstype, 0, data);
}

/* ── umount(2) ────────────────────────────────────────────────────────────── */

s64 vfs_umount(const char *target, int flags)
{
    if (!target || !target[0]) return -(s64)EINVAL;

    char canon[MOUNT_PATH_MAX];
    if (!mnt_canon(target, canon, sizeof(canon))) return -(s64)ENAMETOOLONG;

    /* Find the mount, and while holding the lock make sure nothing is
     * mounted *on top of* it — unmounting the middle of a stack would
     * strand the mounts above it. */
    spinlock_lock(&g_mnt_lock);
    vfsmount_t *m = NULL;
    for (vfsmount_t *c = g_mounts; c; c = c->next) {
        if (strcmp(c->mnt_path, canon) == 0) { m = c; break; }
    }
    if (!m) { spinlock_unlock(&g_mnt_lock); return -(s64)EINVAL; }

    for (vfsmount_t *c = g_mounts; c; c = c->next) {
        if (c != m && strcmp(c->mnt_path, canon) != 0 && path_under(canon, c->mnt_path)) {
            spinlock_unlock(&g_mnt_lock);
            return -(s64)EBUSY;
        }
    }
    spinlock_unlock(&g_mnt_lock);

    /* MNT_DETACH is the "get it out of the namespace now, clean up when the
     * last user goes away" form, so it deliberately skips the busy check. */
    if (!(flags & MNT_DETACH) && mnt_sb_busy(m->mnt_sb, canon))
        return -(s64)EBUSY;

    /* Get this volume's data onto the media before it disappears.  MNT_FORCE
     * says "do not wait on a wedged device"; everything else syncs. */
    if (!(flags & MNT_FORCE)) {
        if (m->mnt_sb) vfs_sync_fs(m->mnt_sb);
        else vfs_sync_all();
    }

    dentry_t *mp = m->mnt_mountpoint;
    super_block_t *sb = m->mnt_sb;

    if (mp) {
        dcache_unhash_subtree(mp);
        if (m->mnt_covered_inode) {
            mp->d_inode   = m->mnt_covered_inode;
            mp->d_sb      = m->mnt_covered_sb;
            mp->d_subdirs = m->mnt_covered_subdirs;
            dcache_rehash_subtree(mp);
        } else {
            /* Nothing was underneath (the filesystem invented this dentry).
             * Leave a negative dentry rather than a dangling inode. */
            mp->d_inode   = NULL;
            mp->d_subdirs = NULL;
        }
        neg_dcache_invalidate(mp, NULL);
        if (mp->d_parent) neg_dcache_invalidate(mp->d_parent, mp->d_name);
    }

    /* Unlink by re-walking under the lock rather than by a pointer saved
     * earlier: another mount may have been prepended in between, and a
     * stale `&g_mounts` would drop it. */
    spinlock_lock(&g_mnt_lock);
    for (vfsmount_t **c = &g_mounts; *c; c = &(*c)->next) {
        if (*c == m) { *c = m->next; break; }
    }
    spinlock_unlock(&g_mnt_lock);

    /* Let the filesystem release its own state.  A bind shares its
     * superblock with the mount it was made from, so only the last
     * non-bind reference may tear it down. */
    if (sb && !m->mnt_is_bind && !mnt_find_for_sb(sb)) {
        if (sb->s_op && sb->s_op->put_super) sb->s_op->put_super(sb);
    }

    pr_debug("[MOUNT] unmounted %s\n", canon);
    kfree(m);
    return 0;
}

/* ── /proc formatting ─────────────────────────────────────────────────────── */

/* Mounts are kept newest-first for cheap lookup, but /proc/mounts is read in
 * mount order (that is what `df`, `mount` with no arguments and every
 * "find the mount for this path" loop expect), so both formatters walk the
 * table by ascending mnt_id instead of by list order. */
static vfsmount_t *mnt_next_by_id(u32 after_id)
{
    vfsmount_t *best = NULL;
    for (vfsmount_t *m = g_mounts; m; m = m->next) {
        if (m->mnt_id <= after_id) continue;
        if (!best || m->mnt_id < best->mnt_id) best = m;
    }
    return best;
}

size_t mnt_format_mounts(char *buf, size_t max)
{
    if (!buf || max == 0) return 0;
    size_t off = 0;
    char opts[128];

    spinlock_lock(&g_mnt_lock);
    u32 id = 0;
    for (vfsmount_t *m = mnt_next_by_id(id); m && off + 1 < max; m = mnt_next_by_id(id)) {
        id = m->mnt_id;
        mnt_format_opts(opts, sizeof(opts), m->mnt_flags);
        off += (size_t)scnprintf(buf + off, max - off, "%s %s %s %s 0 0\n",
                                 m->mnt_devname, m->mnt_path, m->mnt_fstype, opts);
    }
    spinlock_unlock(&g_mnt_lock);
    return off;
}

size_t mnt_format_mountinfo(char *buf, size_t max)
{
    if (!buf || max == 0) return 0;
    size_t off = 0;
    char opts[128];

    spinlock_lock(&g_mnt_lock);
    u32 id = 0;
    for (vfsmount_t *m = mnt_next_by_id(id); m && off + 1 < max; m = mnt_next_by_id(id)) {
        id = m->mnt_id;
        mnt_format_opts(opts, sizeof(opts), m->mnt_flags);
        u64 dev = m->mnt_sb ? m->mnt_sb->s_dev : 0;

        /* mountinfo(5): ID parentID major:minor root mountpoint options
         *               optional-fields - fstype source super-options
         * The "root" field is the subtree of the filesystem that is mounted;
         * only a bind can make it anything but "/", and a bind here always
         * grafts a whole directory, so the source path is the honest answer. */
        off += (size_t)scnprintf(buf + off, max - off,
                    "%u %u %u:%u %s %s %s - %s %s %s\n",
                    m->mnt_id, m->mnt_parent_id,
                    (unsigned)MAJOR(dev), (unsigned)MINOR(dev),
                    m->mnt_is_bind ? m->mnt_devname : "/",
                    m->mnt_path, opts,
                    m->mnt_fstype, m->mnt_devname,
                    (m->mnt_flags & MS_RDONLY) ? "ro" : "rw");
    }
    spinlock_unlock(&g_mnt_lock);
    return off;
}
