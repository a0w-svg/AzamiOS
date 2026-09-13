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
#include "../arch/x86_64/cpu/spinlock.h"
#include "../userland/libc/include/sys/dirent.h"

#define MAX_DEVICES 96

typedef struct {
    char name[32];
    file_operations_t *fops;
    void *private_data;
    u32 mode; /* S_IFCHR or S_IFBLK */
    u64 rdev; /* MKDEV(major, minor), computed once at registration */
    inode_t *inode;
} devfs_node_t;

static spinlock_t g_devfs_lock = SPINLOCK_INIT;
static devfs_node_t g_devices[MAX_DEVICES];
static u32 g_device_count = 0;

/* ── Device numbers ───────────────────────────────────────────────────────
 *
 * st_rdev used to be hard-zeroed for every /dev node — nothing here ever
 * assigned one, so `ls -l /dev` showed "0, 0" for everything and any tool
 * that keys off a device number (mknod-based device managers, dedup-by-
 * device-and-inode logic, etc.) saw nothing useful. This assigns a real
 * one at registration time, matching the well-known Linux majors/minors
 * (Documentation/admin-guide/devices.txt) where a device has a standard
 * one, a family rule for the disk/tty families we mint several instances
 * of, and — for everything else — the LOCAL/EXPERIMENTAL major range
 * (240-254 in that same document, reserved for exactly this) with a
 * sequential minor, so even a device with no standard number gets one
 * that is real and distinct rather than a silent 0.
 */
#define DEVFS_MISC_MAJOR 240

static const struct { const char *name; u32 major, minor; } g_rdev_exact[] = {
    /* mem devices — major 1 */
    { "null",         1, 3   },
    { "zero",         1, 5   },
    { "full",         1, 7   },
    { "random",       1, 8   },
    { "urandom",      1, 9   },
    { "kmsg",         1, 11  },
    /* tty/console — major 4/5 */
    { "tty0",         4, 0   },
    { "tty1",         4, 1   },
    { "tty",          5, 0   },
    { "console",      5, 1   },
    { "ptmx",         5, 2   },
    { "ttyS0",        4, 64  },
    { "ttyS1",        4, 65  },
    /* parallel ports — major 6 */
    { "lp0",          6, 0   },
    { "lp1",          6, 1   },
    /* floppy — major 2 */
    { "fd0",          2, 0   },
    /* input — major 13 */
    { "psaux",        13, 1  },
    { "input0",       13, 65 },
    { "event0",       13, 64 },
    { "input/event0", 13, 64 },
    { "mice",         13, 63 },
    /* framebuffer — major 29 */
    { "fb0",          29, 0  },
    { "fb1",          29, 1  },
    /* sound (legacy OSS layout) — major 14 */
    { "midi",         14, 2  },
    { "midi0",        14, 2  },
    { "dsp",          14, 3  },
    { "audio",        14, 4  },
    { "dsp1",         14, 19 },
    { "dsp2",         14, 35 },
    { "audio2",       14, 36 },
    /* virtio console — major 229 */
    { "hvc0",         229, 0 },
    /* misc (major 10) devices with a standard minor */
    { "rtc",          10, 135 },
    { "watchdog",     10, 130 },
    { "hwrng",        10, 183 },
    { "loop-control", 10, 237 },
    /* DRM — major 226 */
    { "card0",        226, 0 },
};

/* True and sets *out_n if @s is entirely decimal digits (and not empty). */
static bool devfs_parse_uint(const char *s, u32 *out_n)
{
    if (!*s) return false;
    u32 n = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        n = n * 10 + (u32)(*p - '0');
    }
    *out_n = n;
    return true;
}

/* Disk-family rule: @prefix followed by one drive letter ('a'.. ), optionally
 * followed by "pN" for the Nth partition of that drive — the shape every
 * disk driver here (and drivers/block/partition.c) names its devices with.
 * @step is the per-drive minor spacing (room for that many partition minors
 * per drive), matching how Linux spaces its sd-family minors. Returns false
 * if @name doesn't have this shape. */
static bool devfs_letter_disk_rdev(const char *name, const char *prefix,
                                    u32 step, u32 *out_minor)
{
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return false;
    if (!name[plen] || name[plen] < 'a' || name[plen] > 'z') return false;
    u32 minor = (u32)(name[plen] - 'a') * step;
    const char *rest = name + plen + 1;
    if (*rest) {
        if (*rest != 'p') return false;
        u32 part;
        if (!devfs_parse_uint(rest + 1, &part) || part >= step) return false;
        minor += part;
    }
    *out_minor = minor;
    return true;
}

