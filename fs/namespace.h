/* ============================================================================
 * AzamiOS — Mount Namespace (the mount table)
 * File: fs/namespace.h
 *
 * Until this existed, mounting was a one-way operation: a filesystem's
 * ->mount() overwrote the mountpoint dentry's d_inode/d_sb in place and
 * nothing recorded that it had happened.  umount(2) therefore could not
 * work (it synced and returned 0), /proc/mounts was a hard-coded string,
 * and the mount flags every Linux program passes — MS_RDONLY, MS_NOSUID,
 * MS_NODEV, MS_NOEXEC, MS_NOATIME — were dropped on the floor.
 *
 * This header is the record of what is mounted where.  vfs_mount() snapshots
 * the mountpoint before handing off to the filesystem and pushes a vfsmount
 * describing the result; vfs_umount() pops it and puts the covered inode
 * back.  Everything else (/proc/mounts, /proc/self/mountinfo, the MS_* flag
 * checks in fs/vfs.c) reads out of that table.
 * ============================================================================ */
#pragma once

#include "vfs.h"

/* ── Linux mount flags (sys/mount.h) ──────────────────────────────────────
 * The subset with observable behaviour here is enforced; the rest are
 * recorded and reported through /proc/mounts so a program that sets one and
 * reads it back sees what it asked for. */
#define MS_RDONLY       (1UL << 0)
#define MS_NOSUID       (1UL << 1)
#define MS_NODEV        (1UL << 2)
#define MS_NOEXEC       (1UL << 3)
#define MS_SYNCHRONOUS  (1UL << 4)
#define MS_REMOUNT      (1UL << 5)
#define MS_MANDLOCK     (1UL << 6)
#define MS_DIRSYNC      (1UL << 7)
#define MS_NOSYMFOLLOW  (1UL << 8)
#define MS_NOATIME      (1UL << 10)
#define MS_NODIRATIME   (1UL << 11)
#define MS_BIND         (1UL << 12)
#define MS_MOVE         (1UL << 13)
#define MS_REC          (1UL << 14)
#define MS_SILENT       (1UL << 15)
#define MS_POSIXACL     (1UL << 16)
#define MS_UNBINDABLE   (1UL << 17)
#define MS_PRIVATE      (1UL << 18)
#define MS_SLAVE        (1UL << 19)
#define MS_SHARED       (1UL << 20)
#define MS_RELATIME     (1UL << 21)
#define MS_KERNMOUNT    (1UL << 22)
#define MS_I_VERSION    (1UL << 23)
#define MS_STRICTATIME  (1UL << 24)
#define MS_LAZYTIME     (1UL << 25)

/* MS_MGC_VAL: the magic word pre-2.4 mount(2) required in the top 16 bits of
 * the flags word.  Stock util-linux still ORs it in on some paths, so it has
 * to be masked off before the flags are interpreted or every flag bit reads
 * as set. */
#define MS_MGC_VAL      0xC0ED0000UL
#define MS_MGC_MSK      0xFFFF0000UL

/* umount2(2) flags */
#define MNT_FORCE       0x00000001
#define MNT_DETACH      0x00000002
#define MNT_EXPIRE      0x00000004
#define UMOUNT_NOFOLLOW 0x00000008

#define MOUNT_PATH_MAX  256
#define MOUNT_DEV_MAX   64
#define MOUNT_TYPE_MAX  32

typedef struct vfsmount {
    u32   mnt_id;                        /* unique, matches mountinfo's field 1 */
    u32   mnt_parent_id;                 /* enclosing mount's id (self for "/") */
    char  mnt_devname[MOUNT_DEV_MAX];    /* source, as passed to mount(2) */
    char  mnt_path[MOUNT_PATH_MAX];      /* canonical absolute mountpoint */
    char  mnt_fstype[MOUNT_TYPE_MAX];    /* "ext2", "procfs", ... */
    char  mnt_opts[MOUNT_PATH_MAX];      /* the fs-specific data string, verbatim */
    unsigned long mnt_flags;             /* MS_* in force for this mount */

    super_block_t *mnt_sb;               /* what got mounted */
    dentry_t      *mnt_mountpoint;       /* the dentry it was mounted on */

    /* What the mountpoint dentry looked like before — everything umount
     * needs to put the directory back the way it was. */
    inode_t       *mnt_covered_inode;
    super_block_t *mnt_covered_sb;
    dentry_t      *mnt_covered_subdirs;

    bool  mnt_is_bind;                   /* shares mnt_sb with another mount */
    struct vfsmount *next;               /* most recently mounted first */
} vfsmount_t;

/** mnt_init() — reset the mount table.  Called from vfs_init(). */
void mnt_init(void);

/** vfs_mount_flags() — mount(2) proper: honours MS_BIND / MS_MOVE /
 *  MS_REMOUNT and records the result in the mount table.  vfs_mount() is
 *  this with flags = 0 and is what the boot path keeps calling. */
s64 vfs_mount_flags(const char *source, const char *target, const char *fstype,
                    unsigned long flags, const void *data);

/** mnt_find_by_path() — the mount whose mountpoint is exactly @path. */
vfsmount_t *mnt_find_by_path(const char *path);

/** mnt_find_for_sb() — the (first, i.e. most recent) mount of @sb. */
vfsmount_t *mnt_find_for_sb(const super_block_t *sb);

/** mnt_list_head() — head of the mount table, newest first.  Read-only: the
 *  caller must not modify the list.  Only /proc formatting uses it. */
vfsmount_t *mnt_list_head(void);

/** mnt_format_mounts()   — /proc/mounts and /proc/<pid>/mounts (fstab form).
 *  mnt_format_mountinfo() — /proc/<pid>/mountinfo (the extended form
 *  util-linux, systemd and container tooling actually parse).
 *  Both return the number of bytes written (never more than max-1). */
size_t mnt_format_mounts(char *buf, size_t max);
size_t mnt_format_mountinfo(char *buf, size_t max);

/** mnt_format_opts() — render the MS_* bits of @flags as the comma-separated
 *  option string /proc/mounts uses ("rw,nosuid,nodev,noexec,relatime"). */
size_t mnt_format_opts(char *buf, size_t max, unsigned long flags);
