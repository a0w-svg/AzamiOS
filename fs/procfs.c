/* ============================================================================
 * AzamiOS — Process Virtual Filesystem (procfs)
 * File: fs/procfs.c
 *
 * Implements /proc containing system information, telemetry, and per-PID state.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "procfs.h"
#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/mm/pmm.h"
#include "../kernel/sched/sched.h"
#include "../kernel/lib/string.h"
#include "../drivers/char/console.h"
#include "../include/azami/defs.h"
#include "../include/azami/net.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../userland/libc/include/sys/dirent.h"

#define PROCFS_SUPER_MAGIC 0x9FA0

typedef enum {
    PROCFS_TYPE_ROOT_DIR,
    PROCFS_TYPE_PID_DIR,
    PROCFS_TYPE_VERSION,
    PROCFS_TYPE_UPTIME,
    PROCFS_TYPE_MEMINFO,
    PROCFS_TYPE_CPUINFO,
    PROCFS_TYPE_STAT,
    PROCFS_TYPE_DMESG,
    PROCFS_TYPE_NET_DEV,
    PROCFS_TYPE_PID_STATUS,
    PROCFS_TYPE_PID_CMDLINE,
    PROCFS_TYPE_PID_STAT,
} procfs_node_type_t;

typedef struct {
    procfs_node_type_t type;
    u32 pid;
} procfs_priv_t;

/* Forward declarations */
static struct dentry *procfs_lookup(struct inode *dir, struct dentry *dentry);
static s64 procfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 procfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);

static inode_operations_t g_procfs_inode_ops = {
    .lookup = procfs_lookup,
};

static file_operations_t g_procfs_file_ops = {
    .read = procfs_file_read,
    .readdir = procfs_dir_readdir,
};

static inode_t *procfs_alloc_inode(super_block_t *sb, u64 ino, u32 mode, procfs_node_type_t type, u32 pid)
{
    inode_t *inode = (inode_t *)kzalloc(sizeof(inode_t));
    if (!inode) return NULL;

    inode->i_ino = ino;
    inode->i_mode = mode;
    inode->i_sb = sb;
    inode->i_op = &g_procfs_inode_ops;
    inode->i_fop = &g_procfs_file_ops;

    procfs_priv_t *priv = (procfs_priv_t *)kzalloc(sizeof(procfs_priv_t));
    if (priv) {
        priv->type = type;
        priv->pid = pid;
        inode->i_private = priv;
    }

    return inode;
}

static size_t format_proc_version(char *buf, size_t max)
{
    return (size_t)snprintf(buf, max,
        "AzamiOS version 7.0.0-posix (x86_64-elf-gcc) #1 SMP Limine 2026\n");
}

static size_t format_proc_uptime(char *buf, size_t max)
{
    u64 ticks = sched_get_ticks();
    u64 uptime_sec = ticks / 100;
    u64 uptime_dec = ticks % 100;
    u64 idle_ticks = sched_get_idle_ticks(0);
    u64 idle_sec = idle_ticks / 100;
    u64 idle_dec = idle_ticks % 100;
    return (size_t)snprintf(buf, max, "%llu.%02llu %llu.%02llu\n",
                            (unsigned long long)uptime_sec, (unsigned long long)uptime_dec,
                            (unsigned long long)idle_sec, (unsigned long long)idle_dec);
}

static size_t format_proc_meminfo(char *buf, size_t max)
{
    u64 total_kb = (pmm_get_total_pages() * PAGE_SIZE) / 1024;
    u64 free_kb  = (pmm_get_free_pages() * PAGE_SIZE) / 1024;
    u64 used_kb  = total_kb > free_kb ? (total_kb - free_kb) : 0;
    return (size_t)snprintf(buf, max,
        "MemTotal:       %8llu kB\n"
        "MemFree:        %8llu kB\n"
        "MemAvailable:   %8llu kB\n"
        "Buffers:               0 kB\n"
        "Cached:                0 kB\n"
        "SwapTotal:             0 kB\n"
        "SwapFree:              0 kB\n"
        "Active:         %8llu kB\n"
        "PageSize:           4096 B\n",
        (unsigned long long)total_kb, (unsigned long long)free_kb,
        (unsigned long long)free_kb, (unsigned long long)used_kb);
}

static size_t format_proc_cpuinfo(char *buf, size_t max)
{
    size_t off = 0;
    u32 cpu_count = smp_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    for (u32 i = 0; i < cpu_count; i++) {
        off += snprintf(buf + off, max > off ? max - off : 0,
            "processor       : %u\n"
            "vendor_id       : GenuineIntel/AMD (x86_64)\n"
            "model name      : x86_64 Long Mode SMP Virtual Processor\n"
            "cpu MHz         : 2400.000\n"
            "flags           : fpu vme de pse tsc msr pae mce cx8 apic sep mtrr pge mca cmov pat pse36 clflush mmx fxsr sse sse2 ht syscall nx lm smep smap rdrand rdseed\n"
            "\n", i);
    }
    return off;
}

