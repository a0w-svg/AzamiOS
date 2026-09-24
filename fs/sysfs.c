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
#include "../drivers/block/block.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/sched/sched.h"
#include "../kernel/lib/string.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/topology.h"
#include "../include/azami/net.h"
#include "../drivers/base/base.h"
#include "../userland/libc/include/sys/dirent.h"
#include "../kernel/perf/ktrace.h"

#define SYSFS_SUPER_MAGIC 0x62656572

typedef enum {
    SYSFS_TYPE_ROOT_DIR,
    SYSFS_TYPE_CLASS_DIR,
    SYSFS_TYPE_DEVICES_DIR,
    SYSFS_TYPE_KERNEL_DIR,
    SYSFS_TYPE_POWER_DIR,
    SYSFS_TYPE_BUS_DIR,
    SYSFS_TYPE_FS_DIR,
    SYSFS_TYPE_FS_SUBDIR,

    /* Class subdirectories */
    SYSFS_TYPE_CLASS_NET_DIR,
    SYSFS_TYPE_CLASS_BLOCK_DIR,
    SYSFS_TYPE_CLASS_SOUND_DIR,

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
    SYSFS_TYPE_BLOCK_ATTR_RO,
    SYSFS_TYPE_BLOCK_ATTR_START,
    SYSFS_TYPE_BLOCK_QUEUE_DIR,
    SYSFS_TYPE_BLOCK_QUEUE_ATTR,

    /* Sound */
    SYSFS_TYPE_SOUND_CARD_DIR,
    SYSFS_TYPE_SOUND_ATTR_ID,

    /* CPU topology */
    SYSFS_TYPE_SYSTEM_DIR,
    SYSFS_TYPE_CPU_DIR,
    SYSFS_TYPE_CPU_CORE_DIR,
    SYSFS_TYPE_CPU_ATTR_ONLINE,
    SYSFS_TYPE_CPU_ATTR_PRESENT,
    SYSFS_TYPE_CPU_ATTR_POSSIBLE,
    SYSFS_TYPE_CPU_TOPO_DIR,
    SYSFS_TYPE_CPU_TOPO_ATTR,
    SYSFS_TYPE_CPU_CACHE_DIR,
    SYSFS_TYPE_CPU_CORE_ONLINE,

    /* Driver-model backed nodes (drivers/base) — buses, drivers, classes and
     * devices are enumerated live rather than hard-coded here. */
    SYSFS_TYPE_DM_BUS_DIR,
    SYSFS_TYPE_DM_BUS_DEVICES,
    SYSFS_TYPE_DM_BUS_DRIVERS,
    SYSFS_TYPE_DM_DRIVER_DIR,
    SYSFS_TYPE_DM_DRIVER_ATTR,
    SYSFS_TYPE_DM_CLASS_DIR,
    SYSFS_TYPE_DM_DEVICE_DIR,
    SYSFS_TYPE_DM_DEVICE_ATTR,

    /* Power & Kernel */
    SYSFS_TYPE_POWER_ATTR_STATE,
    SYSFS_TYPE_KERNEL_ATTR_UEVENT,
    SYSFS_TYPE_KERNEL_ATTR_PROFILING,
    SYSFS_TYPE_KERNEL_TRACE_DIR,
    SYSFS_TYPE_KERNEL_TRACE_ENABLE,
    SYSFS_TYPE_KERNEL_TRACE_PIPE,
    SYSFS_TYPE_KERNEL_TRACE_CLEAR,
} sysfs_node_type_t;

/*
 * name  — first path component (bus or class name for driver-model nodes)
 * name2 — device or driver name within that bus/class
 * name3 — attribute name
 * index — node-specific; for driver-model device nodes, 0 = reached through
 *         /sys/bus/<bus>, 1 = reached through /sys/class/<class>
 */
typedef struct {
    sysfs_node_type_t type;
    char name[48];
    char name2[48];
    char name3[32];
    u32 index;
} sysfs_priv_t;

/* Forward declarations */
static struct dentry *sysfs_lookup(struct inode *dir, struct dentry *dentry);
static s64 sysfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 sysfs_file_write(struct file *filp, const void *buf, size_t len, u64 *offset);
static s64 sysfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);

static inode_operations_t g_sysfs_inode_ops = {
    .lookup = sysfs_lookup,
};

static file_operations_t g_sysfs_file_ops = {
    .read = sysfs_file_read,
    .write = sysfs_file_write,
    .readdir = sysfs_dir_readdir,
};

/* ── /sys/devices/system/cpu/cpuN/topology ────────────────────────────────
 *
 * The standard Linux attributes. Userspace reads this tree rather than
 * CPUID — it is the only way an unprivileged process can learn the machine's
 * core/thread layout — and a great deal of software sizes itself from it:
 * lscpu, hwloc, OpenMP and TBB runtimes deciding how many worker threads a
 * machine is worth, JVM ergonomics, container CPU accounting. With the tree
 * absent they all fall back to "one thread per logical CPU", which on an SMT
 * part oversubscribes every physical core by two.
 */

/* Stable storage for the cpuN directory names handed back by readdir(): the
 * caller keeps the pointers past this function's return, so they cannot be
 * stack buffers. One slot per possible CPU, filled on demand. */
static char g_sysfs_cpu_names[SMP_MAX_CPUS][12];