/* Numbered-instance rule: @prefix followed by a plain decimal index and
 * nothing else — "sr0", "loop3". */
static bool devfs_numbered_rdev(const char *name, const char *prefix, u32 *out_n)
{
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return false;
    return devfs_parse_uint(name + plen, out_n);
}

/* Numbered-disk rule: @prefix followed by a decimal drive index, optionally
 * followed by "pN" for the Nth partition of that drive — "sata0", "sata0p1",
 * "nvme1". Same shape as devfs_letter_disk_rdev() above, but for the drivers
 * (AHCI, NVMe) that number rather than letter their instances. */
static bool devfs_numbered_disk_rdev(const char *name, const char *prefix, u32 step,
                                      u32 *out_minor)
{
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return false;
    const char *p = name + plen;
    const char *psplit = strchr(p, 'p');
    size_t idxlen = psplit ? (size_t)(psplit - p) : strlen(p);
    if (idxlen == 0 || idxlen > 9) return false;
    char idxbuf[10];
    memcpy(idxbuf, p, idxlen);
    idxbuf[idxlen] = '\0';
    u32 idx;
    if (!devfs_parse_uint(idxbuf, &idx)) return false;
    u32 minor = idx * step;
    if (psplit) {
        u32 part;
        if (!devfs_parse_uint(psplit + 1, &part) || part >= step) return false;
        minor += part;
    }
    *out_minor = minor;
    return true;
}

static u64 devfs_assign_rdev(const char *name, u32 mode)
{
    for (u32 i = 0; i < ARRAY_SIZE(g_rdev_exact); i++) {
        if (strcmp(name, g_rdev_exact[i].name) == 0)
            return MKDEV(g_rdev_exact[i].major, g_rdev_exact[i].minor);
    }

    u32 n, minor;
    /* IDE — primary (hda/hdb) is major 3, secondary (hdc/hdd) is major 22. */
    if (devfs_letter_disk_rdev(name, "hd", 64, &minor)) {
        char letter = name[2];
        return MKDEV(letter <= 'b' ? 3 : 22, letter <= 'b' ? minor : minor - 128);
    }
    if (devfs_letter_disk_rdev(name, "vd", 16, &minor)) return MKDEV(253, minor);
    if (devfs_numbered_disk_rdev(name, "sata", 16, &minor)) return MKDEV(8, minor);
    if (devfs_numbered_disk_rdev(name, "nvme", 16, &minor)) return MKDEV(259, minor);
    if (devfs_numbered_rdev(name, "sr",   &n)) return MKDEV(11, n);
    if (devfs_numbered_rdev(name, "loop", &n)) return MKDEV(7, n);
    if (devfs_numbered_rdev(name, "dri/card", &n)) return MKDEV(226, n);
    if (strncmp(name, "dri/renderD", 11) == 0 && devfs_parse_uint(name + 11, &n))
        return MKDEV(226, n);

    /* No standard number for this one — hand out the next LOCAL/EXPERIMENTAL
     * one instead of leaving it at 0. Shared by every unmatched char and
     * block device, so two unrelated devices never collide. */
    static u32 s_next_misc_minor = 0;
    (void)mode;
    return MKDEV(DEVFS_MISC_MAJOR, s_next_misc_minor++);
}

/* Global function exposed to drivers */
int devfs_register_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (!name) return -1;
    
    spinlock_lock(&g_devfs_lock);
    if (g_device_count >= MAX_DEVICES) {
        spinlock_unlock(&g_devfs_lock);
        return -1;
    }
    
    /* Check if already registered */
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
            spinlock_unlock(&g_devfs_lock);
            return 0;
        }
    }

    devfs_node_t *node = &g_devices[g_device_count++];
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->fops = fops;
    node->private_data = private_data;
    node->mode = S_IFCHR;
    node->rdev = devfs_assign_rdev(node->name, node->mode);

    node->inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (node->inode) {
        node->inode->i_ino = 2 + (g_device_count - 1);
        node->inode->i_mode = S_IFCHR | 0666;
        node->inode->i_rdev = node->rdev;
        node->inode->i_fop = fops;
        node->inode->i_private = private_data;
    }
    spinlock_unlock(&g_devfs_lock);

    return 0;
}