static size_t format_proc_stat(char *buf, size_t max)
{
    size_t off = 0;
    u64 total_user = 0, total_idle = 0;
    u32 cpu_count = smp_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    for (u32 i = 0; i < cpu_count; i++) {
        total_user += sched_get_active_ticks(i);
        total_idle += sched_get_idle_ticks(i);
    }

    off += snprintf(buf + off, max > off ? max - off : 0,
        "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
        (unsigned long long)total_user, (unsigned long long)total_idle);

    for (u32 i = 0; i < cpu_count; i++) {
        off += snprintf(buf + off, max > off ? max - off : 0,
            "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n",
            i, (unsigned long long)sched_get_active_ticks(i),
            (unsigned long long)sched_get_idle_ticks(i));
    }
    return off;
}

static size_t format_proc_net_dev(char *buf, size_t max)
{
    u8 ip[4] = {0};
    net_get_ip(ip);
    net_device_t *ndev = net_get_default_device();
    const char *dname = ndev ? ndev->name : "net0";
    return (size_t)snprintf(buf, max,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0\n"
        "  %4s:   14200     128    0    0    0     0          0         0     8400      64    0    0    0     0       0          0\n",
        dname);
}

static process_t *find_proc_by_pid(u32 pid)
{
    process_t *p = sched_get_process_list();
    while (p) {
        if (p->pid == pid) return p;
        p = p->next;
    }
    return NULL;
}

static size_t format_pid_status(u32 pid, char *buf, size_t max)
{
    process_t *p = find_proc_by_pid(pid);
    if (!p) return (size_t)snprintf(buf, max, "State: X (dead)\n");

    const char *state_str = p->is_zombie ? "Z (zombie)" : "R (running)";
    u32 ppid = p->parent ? p->parent->pid : 0;
    u32 thread_cnt = 0;
    for (thread_t *t = p->threads; t; t = t->proc_next) thread_cnt++;

    u64 vmsize_kb = 0;
    if (p->heap_end >= p->heap_start) {
        vmsize_kb = (p->heap_end - p->heap_start) / 1024 + 64;
    } else {
        vmsize_kb = 64;
    }

    return (size_t)snprintf(buf, max,
        "Name:   %s\n"
        "State:  %s\n"
        "Tgid:   %u\n"
        "Pid:    %u\n"
        "PPid:   %u\n"
        "Threads:%u\n"
        "VmSize: %8llu kB\n",
        p->name[0] ? p->name : "process",
        state_str, p->pid, p->pid, ppid, thread_cnt, (unsigned long long)vmsize_kb);
}

static size_t format_pid_cmdline(u32 pid, char *buf, size_t max)
{
    process_t *p = find_proc_by_pid(pid);
    if (!p) return 0;
    return (size_t)snprintf(buf, max, "%s\n", p->name[0] ? p->name : "");
}

static size_t format_pid_stat(u32 pid, char *buf, size_t max)
{
    process_t *p = find_proc_by_pid(pid);
    if (!p) return 0;
    char state = p->is_zombie ? 'Z' : 'R';
    u32 ppid = p->parent ? p->parent->pid : 0;
    return (size_t)snprintf(buf, max,
        "%u (%s) %c %u 0 0 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        p->pid, p->name[0] ? p->name : "app", state, ppid);
}

/* --------------------------------------------------------------------------
 * Dentry Lookup
 * -------------------------------------------------------------------------- */

static struct dentry *procfs_lookup(struct inode *dir, struct dentry *dentry)
{
    if (!dir || !dentry || !dir->i_private) return dentry;
    procfs_priv_t *dir_priv = (procfs_priv_t *)dir->i_private;
    const char *name = dentry->d_name;

    if (dir_priv->type == PROCFS_TYPE_ROOT_DIR) {
        if (strcmp(name, "version") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 100, S_IFREG | 0444, PROCFS_TYPE_VERSION, 0);
        } else if (strcmp(name, "uptime") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 101, S_IFREG | 0444, PROCFS_TYPE_UPTIME, 0);
        } else if (strcmp(name, "meminfo") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 102, S_IFREG | 0444, PROCFS_TYPE_MEMINFO, 0);
        } else if (strcmp(name, "cpuinfo") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 103, S_IFREG | 0444, PROCFS_TYPE_CPUINFO, 0);
        } else if (strcmp(name, "stat") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 104, S_IFREG | 0444, PROCFS_TYPE_STAT, 0);
        } else if (strcmp(name, "dmesg") == 0 || strcmp(name, "kmsg") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 105, S_IFREG | 0444, PROCFS_TYPE_DMESG, 0);
        } else if (strcmp(name, "net") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 106, S_IFREG | 0444, PROCFS_TYPE_NET_DEV, 0);
        } else {
            /* Check if numeric PID */
            bool is_num = true;
            u32 pid = 0;
            for (int i = 0; name[i]; i++) {
                if (name[i] < '0' || name[i] > '9') { is_num = false; break; }
                pid = pid * 10 + (u32)(name[i] - '0');
            }
            if (is_num && pid > 0 && find_proc_by_pid(pid)) {
                dentry->d_inode = procfs_alloc_inode(dir->i_sb, 1000 + pid, S_IFDIR | 0555, PROCFS_TYPE_PID_DIR, pid);
            }
        }
    } else if (dir_priv->type == PROCFS_TYPE_PID_DIR) {
        u32 pid = dir_priv->pid;
        if (strcmp(name, "status") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 1, S_IFREG | 0444, PROCFS_TYPE_PID_STATUS, pid);
        } else if (strcmp(name, "cmdline") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 2, S_IFREG | 0444, PROCFS_TYPE_PID_CMDLINE, pid);
        } else if (strcmp(name, "stat") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 3, S_IFREG | 0444, PROCFS_TYPE_PID_STAT, pid);
        }
    }

    return dentry;
}

