/* ============================================================================
 * AzamiOS — System Virtual Filesystem (sysfs) Implementation
 * File: fs/sysfs.c
 *
 * Implements /sys virtual filesystem representing device hierarchy, classes,
 * power management, and CPU subsystem topology.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "sysfs.h"
#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/sched/sched.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../include/azami/net.h"
#include "../userland/libc/include/sys/dirent.h"

#define SYSFS_SUPER_MAGIC 0x62656572

typedef enum {
    SYSFS_TYPE_ROOT_DIR,
    SYSFS_TYPE_CLASS_DIR,
    SYSFS_TYPE_DEVICES_DIR,
    SYSFS_TYPE_KERNEL_DIR,
    SYSFS_TYPE_POWER_DIR,
    SYSFS_TYPE_BUS_DIR,
    SYSFS_TYPE_FS_DIR,

    /* Class subdirectories */
    SYSFS_TYPE_CLASS_NET_DIR,
    SYSFS_TYPE_CLASS_BLOCK_DIR,
    SYSFS_TYPE_CLASS_SOUND_DIR,
    SYSFS_TYPE_CLASS_DRM_DIR,

    /* Net devices */
    SYSFS_TYPE_NET_DEV_DIR,
    SYSFS_TYPE_NET_STATS_DIR,
    SYSFS_TYPE_NET_ATTR_ADDR,
    SYSFS_TYPE_NET_ATTR_OPERSTATE,
    SYSFS_TYPE_NET_ATTR_MTU,
    SYSFS_TYPE_NET_ATTR_SPEED,
    SYSFS_TYPE_NET_ATTR_DUPLEX,
    SYSFS_TYPE_NET_ATTR_TYPE,
    SYSFS_TYPE_NET_STAT_RX_BYTES,
    SYSFS_TYPE_NET_STAT_TX_BYTES,
    SYSFS_TYPE_NET_STAT_RX_PACKETS,
    SYSFS_TYPE_NET_STAT_TX_PACKETS,

    /* Block devices */
    SYSFS_TYPE_BLOCK_DEV_DIR,
    SYSFS_TYPE_BLOCK_ATTR_DEV,
    SYSFS_TYPE_BLOCK_ATTR_SIZE,
    SYSFS_TYPE_BLOCK_ATTR_REMOVABLE,
    SYSFS_TYPE_BLOCK_ATTR_STAT,

    /* Sound & DRM */
    SYSFS_TYPE_SOUND_CARD_DIR,
    SYSFS_TYPE_SOUND_ATTR_ID,
    SYSFS_TYPE_DRM_CARD_DIR,
    SYSFS_TYPE_DRM_ATTR_STATUS,
    SYSFS_TYPE_DRM_ATTR_ENABLED,

    /* CPU topology */
    SYSFS_TYPE_SYSTEM_DIR,
    SYSFS_TYPE_CPU_DIR,
    SYSFS_TYPE_CPU_CORE_DIR,
    SYSFS_TYPE_CPU_ATTR_ONLINE,
    SYSFS_TYPE_CPU_ATTR_PRESENT,
    SYSFS_TYPE_CPU_ATTR_POSSIBLE,
    SYSFS_TYPE_CPU_CORE_ONLINE,

    /* Power & Kernel */
    SYSFS_TYPE_POWER_ATTR_STATE,
    SYSFS_TYPE_KERNEL_ATTR_UEVENT,
    SYSFS_TYPE_KERNEL_ATTR_PROFILING,
} sysfs_node_type_t;

typedef struct {
    sysfs_node_type_t type;
    char name[32];
    u32 index;
} sysfs_priv_t;

/* Forward declarations */
static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry);
static s64 sysfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 sysfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);

static inode_operations_t g_sysfs_inode_ops = {
    .lookup = sysfs_lookup,
};

static file_operations_t g_sysfs_file_ops = {
    .read = sysfs_file_read,
    .readdir = sysfs_dir_readdir,
};

static inode_t *sysfs_alloc_inode(super_block_t *sb, u64 ino, u32 mode, sysfs_node_type_t type, const char *name, u32 index)
{
    inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!inode) return NULL;

    inode->i_ino = ino;
    inode->i_mode = mode;
    inode->i_sb = sb;
    inode->i_op = &g_sysfs_inode_ops;
    inode->i_fop = &g_sysfs_file_ops;

    sysfs_priv_t *priv = (sysfs_priv_t *)kzalloc(sizeof(sysfs_priv_t));
    if (priv) {
        priv->type = type;
        priv->index = index;
        if (name) strncpy(priv->name, name, sizeof(priv->name) - 1);
        inode->i_private = priv;
    }

    return inode;
}