int devfs_register_block_device(const char *name, file_operations_t *fops, void *private_data)
{
    if (!name) return -1;
    
    spinlock_lock(&g_devfs_lock);
    if (g_device_count >= MAX_DEVICES) {
        spinlock_unlock(&g_devfs_lock);
        return -1;
    }
    
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(g_devices[i].name, name) == 0) {
            g_devices[i].fops = fops;
            g_devices[i].private_data = private_data;
            if (g_devices[i].inode) {
                g_devices[i].inode->i_fop = fops;
                g_devices[i].inode->i_private = private_data;
            }
            spinlock_unlock(&g_devfs_lock);
            return 0;
        }
    }

    devfs_node_t *node = &g_devices[g_device_count++];
    strncpy(node->name, name, sizeof(node->name) - 1);
    node->fops = fops;
    node->private_data = private_data;
    node->mode = S_IFBLK;
    node->rdev = devfs_assign_rdev(node->name, node->mode);

    node->inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (node->inode) {
        node->inode->i_ino = 2 + (g_device_count - 1);
        node->inode->i_mode = S_IFBLK | 0666;
        node->inode->i_rdev = node->rdev;
        node->inode->i_fop = fops;
        node->inode->i_private = private_data;
    }
    spinlock_unlock(&g_devfs_lock);
    
    return 0;
}

/*
 * A device may register a name containing '/' — dri/card0, input/event0 —
 * which places it in a subdirectory of /dev.  The VFS resolves a path one
 * component at a time, so those directories have to exist as inodes: a
 * directory inode carries its prefix in i_private, and the root's is NULL,
 * meaning the empty prefix.
 */
#define MAX_DEVFS_DIRS 8

static inode_operations_t devfs_inode_ops;
static file_operations_t  devfs_dir_fops;

static struct {
    char     name[32];
    inode_t *inode;
} g_devfs_dirs[MAX_DEVFS_DIRS];
static u32 g_devfs_dir_count = 0;

/*
 * The prefix of a devfs directory inode, or "" for the root.  The mountpoint's
 * root inode is not one of ours and may carry an unrelated i_private, so a
 * pointer only counts as a prefix when it is one this file handed out.
 */
static const char *devfs_dir_prefix(struct inode *dir)
{
    if (!dir || !dir->i_private) return "";
    for (u32 i = 0; i < g_devfs_dir_count; i++) {
        if ((const char *)dir->i_private == g_devfs_dirs[i].name) {
            return g_devfs_dirs[i].name;
        }
    }
    return "";
}

/* Join a directory prefix and one path component into a registered name. */
static void devfs_join(char *out, size_t cap, const char *prefix, const char *name)
{
    if (prefix[0]) snprintf(out, cap, "%s/%s", prefix, name);
    else           snprintf(out, cap, "%s", name);
}

/* Directory inodes are cached so repeated lookups return the same inode. */
static inode_t *devfs_get_dir_inode(super_block_t *sb, const char *path)
{
    for (u32 i = 0; i < g_devfs_dir_count; i++) {
        if (strcmp(g_devfs_dirs[i].name, path) == 0) {
            g_devfs_dirs[i].inode->i_sb = sb;
            return g_devfs_dirs[i].inode;
        }
    }
    if (g_devfs_dir_count >= MAX_DEVFS_DIRS) return NULL;

    inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!inode) return NULL;

    u32 slot = g_devfs_dir_count++;
    strncpy(g_devfs_dirs[slot].name, path, sizeof(g_devfs_dirs[slot].name) - 1);

    inode->i_ino     = 0x1000 + slot;
    inode->i_mode    = S_IFDIR | 0555;
    inode->i_sb      = sb;
    inode->i_op      = &devfs_inode_ops;
    inode->i_fop     = &devfs_dir_fops;
    inode->i_private = g_devfs_dirs[slot].name;   /* the prefix for lookups */

    g_devfs_dirs[slot].inode = inode;
    return inode;
}

dentry_t *devfs_lookup(struct inode *dir, struct dentry *dentry)
{
    char full[48];
    devfs_join(full, sizeof(full), devfs_dir_prefix(dir), dentry->d_name);

    spinlock_lock(&g_devfs_lock);
    for (u32 i = 0; i < g_device_count; i++) {
        if (strcmp(full, g_devices[i].name) == 0) {
            if (!g_devices[i].inode) {
                g_devices[i].inode = (inode_t *)kzalloc(sizeof(inode_t));
                if (g_devices[i].inode) {
                    g_devices[i].inode->i_ino = 2 + i;
                    g_devices[i].inode->i_mode = g_devices[i].mode | 0666;
                    g_devices[i].inode->i_rdev = g_devices[i].rdev;
                    g_devices[i].inode->i_fop = g_devices[i].fops;
                    g_devices[i].inode->i_private = g_devices[i].private_data;
                }
            }
            if (g_devices[i].inode) {
                g_devices[i].inode->i_sb = dir->i_sb;
            }
            dentry->d_inode = g_devices[i].inode;
            spinlock_unlock(&g_devfs_lock);
            return dentry;
        }
    }

    /* Not a device — but it may be the directory some device lives in. */
    size_t flen = strlen(full);
    for (u32 i = 0; i < g_device_count; i++) {
        const char *n = g_devices[i].name;
        if (strncmp(n, full, flen) == 0 && n[flen] == '/') {
            dentry->d_inode = devfs_get_dir_inode(dir->i_sb, full);
            spinlock_unlock(&g_devfs_lock);
            return dentry;
        }
    }

    spinlock_unlock(&g_devfs_lock);
    return dentry; /* Negative dentry */
}