/* --------------------------------------------------------------------------
 * File Reading
 * -------------------------------------------------------------------------- */

static s64 procfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (!filp || !buf || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)filp->f_inode->i_private;

    if (priv->type == PROCFS_TYPE_DMESG) {
        return console_read_klog(buf, len, offset);
    }

    char tmp[2048];
    size_t total_len = 0;

    switch (priv->type) {
    case PROCFS_TYPE_VERSION:
        total_len = format_proc_version(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_UPTIME:
        total_len = format_proc_uptime(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_MEMINFO:
        total_len = format_proc_meminfo(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_CPUINFO:
        total_len = format_proc_cpuinfo(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_STAT:
        total_len = format_proc_stat(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_NET_DEV:
        total_len = format_proc_net_dev(tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_PID_STATUS:
        total_len = format_pid_status(priv->pid, tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_PID_CMDLINE:
        total_len = format_pid_cmdline(priv->pid, tmp, sizeof(tmp));
        break;
    case PROCFS_TYPE_PID_STAT:
        total_len = format_pid_stat(priv->pid, tmp, sizeof(tmp));
        break;
    default:
        return 0;
    }

    if (*offset >= total_len) return 0; /* EOF */

    size_t avail = total_len - (size_t)*offset;
    size_t copy_cnt = (len < avail) ? len : avail;
    memcpy(buf, tmp + *offset, copy_cnt);
    *offset += copy_cnt;

    return (s64)copy_cnt;
}

/* --------------------------------------------------------------------------
 * Directory Readdir
 * -------------------------------------------------------------------------- */

static const char *g_static_entries[] = {
    "version", "uptime", "meminfo", "cpuinfo", "stat", "dmesg", "net"
};
#define NUM_STATIC_ENTRIES (sizeof(g_static_entries) / sizeof(g_static_entries[0]))

static s64 procfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!filp || !dirent_buf || len == 0 || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)filp->f_inode->i_private;
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    if (priv->type == PROCFS_TYPE_ROOT_DIR) {
        /* Count processes for indexing */
        u32 pids[64];
        u32 pid_count = 0;

        sched_lock();
        process_t *p = sched_get_process_list();
        while (p && pid_count < 64) {
            pids[pid_count++] = p->pid;
            p = p->next;
        }
        sched_unlock();

        u64 total_entries = 2 + NUM_STATIC_ENTRIES + pid_count;

        while (idx < total_entries) {
            const char *name = NULL;
            char pid_name[16];
            u8 dtype = DT_REG;

            if (idx == 0) {
                name = "."; dtype = DT_DIR;
            } else if (idx == 1) {
                name = ".."; dtype = DT_DIR;
            } else if (idx - 2 < NUM_STATIC_ENTRIES) {
                name = g_static_entries[idx - 2];
                dtype = DT_REG;
            } else {
                u32 pidx = (u32)(idx - 2 - NUM_STATIC_ENTRIES);
                snprintf(pid_name, sizeof(pid_name), "%u", pids[pidx]);
                name = pid_name;
                dtype = DT_DIR;
            }

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
    } else if (priv->type == PROCFS_TYPE_PID_DIR) {
        const char *pid_entries[] = { ".", "..", "status", "cmdline", "stat" };
        u64 total_entries = 5;

        while (idx < total_entries) {
            const char *name = pid_entries[idx];
            u8 dtype = (idx < 2) ? DT_DIR : DT_REG;

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
    }

    *offset = idx;
    return (s64)written;
}

/* --------------------------------------------------------------------------
 * Mount & Init
 * -------------------------------------------------------------------------- */

static s64 procfs_mount(file_system_type_t *fs_type, const char *dev_name, const char *dir_name, void *data)
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

    sb->s_magic = PROCFS_SUPER_MAGIC;
    sb->s_blocksize = 4096;

    inode_t *root_inode = procfs_alloc_inode(sb, 1, S_IFDIR | 0555, PROCFS_TYPE_ROOT_DIR, 0);
    if (!root_inode) {
        kfree(sb);
        return -(s64)ENOMEM;
    }

    mountpoint->d_inode = root_inode;
    mountpoint->d_sb = sb;
    sb->s_root = mountpoint;

    pr_debug("[PROCFS] Mounted procfs on %s successfully.\n", dir_name);
    return 0;
}

static file_system_type_t g_procfs_type = {
    .name = "procfs",
    .mount = procfs_mount,
};

void procfs_init(void)
{
    vfs_register_fs(&g_procfs_type);
}