/* --------------------------------------------------------------------------
 * Sysfs File Reading
 * -------------------------------------------------------------------------- */

static s64 sysfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (!filp || !buf || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    sysfs_priv_t *priv = (sysfs_priv_t *)filp->f_inode->i_private;
    char tmp[256];
    size_t total_len = 0;

    switch (priv->type) {
    case SYSFS_TYPE_NET_ATTR_ADDR: {
        net_device_t *ndev = net_get_default_device();
        if (ndev && strcmp(priv->name, "lo") != 0) {
            u8 *m = ndev->mac;
            total_len = (size_t)snprintf(tmp, sizeof(tmp), "%02x:%02x:%02x:%02x:%02x:%02x\n",
                                         m[0], m[1], m[2], m[3], m[4], m[5]);
        } else {
            total_len = (size_t)snprintf(tmp, sizeof(tmp), "00:00:00:00:00:00\n");
        }
        break;
    }
    case SYSFS_TYPE_NET_ATTR_OPERSTATE:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "up\n");
        break;
    case SYSFS_TYPE_NET_ATTR_MTU:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "%u\n", (strcmp(priv->name, "lo") == 0) ? 65536 : 1500);
        break;
    case SYSFS_TYPE_NET_ATTR_SPEED:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "1000\n");
        break;
    case SYSFS_TYPE_NET_ATTR_DUPLEX:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "full\n");
        break;
    case SYSFS_TYPE_NET_ATTR_TYPE:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "%u\n", (strcmp(priv->name, "lo") == 0) ? 772 : 1);
        break;
    case SYSFS_TYPE_NET_STAT_RX_BYTES:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "14200\n");
        break;
    case SYSFS_TYPE_NET_STAT_TX_BYTES:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "8400\n");
        break;
    case SYSFS_TYPE_NET_STAT_RX_PACKETS:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "128\n");
        break;
    case SYSFS_TYPE_NET_STAT_TX_PACKETS:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "64\n");
        break;
    case SYSFS_TYPE_BLOCK_ATTR_DEV:
        if (strcmp(priv->name, "sda") == 0) total_len = (size_t)snprintf(tmp, sizeof(tmp), "8:0\n");
        else if (strcmp(priv->name, "sda1") == 0) total_len = (size_t)snprintf(tmp, sizeof(tmp), "8:1\n");
        else if (strcmp(priv->name, "loop0") == 0) total_len = (size_t)snprintf(tmp, sizeof(tmp), "7:0\n");
        else total_len = (size_t)snprintf(tmp, sizeof(tmp), "1:0\n");
        break;
    case SYSFS_TYPE_BLOCK_ATTR_SIZE:
        if (strcmp(priv->name, "sda") == 0) total_len = (size_t)snprintf(tmp, sizeof(tmp), "4194304\n");
        else if (strcmp(priv->name, "sda1") == 0) total_len = (size_t)snprintf(tmp, sizeof(tmp), "4192256\n");
        else total_len = (size_t)snprintf(tmp, sizeof(tmp), "409600\n");
        break;
    case SYSFS_TYPE_BLOCK_ATTR_REMOVABLE:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "0\n");
        break;
    case SYSFS_TYPE_BLOCK_ATTR_STAT:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "    2400     120    38400     1200     1800      90    28800      900        0      450     2100\n");
        break;
    case SYSFS_TYPE_SOUND_ATTR_ID:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "AzamiAudio0\n");
        break;
    case SYSFS_TYPE_DRM_ATTR_STATUS:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "connected\n");
        break;
    case SYSFS_TYPE_DRM_ATTR_ENABLED:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "enabled\n");
        break;
    case SYSFS_TYPE_CPU_ATTR_ONLINE:
    case SYSFS_TYPE_CPU_ATTR_PRESENT:
    case SYSFS_TYPE_CPU_ATTR_POSSIBLE: {
        u32 cpus = smp_cpu_count();
        if (cpus == 0) cpus = 1;
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "0-%u\n", cpus - 1);
        break;
    }
    case SYSFS_TYPE_CPU_CORE_ONLINE:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "1\n");
        break;
    case SYSFS_TYPE_POWER_ATTR_STATE:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "freeze mem disk\n");
        break;
    case SYSFS_TYPE_KERNEL_ATTR_UEVENT:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "1024\n");
        break;
    case SYSFS_TYPE_KERNEL_ATTR_PROFILING:
        total_len = (size_t)snprintf(tmp, sizeof(tmp), "0\n");
        break;
    default:
        return 0;
    }

    if (*offset >= total_len) return 0;
    size_t avail = total_len - (size_t)*offset;
    size_t copy_cnt = (len < avail) ? len : avail;
    memcpy(buf, tmp + *offset, copy_cnt);
    *offset += copy_cnt;
    return (s64)copy_cnt;
}