static const char *sysfs_cpu_dir_name(u32 cpu)
{
    if (cpu >= SMP_MAX_CPUS) return "cpu0";
    if (!g_sysfs_cpu_names[cpu][0])
        scnprintf(g_sysfs_cpu_names[cpu], sizeof(g_sysfs_cpu_names[cpu]), "cpu%u", cpu);
    return g_sysfs_cpu_names[cpu];
}

static bool sysfs_cpu_topo_attr_valid(const char *name)
{
    static const char *attrs[] = {
        "physical_package_id", "die_id", "core_id",
        "thread_siblings", "thread_siblings_list",
        "core_siblings", "core_siblings_list",
    };
    for (u32 i = 0; i < ARRAY_SIZE(attrs); i++)
        if (strcmp(name, attrs[i]) == 0) return true;
    return false;
}

/* Linux prints a cpumask as 32-bit hex groups, most significant first,
 * separated by commas — "00000000,0000000f" for CPUs 0-3 on a machine with
 * more than 32 possible CPUs. Groups beyond the highest set bit are omitted
 * for small machines, which is what every parser expects. */
static size_t sysfs_format_cpumask(char *buf, size_t max, u64 mask)
{
    if (mask >> 32)
        return (size_t)scnprintf(buf, max, "%08x,%08x\n",
                                 (unsigned)(mask >> 32), (unsigned)(mask & 0xFFFFFFFFULL));
    return (size_t)scnprintf(buf, max, "%08x\n", (unsigned)(mask & 0xFFFFFFFFULL));
}

/* And as a range list — "0-3", "0,2", "0-1,4-5". */
static size_t sysfs_format_cpulist(char *buf, size_t max, u64 mask)
{
    size_t off = 0;
    bool first = true;

    for (u32 i = 0; i < 64; ) {
        if (!(mask & (1ULL << i))) { i++; continue; }
        u32 start = i;
        while (i < 64 && (mask & (1ULL << i))) i++;
        u32 end = i - 1;

        off += (size_t)scnprintf(buf + off, max > off ? max - off : 0,
                                 "%s%u", first ? "" : ",", start);
        if (end != start)
            off += (size_t)scnprintf(buf + off, max > off ? max - off : 0, "-%u", end);
        first = false;
    }
    off += (size_t)scnprintf(buf + off, max > off ? max - off : 0, "\n");
    return off;
}

static size_t sysfs_format_cpu_topo(char *buf, size_t max, u32 cpu, const char *attr)
{
    const cpu_topology_t *t = topology_of(cpu);

    /* No decoded topology for this CPU: report it as a package of its own
     * with no siblings, which is both true as far as anything here knows and
     * the answer that makes consumers behave conservatively. */
    u64 self = (cpu < 64) ? (1ULL << cpu) : 0;
    u32 pkg  = t ? t->package_id : cpu;
    u32 die  = t ? t->die_id : 0;
    u32 core = t ? t->core_id : 0;
    u64 smt  = (t && t->smt_mask)  ? t->smt_mask  : self;
    u64 llc  = (t && t->core_mask) ? t->core_mask : self;

    if (strcmp(attr, "physical_package_id") == 0)
        return (size_t)scnprintf(buf, max, "%u\n", pkg);
    if (strcmp(attr, "die_id") == 0)
        return (size_t)scnprintf(buf, max, "%u\n", die);
    if (strcmp(attr, "core_id") == 0)
        return (size_t)scnprintf(buf, max, "%u\n", core);
    if (strcmp(attr, "thread_siblings") == 0)
        return sysfs_format_cpumask(buf, max, smt);
    if (strcmp(attr, "thread_siblings_list") == 0)
        return sysfs_format_cpulist(buf, max, smt);
    if (strcmp(attr, "core_siblings") == 0)
        return sysfs_format_cpumask(buf, max, llc);
    if (strcmp(attr, "core_siblings_list") == 0)
        return sysfs_format_cpulist(buf, max, llc);

    return (size_t)scnprintf(buf, max, "0\n");
}

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

/* Attach the second and third path components to a freshly allocated node. */
static inode_t *sysfs_name23(inode_t *inode, const char *n2, const char *n3)
{
    if (!inode || !inode->i_private) return inode;
    sysfs_priv_t *priv = (sysfs_priv_t *)inode->i_private;
    if (n2) strncpy(priv->name2, n2, sizeof(priv->name2) - 1);
    if (n3) strncpy(priv->name3, n3, sizeof(priv->name3) - 1);
    return inode;
}

/* --------------------------------------------------------------------------
 * Driver-Model Backed Nodes
 *
 * /sys/bus/<bus>/devices/<dev>/<attr>, /sys/bus/<bus>/drivers/<drv>,
 * /sys/class/<class>/<dev>/<attr> and /sys/devices/<bus>/<dev> are all
 * projections of the same live structures in drivers/base, so nothing here
 * is a hard-coded list.
 * -------------------------------------------------------------------------- */

/* Resolve a driver-model device node back to the device it names. */
static dm_device_t *sysfs_dm_device(sysfs_priv_t *priv)
{
    if (priv->index == 1) {
        dm_class_t *cls = dm_class_find(priv->name);
        if (!cls) return NULL;
        for (u32 i = 0; ; i++) {
            dm_device_t *d = dm_class_device_nth(cls, i);
            if (!d) return NULL;
            if (strcmp(d->name, priv->name2) == 0) return d;
        }
    }
    dm_bus_t *bus = dm_bus_find(priv->name);
    return bus ? dm_device_find(bus, priv->name2) : NULL;
}