static s64 devfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!dirent_buf || len == 0) return -(s64)EINVAL;

    const char *prefix = devfs_dir_prefix(filp ? filp->f_inode : NULL);
    size_t plen = strlen(prefix);

    /*
     * Build this directory's entries: the devices directly inside it, plus
     * one entry for each subdirectory a nested device implies, listed once.
     */
    struct { const char *name; size_t len; u64 ino; u8 type; } ents[MAX_DEVICES + MAX_DEVFS_DIRS];
    size_t nents = 0;

    spinlock_lock(&g_devfs_lock);
    for (u32 i = 0; i < g_device_count && nents < ARRAY_SIZE(ents); i++) {
        const char *n = g_devices[i].name;

        if (plen) {
            if (strncmp(n, prefix, plen) != 0 || n[plen] != '/') continue;
            n += plen + 1;
        }

        const char *slash = strchr(n, '/');
        if (!slash) {
            ents[nents].name = n;
            ents[nents].len  = strlen(n);
            ents[nents].ino  = 2 + i;
            ents[nents].type = (g_devices[i].mode == S_IFBLK) ? DT_BLK : DT_CHR;
            nents++;
        } else {
            size_t seglen = (size_t)(slash - n);
            bool seen = false;
            for (size_t k = 0; k < nents; k++) {
                if (ents[k].type == DT_DIR && ents[k].len == seglen &&
                    strncmp(ents[k].name, n, seglen) == 0) { seen = true; break; }
            }
            if (seen) continue;
            ents[nents].name = n;
            ents[nents].len  = seglen;
            ents[nents].ino  = 0x2000 + i;
            ents[nents].type = DT_DIR;
            nents++;
        }
    }
    spinlock_unlock(&g_devfs_lock);

    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    while (idx < (u64)(nents + 2)) {
        const char *name = NULL;
        u64 ino = 1;
        u8 dtype = DT_CHR;
        size_t nlen;

        if (idx == 0) {
            name = "."; ino = 1; dtype = DT_DIR; nlen = 1;
        } else if (idx == 1) {
            name = ".."; ino = 1; dtype = DT_DIR; nlen = 2;
        } else {
            size_t e = (size_t)(idx - 2);
            name  = ents[e].name;
            nlen  = ents[e].len;
            ino   = ents[e].ino;
            dtype = ents[e].type;
        }

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

static s64 devfs_mkdir(inode_t *dir, dentry_t *dentry, u32 mode)
{
    (void)mode;
    char full[48];
    devfs_join(full, sizeof(full), devfs_dir_prefix(dir), dentry->d_name);
    inode_t *ino = devfs_get_dir_inode(dir->i_sb, full);
    if (!ino) return -(s64)ENOMEM;
    dentry->d_inode = ino;
    return 0;
}

static file_operations_t devfs_dir_fops = {
    .readdir = devfs_dir_readdir,
};

static inode_operations_t devfs_inode_ops = {
    .lookup = devfs_lookup,
    .mkdir  = devfs_mkdir,
};

static s64 devfs_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)dev_name;
    (void)data;
    
    dentry_t *mountpoint = NULL;
    s64 err = vfs_path_lookup(dir_name, &mountpoint);
    if (err < 0 || !mountpoint || !mountpoint->d_inode) {
        if (mountpoint && !mountpoint->d_inode) kfree(mountpoint);
        return -(s64)ENOENT;
    }

    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    inode_t *root_inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!sb || !root_inode) {
        if (sb) kfree(sb);
        if (root_inode) kfree(root_inode);
        return -(s64)ENOMEM;
    }
    
    sb->s_magic = 0xDE7F5;
    sb->s_type = fs_type;
    
    root_inode->i_ino = 1;
    root_inode->i_mode = S_IFDIR | 0755;
    root_inode->i_sb = sb;
    root_inode->i_op = &devfs_inode_ops;
    root_inode->i_fop = &devfs_dir_fops;
    
    mountpoint->d_inode = root_inode;
    mountpoint->d_sb = sb;
    sb->s_root = mountpoint;
    
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