/* --------------------------------------------------------------------------
 * Sysfs Dentry Lookup
 * -------------------------------------------------------------------------- */

static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry)
{
    if (!dir || !dentry || !dir->i_private) return dentry;
    sysfs_priv_t *priv = (sysfs_priv_t *)dir->i_private;
    const char *name = dentry->d_name;

    if (priv->type == SYSFS_TYPE_ROOT_DIR) {
        if (strcmp(name, "class") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 10, S_IFDIR | 0555, SYSFS_TYPE_CLASS_DIR, NULL, 0);
        } else if (strcmp(name, "devices") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 11, S_IFDIR | 0555, SYSFS_TYPE_DEVICES_DIR, NULL, 0);
        } else if (strcmp(name, "kernel") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 12, S_IFDIR | 0555, SYSFS_TYPE_KERNEL_DIR, NULL, 0);
        } else if (strcmp(name, "power") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 13, S_IFDIR | 0555, SYSFS_TYPE_POWER_DIR, NULL, 0);
        } else if (strcmp(name, "bus") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 14, S_IFDIR | 0555, SYSFS_TYPE_BUS_DIR, NULL, 0);
        } else if (strcmp(name, "fs") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 15, S_IFDIR | 0555, SYSFS_TYPE_FS_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_DIR) {
        if (strcmp(name, "net") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 20, S_IFDIR | 0555, SYSFS_TYPE_CLASS_NET_DIR, NULL, 0);
        } else if (strcmp(name, "block") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 21, S_IFDIR | 0555, SYSFS_TYPE_CLASS_BLOCK_DIR, NULL, 0);
        } else if (strcmp(name, "sound") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 22, S_IFDIR | 0555, SYSFS_TYPE_CLASS_SOUND_DIR, NULL, 0);
        } else if (strcmp(name, "drm") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 23, S_IFDIR | 0555, SYSFS_TYPE_CLASS_DRM_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_NET_DIR) {
        if (strcmp(name, "eth0") == 0 || strcmp(name, "net0") == 0 || strcmp(name, "lo") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 100, S_IFDIR | 0555, SYSFS_TYPE_NET_DEV_DIR, name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_NET_DEV_DIR) {
        if (strcmp(name, "address") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 101, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_ADDR, priv->name, 0);
        } else if (strcmp(name, "operstate") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 102, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_OPERSTATE, priv->name, 0);
        } else if (strcmp(name, "mtu") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 103, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_MTU, priv->name, 0);
        } else if (strcmp(name, "speed") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 104, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_SPEED, priv->name, 0);
        } else if (strcmp(name, "duplex") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 105, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_DUPLEX, priv->name, 0);
        } else if (strcmp(name, "type") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 106, S_IFREG | 0444, SYSFS_TYPE_NET_ATTR_TYPE, priv->name, 0);
        } else if (strcmp(name, "statistics") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 107, S_IFDIR | 0555, SYSFS_TYPE_NET_STATS_DIR, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_NET_STATS_DIR) {
        if (strcmp(name, "rx_bytes") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 110, S_IFREG | 0444, SYSFS_TYPE_NET_STAT_RX_BYTES, priv->name, 0);
        } else if (strcmp(name, "tx_bytes") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 111, S_IFREG | 0444, SYSFS_TYPE_NET_STAT_TX_BYTES, priv->name, 0);
        } else if (strcmp(name, "rx_packets") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 112, S_IFREG | 0444, SYSFS_TYPE_NET_STAT_RX_PACKETS, priv->name, 0);
        } else if (strcmp(name, "tx_packets") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 113, S_IFREG | 0444, SYSFS_TYPE_NET_STAT_TX_PACKETS, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_BLOCK_DIR) {
        if (strcmp(name, "sda") == 0 || strcmp(name, "sda1") == 0 || strcmp(name, "loop0") == 0 || strcmp(name, "ram0") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 200, S_IFDIR | 0555, SYSFS_TYPE_BLOCK_DEV_DIR, name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_BLOCK_DEV_DIR) {
        if (strcmp(name, "dev") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 201, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_DEV, priv->name, 0);
        } else if (strcmp(name, "size") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 202, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_SIZE, priv->name, 0);
        } else if (strcmp(name, "removable") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 203, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_REMOVABLE, priv->name, 0);
        } else if (strcmp(name, "stat") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 204, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_STAT, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_SOUND_DIR) {
        if (strcmp(name, "card0") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 300, S_IFDIR | 0555, SYSFS_TYPE_SOUND_CARD_DIR, name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_SOUND_CARD_DIR) {
        if (strcmp(name, "id") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 301, S_IFREG | 0444, SYSFS_TYPE_SOUND_ATTR_ID, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_DRM_DIR) {
        if (strcmp(name, "card0") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 400, S_IFDIR | 0555, SYSFS_TYPE_DRM_CARD_DIR, name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_DRM_CARD_DIR) {
        if (strcmp(name, "status") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 401, S_IFREG | 0444, SYSFS_TYPE_DRM_ATTR_STATUS, priv->name, 0);
        } else if (strcmp(name, "enabled") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 402, S_IFREG | 0444, SYSFS_TYPE_DRM_ATTR_ENABLED, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_DEVICES_DIR) {
        if (strcmp(name, "system") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 500, S_IFDIR | 0555, SYSFS_TYPE_SYSTEM_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_SYSTEM_DIR) {
        if (strcmp(name, "cpu") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 510, S_IFDIR | 0555, SYSFS_TYPE_CPU_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CPU_DIR) {
        if (strcmp(name, "online") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 520, S_IFREG | 0444, SYSFS_TYPE_CPU_ATTR_ONLINE, NULL, 0);
        } else if (strcmp(name, "present") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 521, S_IFREG | 0444, SYSFS_TYPE_CPU_ATTR_PRESENT, NULL, 0);
        } else if (strcmp(name, "possible") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 522, S_IFREG | 0444, SYSFS_TYPE_CPU_ATTR_POSSIBLE, NULL, 0);
        } else if (strncmp(name, "cpu", 3) == 0 && name[3] >= '0' && name[3] <= '9') {
            u32 cpuid = 0;
            const char *p = name + 3;
            while (*p >= '0' && *p <= '9') { cpuid = cpuid * 10 + (u32)(*p - '0'); p++; }
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 530 + cpuid, S_IFDIR | 0555, SYSFS_TYPE_CPU_CORE_DIR, name, cpuid);
        }
    } else if (priv->type == SYSFS_TYPE_CPU_CORE_DIR) {
        if (strcmp(name, "online") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 540 + priv->index, S_IFREG | 0444, SYSFS_TYPE_CPU_CORE_ONLINE, priv->name, priv->index);
        }
    } else if (priv->type == SYSFS_TYPE_POWER_DIR) {
        if (strcmp(name, "state") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 600, S_IFREG | 0644, SYSFS_TYPE_POWER_ATTR_STATE, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_KERNEL_DIR) {
        if (strcmp(name, "uevent_seqnum") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 700, S_IFREG | 0444, SYSFS_TYPE_KERNEL_ATTR_UEVENT, NULL, 0);
        } else if (strcmp(name, "profiling") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 701, S_IFREG | 0644, SYSFS_TYPE_KERNEL_ATTR_PROFILING, NULL, 0);
        }
    }

    return dentry;
}