static dm_driver_t *sysfs_dm_driver(sysfs_priv_t *priv)
{
    dm_bus_t *bus = dm_bus_find(priv->name);
    if (!bus) return NULL;
    for (u32 i = 0; ; i++) {
        dm_driver_t *d = dm_driver_nth(bus, i);
        if (!d) return NULL;
        if (strcmp(d->name, priv->name2) == 0) return d;
    }
}

/* The attribute set of a device: the generic ones, then the bus's own. */
static const char *sysfs_dm_device_attr(dm_device_t *dev, u32 idx)
{
    u32 n = 0;
    for (const char *const *a = dm_device_generic_attrs; *a; a++, n++) {
        if (n == idx) return *a;
    }
    if (dev->bus && dev->bus->dev_attrs) {
        for (const char *const *a = dev->bus->dev_attrs; *a; a++, n++) {
            if (n == idx) return *a;
        }
    }
    return NULL;
}

/* Names the idx-th child of a driver-model directory (excluding . and ..). */
static const char *sysfs_dm_child(sysfs_priv_t *priv, u64 idx, u8 *dtype)
{
    *dtype = DT_DIR;

    switch (priv->type) {
    case SYSFS_TYPE_BUS_DIR: {
        dm_bus_t *bus = dm_bus_nth((u32)idx);
        return bus ? bus->name : NULL;
    }
    case SYSFS_TYPE_DM_BUS_DIR:
        if (idx == 0) return "devices";
        if (idx == 1) return "drivers";
        return NULL;
    case SYSFS_TYPE_DM_BUS_DEVICES: {
        dm_bus_t *bus = dm_bus_find(priv->name);
        dm_device_t *dev = bus ? dm_device_nth(bus, (u32)idx) : NULL;
        return dev ? dev->name : NULL;
    }
    case SYSFS_TYPE_DM_BUS_DRIVERS: {
        dm_bus_t *bus = dm_bus_find(priv->name);
        dm_driver_t *drv = bus ? dm_driver_nth(bus, (u32)idx) : NULL;
        return drv ? drv->name : NULL;
    }
    case SYSFS_TYPE_DM_DRIVER_DIR:
        *dtype = DT_REG;
        return (idx == 0) ? "bound" : NULL;
    case SYSFS_TYPE_DM_CLASS_DIR: {
        dm_class_t *cls = dm_class_find(priv->name);
        dm_device_t *dev = cls ? dm_class_device_nth(cls, (u32)idx) : NULL;
        return dev ? dev->name : NULL;
    }
    case SYSFS_TYPE_DM_DEVICE_DIR: {
        dm_device_t *dev = sysfs_dm_device(priv);
        if (!dev) return NULL;
        *dtype = DT_REG;
        return sysfs_dm_device_attr(dev, (u32)idx);
    }
    default:
        return NULL;
    }
}

static bool sysfs_is_dm_dir(sysfs_node_type_t type)
{
    switch (type) {
    case SYSFS_TYPE_BUS_DIR:
    case SYSFS_TYPE_DM_BUS_DIR:
    case SYSFS_TYPE_DM_BUS_DEVICES:
    case SYSFS_TYPE_DM_BUS_DRIVERS:
    case SYSFS_TYPE_DM_DRIVER_DIR:
    case SYSFS_TYPE_DM_CLASS_DIR:
    case SYSFS_TYPE_DM_DEVICE_DIR:
        return true;
    default:
        return false;
    }
}

/* Emit dirents for a driver-model directory, walking it live each call. */
static s64 sysfs_dm_readdir(sysfs_priv_t *priv, void *dirent_buf, size_t len, u64 *offset)
{
    size_t written = 0;
    u8 *out = (u8 *)dirent_buf;
    u64 idx = *offset;

    for (;;) {
        const char *name;
        u8 dtype;

        if (idx == 0)       { name = ".";  dtype = DT_DIR; }
        else if (idx == 1)  { name = ".."; dtype = DT_DIR; }
        else {
            name = sysfs_dm_child(priv, idx - 2, &dtype);
            if (!name) break;
        }

        size_t nlen   = strlen(name);
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + nlen + 1, 8);
        if (written + reclen > len) {
            if (written == 0) return -(s64)EINVAL;
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)(out + written);
        d->d_ino    = idx + 1;
        d->d_off    = idx + 1;
        d->d_reclen = (unsigned short)reclen;
        d->d_type   = dtype;
        memcpy(d->d_name, name, nlen + 1);

        written += reclen;
        idx++;
    }

    *offset = idx;
    return (s64)written;
}

/* --------------------------------------------------------------------------
 * Sysfs File Reading
 * -------------------------------------------------------------------------- */