/* --------------------------------------------------------------------------
 * Sysfs Directory Readdir
 * -------------------------------------------------------------------------- */

static s64 sysfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!filp || !dirent_buf || len == 0 || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    sysfs_priv_t *priv = (sysfs_priv_t *)filp->f_inode->i_private;
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    const char *entries[16];
    u8 types[16];
    u64 total_entries = 2;
    entries[0] = "."; types[0] = DT_DIR;
    entries[1] = ".."; types[1] = DT_DIR;

    if (priv->type == SYSFS_TYPE_ROOT_DIR) {
        const char *r[] = { "class", "devices", "kernel", "power", "bus", "fs" };
        for (int i = 0; i < 6; i++) { entries[total_entries] = r[i]; types[total_entries++] = DT_DIR; }
    } else if (priv->type == SYSFS_TYPE_CLASS_DIR) {
        const char *c[] = { "net", "block", "sound", "drm" };
        for (int i = 0; i < 4; i++) { entries[total_entries] = c[i]; types[total_entries++] = DT_DIR; }
    } else if (priv->type == SYSFS_TYPE_CLASS_NET_DIR) {
        net_device_t *ndev = net_get_default_device();
        entries[total_entries] = ndev ? ndev->name : "eth0"; types[total_entries++] = DT_DIR;
        entries[total_entries] = "lo"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_NET_DEV_DIR) {
        const char *na[] = { "address", "operstate", "mtu", "speed", "duplex", "type" };
        for (int i = 0; i < 6; i++) { entries[total_entries] = na[i]; types[total_entries++] = DT_REG; }
        entries[total_entries] = "statistics"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_NET_STATS_DIR) {
        const char *ns[] = { "rx_bytes", "tx_bytes", "rx_packets", "tx_packets" };
        for (int i = 0; i < 4; i++) { entries[total_entries] = ns[i]; types[total_entries++] = DT_REG; }
    } else if (priv->type == SYSFS_TYPE_CLASS_BLOCK_DIR) {
        const char *b[] = { "sda", "sda1", "loop0", "ram0" };
        for (int i = 0; i < 4; i++) { entries[total_entries] = b[i]; types[total_entries++] = DT_DIR; }
    } else if (priv->type == SYSFS_TYPE_BLOCK_DEV_DIR) {
        const char *ba[] = { "dev", "size", "removable", "stat" };
        for (int i = 0; i < 4; i++) { entries[total_entries] = ba[i]; types[total_entries++] = DT_REG; }
    } else if (priv->type == SYSFS_TYPE_CLASS_SOUND_DIR) {
        entries[total_entries] = "card0"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_SOUND_CARD_DIR) {
        entries[total_entries] = "id"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_CLASS_DRM_DIR) {
        entries[total_entries] = "card0"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_DRM_CARD_DIR) {
        entries[total_entries] = "status"; types[total_entries++] = DT_REG;
        entries[total_entries] = "enabled"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_DEVICES_DIR) {
        entries[total_entries] = "system"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_SYSTEM_DIR) {
        entries[total_entries] = "cpu"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_CPU_DIR) {
        entries[total_entries] = "online"; types[total_entries++] = DT_REG;
        entries[total_entries] = "present"; types[total_entries++] = DT_REG;
        entries[total_entries] = "possible"; types[total_entries++] = DT_REG;
        entries[total_entries] = "cpu0"; types[total_entries++] = DT_DIR;
        entries[total_entries] = "cpu1"; types[total_entries++] = DT_DIR;
        entries[total_entries] = "cpu2"; types[total_entries++] = DT_DIR;
        entries[total_entries] = "cpu3"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_CPU_CORE_DIR) {
        entries[total_entries] = "online"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_POWER_DIR) {
        entries[total_entries] = "state"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_KERNEL_DIR) {
        entries[total_entries] = "uevent_seqnum"; types[total_entries++] = DT_REG;
        entries[total_entries] = "profiling"; types[total_entries++] = DT_REG;
    }

    while (idx < total_entries) {
        const char *name = entries[idx];
        u8 dtype = types[idx];

        size_t nlen = strlen(name);
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + nlen + 1, 8);
        if (written + reclen > len) {
            if (written == 0) return -(s64)EINVAL;
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out_ptr + written);
        d->d_ino = idx + 1;
        d->d_off = idx + 1;
        d->d_reclen = (unsigned short)reclen;
        d->d_type = dtype;
        memcpy(d->d_name, name, nlen + 1);

        written += reclen;
        idx++;
    }

    *offset = idx;
    return (s64)written;
}

/* --------------------------------------------------------------------------
 * Mount & Registration
 * -------------------------------------------------------------------------- */

static s64 sysfs_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
{
    (void)fs_type; (void)dev_name; (void)dir_name; (void)data;

    dentry_t *mountpoint = NULL;
    s64 err = vfs_path_lookup(dir_name, &mountpoint);
    if (err < 0 || !mountpoint || !mountpoint->d_inode) {
        if (mountpoint && !mountpoint->d_inode) kfree(mountpoint);
        return -(s64)ENOENT;
    }

    super_block_t *sb = (super_block_t *)kzalloc(sizeof(super_block_t));
    if (!sb) return -(s64)ENOMEM;

    sb->s_magic = SYSFS_SUPER_MAGIC;
    sb->s_blocksize = 4096;

    inode_t *root_inode = sysfs_alloc_inode(sb, 1, S_IFDIR | 0555, SYSFS_TYPE_ROOT_DIR, NULL, 0);
    if (!root_inode) {
        kfree(sb);
        return -(s64)ENOMEM;
    }

    mountpoint->d_inode = root_inode;
    mountpoint->d_sb = sb;
    sb->s_root = mountpoint;

    pr_debug("[SYSFS] Mounted sysfs on %s successfully.\n", dir_name);
    return 0;
}

static file_system_type_t g_sysfs_type = {
    .name = "sysfs",
    .mount = sysfs_mount,
};

void sysfs_init(void)
{
    vfs_register_fs(&g_sysfs_type);
}