static s64 sysfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (!filp || !buf || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    sysfs_priv_t *priv = (sysfs_priv_t *)filp->f_inode->i_private;
    /* A device's "uevent" attribute is several lines of key=value, so this is
     * sized for the largest attribute rather than the typical one. */
    char tmp[512];
    size_t total_len = 0;

    switch (priv->type) {
    case SYSFS_TYPE_DM_DEVICE_ATTR: {
        dm_device_t *dev = sysfs_dm_device(priv);
        if (!dev) return 0;
        int n = dm_device_attr_show(dev, priv->name3, tmp, sizeof(tmp));
        if (n < 0) return 0;
        total_len = (size_t)n;
        break;
    }
    case SYSFS_TYPE_DM_DRIVER_ATTR: {
        dm_driver_t *drv = sysfs_dm_driver(priv);
        if (!drv) return 0;
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n", drv->nbound);
        break;
    }
    case SYSFS_TYPE_NET_ATTR_ADDR: {
        net_device_t *ndev = net_get_default_device();
        if (ndev && strcmp(priv->name, "lo") != 0) {
            u8 *m = ndev->mac;
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%02x:%02x:%02x:%02x:%02x:%02x\n",
                                         m[0], m[1], m[2], m[3], m[4], m[5]);
        } else {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "00:00:00:00:00:00\n");
        }
        break;
    }
    case SYSFS_TYPE_NET_ATTR_OPERSTATE:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "up\n");
        break;
    case SYSFS_TYPE_NET_ATTR_MTU:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n", (strcmp(priv->name, "lo") == 0) ? 65536 : 1500);
        break;
    case SYSFS_TYPE_NET_ATTR_SPEED:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "1000\n");
        break;
    case SYSFS_TYPE_NET_ATTR_DUPLEX:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "full\n");
        break;
    case SYSFS_TYPE_NET_ATTR_TYPE:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n", (strcmp(priv->name, "lo") == 0) ? 772 : 1);
        break;
    /* Real counters from the same net_device_t stats block
     * fs/procfs.c's format_proc_net_dev() already reads for /proc/net/dev,
     * instead of fixed "14200"/"8400"/"128"/"64" that never moved no matter
     * how much traffic actually crossed the interface. */
    case SYSFS_TYPE_NET_STAT_RX_BYTES: {
        net_device_t *ndev = net_get_default_device();
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
            (unsigned long long)(ndev ? ndev->stats.rx_bytes : 0));
        break;
    }
    case SYSFS_TYPE_NET_STAT_TX_BYTES: {
        net_device_t *ndev = net_get_default_device();
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
            (unsigned long long)(ndev ? ndev->stats.tx_bytes : 0));
        break;
    }
    case SYSFS_TYPE_NET_STAT_RX_PACKETS: {
        net_device_t *ndev = net_get_default_device();
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
            (unsigned long long)(ndev ? ndev->stats.rx_packets : 0));
        break;
    }
    case SYSFS_TYPE_NET_STAT_TX_PACKETS: {
        net_device_t *ndev = net_get_default_device();
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
            (unsigned long long)(ndev ? ndev->stats.tx_packets : 0));
        break;
    }
    /* Every one of these used to be a constant chosen per hard-coded device
     * name, so /sys/block described a disk layout that had nothing to do
     * with the machine. They come from the block registry now. */
    case SYSFS_TYPE_BLOCK_ATTR_DEV: {
        block_dev_t *bd = block_dev_get(priv->name);
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u:%u\n",
                        bd ? (unsigned)MAJOR(bd->rdev) : 0u,
                        bd ? (unsigned)MINOR(bd->rdev) : 0u);
        break;
    }
    case SYSFS_TYPE_BLOCK_ATTR_SIZE: {
        /* Always in 512-byte units, whatever the device's sector size —
         * that is the documented unit for this file. */
        block_dev_t *bd = block_dev_get(priv->name);
        u64 sectors = bd ? (bd->sector_count * (u64)bd->sector_size) / 512 : 0;
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n", (unsigned long long)sectors);
        break;
    }
    case SYSFS_TYPE_BLOCK_ATTR_REMOVABLE: {
        block_dev_t *bd = block_dev_get(priv->name);
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%d\n",
                        (bd && (bd->flags & BLKDEV_REMOVABLE)) ? 1 : 0);
        break;
    }
    case SYSFS_TYPE_BLOCK_ATTR_RO: {
        block_dev_t *bd = block_dev_get(priv->name);
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%d\n",
                        (bd && (bd->flags & BLKDEV_RO)) ? 1 : 0);
        break;
    }
    case SYSFS_TYPE_BLOCK_ATTR_START: {
        block_dev_t *bd = block_dev_get(priv->name);
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
                        (unsigned long long)(bd ? bd->start_lba : 0));
        break;
    }
    case SYSFS_TYPE_BLOCK_ATTR_STAT:
        /* No per-device I/O counters are kept, and inventing plausible ones
         * (which is what the previous fixed line did) is worse than zeros:
         * a monitoring tool reading it would report traffic that never
         * happened. All-zero is the honest "nothing recorded". */
        total_len = (size_t)scnprintf(tmp, sizeof(tmp),
            "       0       0        0        0       0       0        0        0        0        0        0\n");
        break;
    case SYSFS_TYPE_BLOCK_QUEUE_ATTR: {
        block_dev_t *bd = block_dev_get(priv->name);
        u32 ss  = bd && bd->sector_size ? bd->sector_size : 512;
        u32 pss = bd && bd->phys_sector_size ? bd->phys_sector_size : ss;
        const char *a = priv->name3;
        if (strcmp(a, "logical_block_size") == 0 || strcmp(a, "hw_sector_size") == 0 ||
            strcmp(a, "minimum_io_size") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n", ss);
        } else if (strcmp(a, "physical_block_size") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n", pss);
        } else if (strcmp(a, "optimal_io_size") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "0\n");
        } else if (strcmp(a, "rotational") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%d\n",
                            (bd && (bd->flags & BLKDEV_ROTATIONAL)) ? 1 : 0);
        } else if (strcmp(a, "discard_granularity") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n",
                            (bd && bd->ops && bd->ops->trim) ? ss : 0u);
        } else if (strcmp(a, "discard_max_bytes") == 0) {
            u64 cap = bd ? bd->sector_count * (u64)ss : 0;
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
                            (unsigned long long)((bd && bd->ops && bd->ops->trim) ? cap : 0));
        } else if (strcmp(a, "max_sectors_kb") == 0 || strcmp(a, "max_hw_sectors_kb") == 0) {
            /* The 1 MiB cap block_map_range() enforces on one transfer. */
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "1024\n");
        } else if (strcmp(a, "read_ahead_kb") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "128\n");
        } else if (strcmp(a, "nr_requests") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "128\n");
        } else if (strcmp(a, "scheduler") == 0) {
            /* Requests go straight to the driver; "none" is what a
             * queue with no elevator reports. */
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "[none]\n");
        } else if (strcmp(a, "write_cache") == 0) {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%s\n",
                            (bd && bd->ops && bd->ops->flush) ? "write back" : "write through");
        } else {
            total_len = (size_t)scnprintf(tmp, sizeof(tmp), "0\n");
        }
        break;
    }
    case SYSFS_TYPE_SOUND_ATTR_ID:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "AzamiAudio0\n");
        break;
    case SYSFS_TYPE_CPU_ATTR_ONLINE: {
        /* The CPUs actually running, not just the ones the firmware listed.
         * An AP that failed to come up is present but not online, and
         * anything sizing a thread pool off this file would otherwise create
         * workers for a core that will never execute them. */
        u64 mask = smp_online_mask();
        if (!mask) mask = 1ULL;
        total_len = sysfs_format_cpulist(tmp, sizeof(tmp), mask);
        break;
    }
    case SYSFS_TYPE_CPU_ATTR_PRESENT:
    case SYSFS_TYPE_CPU_ATTR_POSSIBLE: {
        u32 cpus = smp_cpu_count();
        if (cpus == 0) cpus = 1;
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "0-%u\n", cpus - 1);
        break;
    }
    case SYSFS_TYPE_CPU_CORE_ONLINE:
        /* Real state, not a constant: an AP that failed to come up is
         * present but not online, and this is where userspace looks. */
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%u\n",
                                      smp_cpu_online(priv->index) ? 1u : 0u);
        break;
    case SYSFS_TYPE_CPU_TOPO_ATTR:
        total_len = sysfs_format_cpu_topo(tmp, sizeof(tmp), priv->index, priv->name);
        break;
    case SYSFS_TYPE_POWER_ATTR_STATE:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "freeze mem disk\n");
        break;
    case SYSFS_TYPE_KERNEL_ATTR_UEVENT:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "%llu\n",
                                     (unsigned long long)dm_uevent_seqnum());
        break;
    case SYSFS_TYPE_KERNEL_ATTR_PROFILING:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "0\n");
        break;
    case SYSFS_TYPE_KERNEL_TRACE_ENABLE:
        total_len = ktrace_get_enabled_list(tmp, sizeof(tmp));
        break;
    case SYSFS_TYPE_KERNEL_TRACE_PIPE: {
        size_t n = ktrace_read_text((char *)buf, len);
        return (s64)n;
    }
    case SYSFS_TYPE_KERNEL_TRACE_CLEAR:
        total_len = (size_t)scnprintf(tmp, sizeof(tmp), "0\n");
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

static s64 sysfs_file_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !buf || len == 0 || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;
    sysfs_priv_t *priv = (sysfs_priv_t *)filp->f_inode->i_private;
    char str[128];
    size_t copy_len = (len < sizeof(str) - 1) ? len : sizeof(str) - 1;
    memcpy(str, buf, copy_len);
    str[copy_len] = '\0';
    while (copy_len > 0 && (str[copy_len - 1] == '\n' || str[copy_len - 1] == '\r' || str[copy_len - 1] == ' ')) {
        str[--copy_len] = '\0';
    }

    if (priv->type == SYSFS_TYPE_KERNEL_TRACE_ENABLE) {
        if (str[0] == '-') {
            ktrace_disable(str + 1);
        } else {
            ktrace_enable(str);
        }
        return (s64)len;
    } else if (priv->type == SYSFS_TYPE_KERNEL_TRACE_CLEAR) {
        ktrace_ring_clear();
        return (s64)len;
    }
    return -(s64)EPERM;
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
        } else if (strcmp(name, "block") == 0) {
            /* /sys/block is the traditional location and still the one most
             * tools look in first; /sys/class/block below is the same set. */
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 16, S_IFDIR | 0555, SYSFS_TYPE_CLASS_BLOCK_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_DIR) {
        if (strcmp(name, "net") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 20, S_IFDIR | 0555, SYSFS_TYPE_CLASS_NET_DIR, NULL, 0);
        } else if (strcmp(name, "block") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 21, S_IFDIR | 0555, SYSFS_TYPE_CLASS_BLOCK_DIR, NULL, 0);
        } else if (strcmp(name, "sound") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 22, S_IFDIR | 0555, SYSFS_TYPE_CLASS_SOUND_DIR, NULL, 0);
        } else {
            dm_class_t *cls = dm_class_find(name);
            if (cls) {
                dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 24, S_IFDIR | 0555,
                                                    SYSFS_TYPE_DM_CLASS_DIR, cls->name, 0);
            }
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
        /* The set of block devices used to be a fixed list of four names.
         * Ask the registry instead, so a disk that is actually present shows
         * up and one that is not does not. */
        if (block_dev_get(name)) {
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
        } else if (strcmp(name, "ro") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 205, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_RO, priv->name, 0);
        } else if (strcmp(name, "start") == 0) {
            /* Only a partition has a start offset, which is exactly how
             * lsblk and udev tell a partition from a whole disk. */
            block_dev_t *bd = block_dev_get(priv->name);
            if (bd && bd->parent)
                dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 206, S_IFREG | 0444, SYSFS_TYPE_BLOCK_ATTR_START, priv->name, 0);
        } else if (strcmp(name, "queue") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 207, S_IFDIR | 0555, SYSFS_TYPE_BLOCK_QUEUE_DIR, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_BLOCK_QUEUE_DIR) {
        static const char *qattrs[] = {
            "logical_block_size", "physical_block_size", "hw_sector_size",
            "minimum_io_size", "optimal_io_size", "rotational",
            "discard_granularity", "discard_max_bytes", "max_sectors_kb",
            "max_hw_sectors_kb", "read_ahead_kb", "nr_requests", "scheduler",
            "write_cache", "add_random", "nomerges",
        };
        for (size_t i = 0; i < ARRAY_SIZE(qattrs); i++) {
            if (strcmp(name, qattrs[i]) != 0) continue;
            inode_t *ino = sysfs_alloc_inode(dir->i_sb, 220 + i, S_IFREG | 0444,
                                             SYSFS_TYPE_BLOCK_QUEUE_ATTR, priv->name, 0);
            if (ino && ino->i_private)
                strncpy(((sysfs_priv_t *)ino->i_private)->name3, qattrs[i],
                        sizeof(((sysfs_priv_t *)ino->i_private)->name3) - 1);
            dentry->d_inode = ino;
            break;
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_SOUND_DIR) {
        if (strcmp(name, "card0") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 300, S_IFDIR | 0555, SYSFS_TYPE_SOUND_CARD_DIR, name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_SOUND_CARD_DIR) {
        if (strcmp(name, "id") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 301, S_IFREG | 0444, SYSFS_TYPE_SOUND_ATTR_ID, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_DEVICES_DIR) {
        if (strcmp(name, "system") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 500, S_IFDIR | 0555, SYSFS_TYPE_SYSTEM_DIR, NULL, 0);
        } else {
            dm_bus_t *bus = dm_bus_find(name);
            if (bus) {
                dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 803, S_IFDIR | 0555,
                                                    SYSFS_TYPE_DM_BUS_DEVICES, bus->name, 0);
            }
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
        } else if (strcmp(name, "topology") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 700 + priv->index, S_IFDIR | 0555, SYSFS_TYPE_CPU_TOPO_DIR, NULL, priv->index);
        }
    } else if (priv->type == SYSFS_TYPE_CPU_TOPO_DIR) {
        /* One inode type for every topology attribute; the attribute name is
         * carried in priv->name and dispatched on at read time. They are all
         * one short line derived from the same decoded record, so a type per
         * file would be six enum values and six switch arms saying the same
         * thing. */
        if (sysfs_cpu_topo_attr_valid(name)) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 720 + priv->index * 8,
                                                S_IFREG | 0444, SYSFS_TYPE_CPU_TOPO_ATTR,
                                                name, priv->index);
        }
    } else if (priv->type == SYSFS_TYPE_POWER_DIR) {
        if (strcmp(name, "state") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 600, S_IFREG | 0644, SYSFS_TYPE_POWER_ATTR_STATE, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_BUS_DIR) {
        dm_bus_t *bus = dm_bus_find(name);
        if (bus) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 800, S_IFDIR | 0555,
                                                SYSFS_TYPE_DM_BUS_DIR, bus->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_DM_BUS_DIR) {
        if (strcmp(name, "devices") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 801, S_IFDIR | 0555,
                                                SYSFS_TYPE_DM_BUS_DEVICES, priv->name, 0);
        } else if (strcmp(name, "drivers") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 802, S_IFDIR | 0555,
                                                SYSFS_TYPE_DM_BUS_DRIVERS, priv->name, 0);
        }
    } else if (priv->type == SYSFS_TYPE_DM_BUS_DEVICES) {
        dm_bus_t *bus = dm_bus_find(priv->name);
        dm_device_t *dev = bus ? dm_device_find(bus, name) : NULL;
        if (dev) {
            dentry->d_inode = sysfs_name23(
                sysfs_alloc_inode(dir->i_sb, 810, S_IFDIR | 0555,
                                  SYSFS_TYPE_DM_DEVICE_DIR, priv->name, 0),
                dev->name, NULL);
        }
    } else if (priv->type == SYSFS_TYPE_DM_BUS_DRIVERS) {
        sysfs_priv_t probe = *priv;
        strncpy(probe.name2, name, sizeof(probe.name2) - 1);
        if (sysfs_dm_driver(&probe)) {
            dentry->d_inode = sysfs_name23(
                sysfs_alloc_inode(dir->i_sb, 820, S_IFDIR | 0555,
                                  SYSFS_TYPE_DM_DRIVER_DIR, priv->name, 0),
                name, NULL);
        }
    } else if (priv->type == SYSFS_TYPE_DM_DRIVER_DIR) {
        if (strcmp(name, "bound") == 0) {
            dentry->d_inode = sysfs_name23(
                sysfs_alloc_inode(dir->i_sb, 821, S_IFREG | 0444,
                                  SYSFS_TYPE_DM_DRIVER_ATTR, priv->name, 0),
                priv->name2, name);
        }
    } else if (priv->type == SYSFS_TYPE_DM_CLASS_DIR) {
        sysfs_priv_t probe = *priv;
        probe.index = 1;
        strncpy(probe.name2, name, sizeof(probe.name2) - 1);
        if (sysfs_dm_device(&probe)) {
            dentry->d_inode = sysfs_name23(
                sysfs_alloc_inode(dir->i_sb, 830, S_IFDIR | 0555,
                                  SYSFS_TYPE_DM_DEVICE_DIR, priv->name, 1),
                name, NULL);
        }
    } else if (priv->type == SYSFS_TYPE_DM_DEVICE_DIR) {
        dm_device_t *dev = sysfs_dm_device(priv);
        bool known = false;
        for (u32 i = 0; dev; i++) {
            const char *attr = sysfs_dm_device_attr(dev, i);
            if (!attr) break;
            if (strcmp(attr, name) == 0) { known = true; break; }
        }
        if (known) {
            dentry->d_inode = sysfs_name23(
                sysfs_alloc_inode(dir->i_sb, 840, S_IFREG | 0444,
                                  SYSFS_TYPE_DM_DEVICE_ATTR, priv->name, priv->index),
                priv->name2, name);
        }
    } else if (priv->type == SYSFS_TYPE_KERNEL_DIR) {
        if (strcmp(name, "uevent_seqnum") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 700, S_IFREG | 0444, SYSFS_TYPE_KERNEL_ATTR_UEVENT, NULL, 0);
        } else if (strcmp(name, "profiling") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 701, S_IFREG | 0644, SYSFS_TYPE_KERNEL_ATTR_PROFILING, NULL, 0);
        } else if (strcmp(name, "trace") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 702, S_IFDIR | 0555, SYSFS_TYPE_KERNEL_TRACE_DIR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_KERNEL_TRACE_DIR) {
        if (strcmp(name, "enable") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 710, S_IFREG | 0666, SYSFS_TYPE_KERNEL_TRACE_ENABLE, NULL, 0);
        } else if (strcmp(name, "pipe") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 711, S_IFREG | 0444, SYSFS_TYPE_KERNEL_TRACE_PIPE, NULL, 0);
        } else if (strcmp(name, "clear") == 0) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 712, S_IFREG | 0666, SYSFS_TYPE_KERNEL_TRACE_CLEAR, NULL, 0);
        }
    } else if (priv->type == SYSFS_TYPE_FS_DIR) {
        file_system_type_t *fs = vfs_find_fs(name);
        if (fs) {
            dentry->d_inode = sysfs_alloc_inode(dir->i_sb, 900, S_IFDIR | 0555, SYSFS_TYPE_FS_SUBDIR, fs->name, 0);
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

    if (sysfs_is_dm_dir(priv->type)) {
        return sysfs_dm_readdir(priv, dirent_buf, len, offset);
    }

    const char *entries[32];
    u8 types[32];
    u64 total_entries = 2;
    entries[0] = "."; types[0] = DT_DIR;
    entries[1] = ".."; types[1] = DT_DIR;

    if (priv->type == SYSFS_TYPE_ROOT_DIR) {
        const char *r[] = { "class", "devices", "kernel", "power", "bus", "fs", "block" };
        for (int i = 0; i < 7; i++) { entries[total_entries] = r[i]; types[total_entries++] = DT_DIR; }
    } else if (priv->type == SYSFS_TYPE_CLASS_DIR) {
        const char *c[] = { "net", "block", "sound" };
        for (int i = 0; i < 3; i++) { entries[total_entries] = c[i]; types[total_entries++] = DT_DIR; }
        for (u32 i = 0; total_entries < ARRAY_SIZE(entries); i++) {
            dm_class_t *cls = dm_class_nth(i);
            if (!cls) break;
            entries[total_entries] = cls->name; types[total_entries++] = DT_DIR;
        }
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
        for (block_dev_t *bd = block_dev_first(); bd && total_entries < ARRAY_SIZE(entries);
             bd = block_dev_next(bd)) {
            entries[total_entries] = bd->name; types[total_entries++] = DT_DIR;
        }
    } else if (priv->type == SYSFS_TYPE_BLOCK_DEV_DIR) {
        const char *ba[] = { "dev", "size", "removable", "stat", "ro" };
        for (int i = 0; i < 5; i++) { entries[total_entries] = ba[i]; types[total_entries++] = DT_REG; }
        block_dev_t *bd = block_dev_get(priv->name);
        if (bd && bd->parent) { entries[total_entries] = "start"; types[total_entries++] = DT_REG; }
        entries[total_entries] = "queue"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_BLOCK_QUEUE_DIR) {
        static const char *qa[] = {
            "logical_block_size", "physical_block_size", "hw_sector_size",
            "minimum_io_size", "optimal_io_size", "rotational",
            "discard_granularity", "discard_max_bytes", "max_sectors_kb",
            "max_hw_sectors_kb", "read_ahead_kb", "nr_requests", "scheduler",
            "write_cache", "add_random", "nomerges",
        };
        for (size_t i = 0; i < ARRAY_SIZE(qa) && total_entries < ARRAY_SIZE(entries); i++) {
            entries[total_entries] = qa[i]; types[total_entries++] = DT_REG;
        }
    } else if (priv->type == SYSFS_TYPE_CLASS_SOUND_DIR) {
        entries[total_entries] = "card0"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_SOUND_CARD_DIR) {
        entries[total_entries] = "id"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_DEVICES_DIR) {
        /* The topological view: /sys/devices/<bus>/<device>. */
        entries[total_entries] = "system"; types[total_entries++] = DT_DIR;
        for (u32 i = 0; total_entries < ARRAY_SIZE(entries); i++) {
            dm_bus_t *bus = dm_bus_nth(i);
            if (!bus) break;
            entries[total_entries] = bus->name; types[total_entries++] = DT_DIR;
        }
    } else if (priv->type == SYSFS_TYPE_SYSTEM_DIR) {
        entries[total_entries] = "cpu"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_CPU_DIR) {
        entries[total_entries] = "online"; types[total_entries++] = DT_REG;
        entries[total_entries] = "present"; types[total_entries++] = DT_REG;
        entries[total_entries] = "possible"; types[total_entries++] = DT_REG;
        /* One entry per CPU that actually exists. This used to be a fixed
         * cpu0..cpu3, which listed four CPUs on a single-core boot and hid
         * every CPU past the fourth on anything larger — and the directories
         * it named did resolve, because lookup parses any cpuN. Anything
         * enumerating CPUs through sysfs (nproc, lscpu, hwloc, glibc's
         * get_nprocs) reads this list. */
        u32 ncpu = smp_cpu_count();
        if (ncpu == 0) ncpu = 1;
        for (u32 i = 0; i < ncpu && total_entries < ARRAY_SIZE(entries) - 1; i++) {
            entries[total_entries] = sysfs_cpu_dir_name(i);
            types[total_entries++] = DT_DIR;
        }
    } else if (priv->type == SYSFS_TYPE_CPU_CORE_DIR) {
        entries[total_entries] = "online"; types[total_entries++] = DT_REG;
        entries[total_entries] = "topology"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_CPU_TOPO_DIR) {
        entries[total_entries] = "physical_package_id"; types[total_entries++] = DT_REG;
        entries[total_entries] = "die_id";              types[total_entries++] = DT_REG;
        entries[total_entries] = "core_id";             types[total_entries++] = DT_REG;
        entries[total_entries] = "thread_siblings";     types[total_entries++] = DT_REG;
        entries[total_entries] = "thread_siblings_list";types[total_entries++] = DT_REG;
        entries[total_entries] = "core_siblings";       types[total_entries++] = DT_REG;
        entries[total_entries] = "core_siblings_list";  types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_POWER_DIR) {
        entries[total_entries] = "state"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_KERNEL_DIR) {
        entries[total_entries] = "uevent_seqnum"; types[total_entries++] = DT_REG;
        entries[total_entries] = "profiling"; types[total_entries++] = DT_REG;
        entries[total_entries] = "trace"; types[total_entries++] = DT_DIR;
    } else if (priv->type == SYSFS_TYPE_KERNEL_TRACE_DIR) {
        entries[total_entries] = "enable"; types[total_entries++] = DT_REG;
        entries[total_entries] = "pipe"; types[total_entries++] = DT_REG;
        entries[total_entries] = "clear"; types[total_entries++] = DT_REG;
    } else if (priv->type == SYSFS_TYPE_FS_DIR) {
        const char *known_fs[] = { "ext2", "tmpfs", "devpts", "devfs", "procfs", "squashfs", "fat32" };
        for (size_t i = 0; i < sizeof(known_fs) / sizeof(known_fs[0]) && total_entries < ARRAY_SIZE(entries); i++) {
            if (vfs_find_fs(known_fs[i])) {
                entries[total_entries] = known_fs[i];
                types[total_entries++] = DT_DIR;
            }
        }
    } else if (priv->type == SYSFS_TYPE_FS_SUBDIR) {
        /* No extra entries needed, just . and .. */
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

static s64 sysfs_statfs(super_block_t *sb, struct statfs *buf)
{
    if (!buf) return -(s64)EFAULT;
    memset(buf, 0, sizeof(struct statfs));
    buf->f_type = SYSFS_SUPER_MAGIC;
    buf->f_bsize = 4096;
    buf->f_blocks = 0;
    buf->f_bfree = 0;
    buf->f_bavail = 0;
    buf->f_files = 0;
    buf->f_ffree = 0;
    buf->f_namelen = 255;
    (void)sb;
    return 0;
}

static super_operations_t g_sysfs_super_ops = {
    .statfs = sysfs_statfs,
};

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
    sb->s_dev = vfs_alloc_anon_dev();
    sb->s_blocksize = 4096;
    sb->s_op = &g_sysfs_super_ops;

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
