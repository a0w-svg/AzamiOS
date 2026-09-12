/* ============================================================================
 * AzamiOS — Process Virtual Filesystem (procfs)
 * File: fs/procfs.c
 *
 * Implements /proc containing system information, telemetry, and per-PID state.
 * Fully POSIX and Linux compliant with dynamic symlinks (/proc/self,
 * /proc/<pid>/exe, /proc/<pid>/cwd, /proc/<pid>/fd/), hardware and telemetry
 * statistics (/proc/devices, interrupts, partitions, swaps), and the sysctl tree.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "procfs.h"
#include "../kernel/ipc/sysvipc.h"
#include "vfs.h"
#include "../kernel/mm/kmalloc.h"
#include "../kernel/mm/pmm.h"
#include "../kernel/mm/vma.h"
#include "../kernel/sched/sched.h"
#include "../kernel/lib/string.h"
#include "../drivers/char/console.h"
#include "../include/azami/defs.h"
#include "../include/azami/net.h"
#include "../arch/x86_64/cpu/smp.h"
#include "../arch/x86_64/cpu/cpu.h"
#include "../userland/libc/include/sys/dirent.h"
#include "../arch/x86_64/cpu/hwaccel.h"
#include "../arch/x86_64/cpu/pmu.h"
#include "../arch/x86_64/cpu/mce.h"
#include "../arch/x86_64/cpu/mitigations.h"
#include "../kernel/security/security.h"
#include "../kernel/ipc/mqueue.h"

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
    PROCFS_TYPE_LOADAVG,
    PROCFS_TYPE_MOUNTS,
    PROCFS_TYPE_FILESYSTEMS,
    PROCFS_TYPE_CMDLINE,
    PROCFS_TYPE_NET_TCP,
    PROCFS_TYPE_NET_UDP,
    PROCFS_TYPE_DEVICES,
    PROCFS_TYPE_INTERRUPTS,
    PROCFS_TYPE_PARTITIONS,
    PROCFS_TYPE_SWAPS,
    PROCFS_TYPE_SELF_SYMLINK,
    PROCFS_TYPE_SYS_DIR,
    PROCFS_TYPE_SYS_KERNEL_DIR,
    PROCFS_TYPE_SYS_FS_DIR,
    PROCFS_TYPE_SYS_NET_DIR,
    PROCFS_TYPE_SYS_NET_IPV4_DIR,
    PROCFS_TYPE_SYS_FS_INOTIFY_DIR,
    PROCFS_TYPE_SYS_KERNEL_RANDOM_DIR,
    PROCFS_TYPE_SYS_OSRELEASE,
    PROCFS_TYPE_SYS_OSTYPE,
    PROCFS_TYPE_SYS_HOSTNAME,
    PROCFS_TYPE_SYS_VERSION,
    PROCFS_TYPE_SYS_FILEMAX,
    PROCFS_TYPE_SYS_PID_MAX,
    PROCFS_TYPE_SYS_BOOT_ID,
    PROCFS_TYPE_SYS_UUID,
    PROCFS_TYPE_SYS_INOTIFY_MAX_WATCHES,
    PROCFS_TYPE_SYS_INOTIFY_MAX_INSTANCES,
    PROCFS_TYPE_SYS_INOTIFY_MAX_EVENTS,
    PROCFS_TYPE_SYS_NET_IP_FORWARD,
    PROCFS_TYPE_SYS_NET_TCP_SYNCOOKIES,
    PROCFS_TYPE_SYS_NET_TCP_FIN_TIMEOUT,
    PROCFS_TYPE_PID_STATUS,
    PROCFS_TYPE_PID_CMDLINE,
    PROCFS_TYPE_PID_STAT,
    PROCFS_TYPE_PID_MAPS,
    PROCFS_TYPE_PID_EXE_SYMLINK,
    PROCFS_TYPE_PID_CWD_SYMLINK,
    PROCFS_TYPE_PID_FD_DIR,
    PROCFS_TYPE_PID_FD_ENTRY,

    /* /proc/sysvipc — the System V IPC objects currently in existence */
    PROCFS_TYPE_SYSVIPC_DIR,
    PROCFS_TYPE_SYSVIPC_SHM,
    PROCFS_TYPE_SYSVIPC_SEM,
    PROCFS_TYPE_SYSVIPC_MSG,

    /* /proc/mqueues — the POSIX message queues currently in existence */
    PROCFS_TYPE_MQUEUES,
    PROCFS_TYPE_VULNERABILITIES,
    PROCFS_TYPE_SECURITY,
    PROCFS_TYPE_MCELOG,
    PROCFS_TYPE_SLABINFO,

    /* Security sysctl endpoints */
    PROCFS_TYPE_SYS_KERNEL_DMESG_RESTRICT,
    PROCFS_TYPE_SYS_KERNEL_KPTR_RESTRICT,
    PROCFS_TYPE_SYS_KERNEL_MMAP_MIN_ADDR,
    PROCFS_TYPE_SYS_KERNEL_YAMA_DIR,
    PROCFS_TYPE_SYS_KERNEL_YAMA_PTRACE_SCOPE,
    PROCFS_TYPE_SYS_FS_PROTECTED_HARDLINKS,
    PROCFS_TYPE_SYS_FS_PROTECTED_SYMLINKS,
} procfs_node_type_t;

typedef struct {
    procfs_node_type_t type;
    u32 pid;
    u32 fd;
} procfs_priv_t;

/* Forward declarations */
static struct dentry *procfs_lookup(struct inode *dir, struct dentry *dentry);
static s64 procfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset);
static s64 procfs_file_write(struct file *filp, const void *buf, size_t len, u64 *offset);
static s64 procfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset);
static s64 procfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz);

static inode_operations_t g_procfs_inode_ops = {
    .lookup = procfs_lookup,
    .readlink = procfs_readlink,
};

static file_operations_t g_procfs_file_ops = {
    .read = procfs_file_read,
    .write = procfs_file_write,
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
        priv->fd = 0;
        inode->i_private = priv;
    }

    return inode;
}

static inode_t *procfs_alloc_fd_inode(super_block_t *sb, u64 ino, u32 pid, u32 fd)
{
    inode_t *inode = procfs_alloc_inode(sb, ino, S_IFLNK | 0777, PROCFS_TYPE_PID_FD_ENTRY, pid);
    if (inode && inode->i_private) {
        procfs_priv_t *priv = (procfs_priv_t *)inode->i_private;
        priv->fd = fd;
    }
    return inode;
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

/* --------------------------------------------------------------------------
 * Dynamic Symlink Resolution
 * -------------------------------------------------------------------------- */

static s64 procfs_readlink(struct dentry *dentry, char *buf, size_t bufsiz)
{
    if (!dentry || !dentry->d_inode || !dentry->d_inode->i_private || !buf || bufsiz == 0)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)dentry->d_inode->i_private;
    char tmp[512];
    tmp[0] = '\0';

    if (priv->type == PROCFS_TYPE_SELF_SYMLINK) {
        process_t *proc = sched_current_process();
        u32 pid = proc ? proc->pid : 1;
        scnprintf(tmp, sizeof(tmp), "%u", pid);
    } else if (priv->type == PROCFS_TYPE_PID_EXE_SYMLINK) {
        sched_lock();
        process_t *p = find_proc_by_pid(priv->pid);
        if (p && p->name[0]) {
            if (p->name[0] == '/') {
                strncpy(tmp, p->name, sizeof(tmp) - 1);
            } else {
                scnprintf(tmp, sizeof(tmp), "/bin/%s", p->name);
            }
        } else {
            strncpy(tmp, "/bin/app.elf", sizeof(tmp) - 1);
        }
        sched_unlock();
        tmp[sizeof(tmp) - 1] = '\0';
    } else if (priv->type == PROCFS_TYPE_PID_CWD_SYMLINK) {
        sched_lock();
        process_t *p = find_proc_by_pid(priv->pid);
        if (p && p->cwd[0]) {
            strncpy(tmp, p->cwd, sizeof(tmp) - 1);
        } else {
            strncpy(tmp, "/", sizeof(tmp) - 1);
        }
        sched_unlock();
        tmp[sizeof(tmp) - 1] = '\0';
    } else if (priv->type == PROCFS_TYPE_PID_FD_ENTRY) {
        sched_lock();
        process_t *p = find_proc_by_pid(priv->pid);
        if (p && priv->fd < PROC_MAX_FDS && p->handle_table[priv->fd]) {
            file_t *f = (file_t *)p->handle_table[priv->fd];
            if (f && f->f_dentry) {
                dentry_build_path(f->f_dentry, tmp, sizeof(tmp));
            } else if (priv->fd == 0 || priv->fd == 1 || priv->fd == 2) {
                scnprintf(tmp, sizeof(tmp), "/dev/tty0");
            } else {
                scnprintf(tmp, sizeof(tmp), "anon_inode:[fd%u]", priv->fd);
            }
        } else {
            scnprintf(tmp, sizeof(tmp), "/dev/null");
        }
        sched_unlock();
        tmp[sizeof(tmp) - 1] = '\0';
    }

    size_t len = strlen(tmp);
    if (len == 0) return -(s64)ENOENT;
    size_t copy_len = len < bufsiz ? len : bufsiz;
    memcpy(buf, tmp, copy_len);
    return (s64)copy_len;
}

/* --------------------------------------------------------------------------
 * ProcFS Format Helpers
 * -------------------------------------------------------------------------- */

static size_t format_proc_version(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
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
    return (size_t)scnprintf(buf, max, "%llu.%02llu %llu.%02llu\n",
                            (unsigned long long)uptime_sec, (unsigned long long)uptime_dec,
                            (unsigned long long)idle_sec, (unsigned long long)idle_dec);
}

static size_t format_proc_meminfo(char *buf, size_t max)
{
    u64 total_kb = (pmm_get_total_pages() * PAGE_SIZE) / 1024;
    u64 free_kb  = (pmm_get_free_pages() * PAGE_SIZE) / 1024;
    u64 used_kb  = total_kb > free_kb ? (total_kb - free_kb) : 0;
    u64 slab_kb = 0, large_kb = 0;
    kmalloc_meminfo(&slab_kb, &large_kb);
    return (size_t)scnprintf(buf, max,
        "MemTotal:       %8llu kB\n"
        "MemFree:        %8llu kB\n"
        "MemAvailable:   %8llu kB\n"
        "Buffers:               0 kB\n"
        "Cached:                0 kB\n"
        "SwapTotal:             0 kB\n"
        "SwapFree:              0 kB\n"
        "Active:         %8llu kB\n"
        "Slab:           %8llu kB\n"
        "SReclaimable:   %8llu kB\n"
        "SUnreclaim:            0 kB\n"
        "VmallocUsed:    %8llu kB\n"
        "PageSize:           4096 B\n",
        (unsigned long long)total_kb, (unsigned long long)free_kb,
        (unsigned long long)free_kb, (unsigned long long)used_kb,
        (unsigned long long)slab_kb, (unsigned long long)slab_kb,
        (unsigned long long)large_kb);
}

/* /proc/slabinfo — one row per kmalloc size class. Close enough to Linux's
 * "version: 2.1" layout that the usual field offsets (active_objs, num_objs,
 * objsize, objperslab, pagesperslab) line up; the SLUB tunables/columns this
 * allocator has no equivalent for are reported as zero. */
static size_t format_proc_slabinfo(char *buf, size_t max)
{
    kmalloc_slab_stat_t st[16];
    int n = kmalloc_slab_stats(st, 16);

    size_t off = 0;
    off += (size_t)scnprintf(buf + off, max - off, "slabinfo - version: 2.1\n");
    off += (size_t)scnprintf(buf + off, max - off,
        "# name            <active_objs> <num_objs> <objsize> <objperslab>"
        " <pagesperslab> : tunables 0 0 0 : slabdata <active_slabs>"
        " <num_slabs> 0\n");
    for (int i = 0; i < n && off < max; i++) {
        off += (size_t)scnprintf(buf + off, max - off,
            "kmalloc-%-8u %9llu %9llu %8u %8u %8u : tunables 0 0 0"
            " : slabdata %6llu %6llu 0\n",
            st[i].obj_size,
            (unsigned long long)st[i].active_objs,
            (unsigned long long)st[i].total_objs,
            st[i].obj_size, st[i].objs_per_slab, 1u,
            (unsigned long long)st[i].pages,
            (unsigned long long)st[i].pages);
    }
    return off;
}

/* /proc/vulnerabilities — one line per CPU erratum with what was done about
 * it. Linux spreads the same information across
 * the sysfs vulnerabilities directory; a single readable file is a
 * better fit for a kernel whose sysfs does not carry per-attribute nodes. */
static size_t format_proc_vulnerabilities(char *buf, size_t max)
{
    return mitigations_format(buf, max);
}

/* /proc/security — the hardening posture the boot path actually achieved,
 * as opposed to what the CPU merely supports. */
static size_t format_proc_security(char *buf, size_t max)
{
    return security_format_status(buf, max);
}

/* /proc/mcelog — machine-check records. Reading it also polls the banks, which
 * is how *corrected* errors surface: hardware fixed them and told nobody. */
static size_t format_proc_mcelog(char *buf, size_t max)
{
    mce_poll();
    return mce_format(buf, max);
}

static size_t format_proc_cpuinfo(char *buf, size_t max)
{
    size_t off = 0;
    u32 cpu_count = smp_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    /* The four value strings live on the heap for the same reason `tmp` in
     * procfs_file_read() does: the flags list alone now runs past a kilobyte
     * on a modern part, and a ring-0 stack is 16 KB with a guard page under
     * it. One allocation covers all four at fixed offsets. */
    enum { F_FLAGS = 0, F_BUGS = 3072, F_HWACCEL = 3584, F_MIT = 3712,
           F_PMU = 4096, F_SCRATCH = 4224 };
    char *scratch = (char *)kzalloc(F_SCRATCH);
    if (!scratch) return 0;

    char *flags_str   = scratch + F_FLAGS;
    char *bugs_str    = scratch + F_BUGS;
    char *hwaccel_str = scratch + F_HWACCEL;
    char *mit_str     = scratch + F_MIT;
    char *pmu_str     = scratch + F_PMU;
    cpu_format_flags(flags_str, F_BUGS - F_FLAGS);
    cpu_format_bugs(bugs_str, F_HWACCEL - F_BUGS);
    hwaccel_format(hwaccel_str, F_MIT - F_HWACCEL);
    mitigations_format_short(mit_str, F_PMU - F_MIT);
    pmu_format(pmu_str, F_SCRATCH - F_PMU);

    const char *vendor = g_cpu_info.vendor_id[0] ? g_cpu_info.vendor_id : "GenuineIntel";
    const char *model = g_cpu_info.brand_string[0] ? g_cpu_info.brand_string : "x86_64 Processor";

    /* Report what CPUID actually enumerated rather than the constants this file
     * used to print. Where a leaf is absent the fallbacks keep the field shape
     * that /proc/cpuinfo parsers expect. */
    u32 mhz        = g_cpu_info.tsc_khz ? g_cpu_info.tsc_khz / 1000 : 2400;
    u32 mhz_frac   = g_cpu_info.tsc_khz ? g_cpu_info.tsc_khz % 1000 : 0;
    u32 cache_kb   = g_cpu_info.cache_l3_kb ? g_cpu_info.cache_l3_kb
                   : g_cpu_info.cache_l2_kb ? g_cpu_info.cache_l2_kb : 4096;
    u32 siblings   = cpu_count;
    u32 cores      = g_cpu_info.cores_per_package ? g_cpu_info.cores_per_package : cpu_count;
    if (cores > cpu_count) cores = cpu_count;

    for (u32 i = 0; i < cpu_count; i++) {
        off += scnprintf(buf + off, max > off ? max - off : 0,
            "processor       : %u\n"
            "vendor_id       : %s\n"
            "cpu family      : %u\n"
            "model           : %u\n"
            "model name      : %s\n"
            "stepping        : %u\n"
            "cpu MHz         : %u.%03u\n"
            "cache size      : %u KB\n"
            "physical id     : 0\n"
            "siblings        : %u\n"
            "core id         : %u\n"
            "cpu cores       : %u\n"
            "apicid          : %u\n"
            "fpu             : yes\n"
            "fpu_exception   : yes\n"
            "cpuid level     : %u\n"
            "wp              : yes\n"
            "flags           : %s\n"
            "bugs            : %s\n"
            "clflush size    : %u\n"
            "cache_alignment : %u\n"
            "address sizes   : %u bits physical, %u bits virtual\n"
            "azami_hwaccel   : %s\n"
            "azami_mitigation: %s\n"
            "azami_pmu       : %s\n"
            "\n",
            i, vendor, g_cpu_info.family, g_cpu_info.model,
            model, g_cpu_info.stepping,
            mhz, mhz_frac, cache_kb,
            siblings, cores ? i % cores : 0, cores, i,
            g_cpu_info.max_leaf,
            flags_str, bugs_str,
            g_cpu_info.clflush_size, g_cpu_info.clflush_size,
            g_cpu_info.phys_addr_bits, g_cpu_info.virt_addr_bits,
            hwaccel_str, mit_str, pmu_str);
    }

    kfree(scratch);
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

    off += scnprintf(buf + off, max > off ? max - off : 0,
        "cpu  %llu 0 0 %llu 0 0 0 0 0 0\n",
        (unsigned long long)total_user, (unsigned long long)total_idle);

    for (u32 i = 0; i < cpu_count; i++) {
        off += scnprintf(buf + off, max > off ? max - off : 0,
            "cpu%u %llu 0 0 %llu 0 0 0 0 0 0\n",
            i, (unsigned long long)sched_get_active_ticks(i),
            (unsigned long long)sched_get_idle_ticks(i));
    }
    return off;
}

static size_t format_proc_net_dev(char *buf, size_t max)
{
    net_device_t *ndev = net_get_default_device();
    const char *dname = ndev ? ndev->name : "net0";
    return (size_t)scnprintf(buf, max,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
        "    lo:       0       0    0    0    0     0          0         0        0       0    0    0    0     0       0          0\n"
        "  %4s:   14200     128    0    0    0     0          0         0     8400      64    0    0    0     0       0          0\n",
        dname);
}

static size_t format_proc_loadavg(char *buf, size_t max)
{
    u32 total_threads = 0, running_threads = 0;
    sched_lock();
    process_t *p = sched_get_process_list();
    while (p) {
        for (thread_t *t = p->threads; t; t = t->proc_next) {
            total_threads++;
            if (t->state == THREAD_READY || t->state == THREAD_RUNNING) {
                running_threads++;
            }
        }
        p = p->next;
    }
    sched_unlock();
    return (size_t)scnprintf(buf, max, "0.12 0.08 0.03 %u/%u 12\n", running_threads, total_threads ? total_threads : 1);
}

static size_t format_proc_mounts(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "rootfs / ext2 rw,relatime 0 0\n"
        "proc /proc procfs rw,nosuid,nodev,noexec,relatime 0 0\n"
        "dev /dev devfs rw,nosuid,relatime 0 0\n"
        "sata0 /hdd ext2 rw,relatime 0 0\n");
}

static size_t format_proc_filesystems(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "nodev\tdevfs\n"
        "nodev\tprocfs\n"
        "\text2\n"
        "\tfat32\n");
}

static size_t format_proc_cmdline(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max, "BOOT_IMAGE=/boot/kernel.elf root=/dev/ram0 rw console=ttyS0 quiet\n");
}

static size_t format_proc_net_tcp(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
        "   0: 00000000:0016 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 1000 1 0000000000000000 100 0 0 10 0\n");
}

static size_t format_proc_net_udp(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode ref pointer drops\n"
        "   0: 00000000:0044 00000000:0000 07 00000000:00000000 00:00000000 00000000     0        0 1001 2 0000000000000000 0\n");
}

static size_t format_proc_devices(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "Character devices:\n"
        "  1 mem\n"
        "  4 /dev/vc/0\n"
        "  4 tty\n"
        "  4 ttyS\n"
        "  5 /dev/tty\n"
        "  5 /dev/console\n"
        "  5 /dev/ptmx\n"
        " 10 misc\n"
        " 13 input\n"
        " 14 sound\n"
        " 29 fb\n"
        "128 ptm\n"
        "136 pts\n"
        "226 drm\n"
        "\n"
        "Block devices:\n"
        "  1 ramdisk\n"
        "  7 loop\n"
        "  8 sd\n"
        " 65 sd\n");
}

static size_t format_proc_interrupts(char *buf, size_t max)
{
    u64 ticks = sched_get_ticks();
    u32 cpu_count = smp_cpu_count();
    if (cpu_count == 0) cpu_count = 1;

    size_t off = 0;
    off += scnprintf(buf + off, max > off ? max - off : 0, "           ");
    for (u32 i = 0; i < cpu_count; i++) {
        off += scnprintf(buf + off, max > off ? max - off : 0, "CPU%u       ", i);
    }
    off += scnprintf(buf + off, max > off ? max - off : 0, "\n");

    off += scnprintf(buf + off, max > off ? max - off : 0,
        "  0: %10llu   IO-APIC   2-edge      timer\n"
        "  1: %10llu   IO-APIC   1-edge      i8042 (keyboard)\n"
        "  4: %10llu   IO-APIC   4-edge      ttyS0 (serial)\n"
        "  8: %10llu   IO-APIC   8-edge      rtc0\n"
        "  9: %10llu   IO-APIC   9-fasteoi   acpi\n"
        " 11: %10llu   IO-APIC  11-fasteoi   e1000, ac97, snd_hda_intel\n"
        " 14: %10llu   IO-APIC  14-edge      ata_piix\n"
        " 15: %10llu   IO-APIC  15-edge      ata_piix\n",
        (unsigned long long)ticks,
        (unsigned long long)(ticks / 10 + 15),
        (unsigned long long)(ticks / 5 + 42),
        (unsigned long long)(ticks / 100 + 1),
        (unsigned long long)1,
        (unsigned long long)(ticks / 8 + 128),
        (unsigned long long)(ticks / 20 + 3),
        (unsigned long long)0);
    return off;
}

static size_t format_proc_partitions(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "major minor  #blocks  name\n\n"
        "   8        0    2097152 sda\n"
        "   8        1    2096128 sda1\n"
        "   7        0     204800 loop0\n"
        "   1        0     204800 ram0\n");
}

static size_t format_proc_swaps(char *buf, size_t max)
{
    return (size_t)scnprintf(buf, max,
        "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
}

struct maps_ctx {
    char       *buf;
    size_t      max;
    size_t      off;
    const char *name;
};

static void maps_emit_line(struct maps_ctx *c, u64 start, u64 end,
                           char rd, char wr, char ex, char sh, const char *tag)
{
    if (c->off >= c->max) return;
    c->off += (size_t)scnprintf(c->buf + c->off, c->max - c->off,
        "%012llx-%012llx %c%c%c%c 00000000 00:00 0 %s\n",
        (unsigned long long)start, (unsigned long long)end,
        rd, wr, ex, sh, tag ? tag : "");
}

static void maps_emit_vma(const vm_area_t *v, void *pv)
{
    struct maps_ctx *c = (struct maps_ctx *)pv;
    const char *tag = "";
    if (v->flags & VMA_F_STACK)      tag = "[stack]";
    else if (v->flags & VMA_F_FILE)  tag = c->name;
    maps_emit_line(c,
        v->start, v->end,
        (v->prot & VMA_PROT_READ)  ? 'r' : '-',
        (v->prot & VMA_PROT_WRITE) ? 'w' : '-',
        (v->prot & VMA_PROT_EXEC)  ? 'x' : '-',
        (v->flags & VMA_F_SHARED)  ? 's' : 'p',
        tag);
}

static size_t format_pid_maps(u32 pid, char *buf, size_t max)
{
    char name[64] = "/bin/app.elf";
    struct maps_ctx c = { buf, max, 0, name };

    sched_lock();
    process_t *p = find_proc_by_pid(pid);
    if (p) {
        if (p->name[0]) {
            strncpy(name, p->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        u64 heap_s = p->heap_start, heap_e = p->heap_end;
        vma_for_each(p, maps_emit_vma, &c);      /* p stays valid under sched_lock */
        if (heap_e > heap_s)
            maps_emit_line(&c, heap_s, heap_e, 'r', 'w', '-', 'p', "[heap]");
    }
    sched_unlock();

    if (c.off == 0) c.off = (size_t)scnprintf(buf, max, "\n");
    return c.off;
}

static size_t format_pid_status(u32 pid, char *buf, size_t max)
{
    char name[32] = "process";
    char state_str[20] = "R (running)";
    u32 ppid = 0;
    u32 tracer_pid = 0;
    u32 thread_cnt = 0;
    u64 vmsize_kb = 64;
    bool found = false;

    sched_lock();
    process_t *p = find_proc_by_pid(pid);
    if (p) {
        found = true;
        if (p->name[0]) {
            strncpy(name, p->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        /* Linux distinguishes the two kinds of stop, and tools rely on it: a
         * job-control stop is 'T', a ptrace-stop is 't'. */
        if (p->stop_state == PROC_STOP_PTRACE) strcpy(state_str, "t (tracing stop)");
        else if (p->stop_state != PROC_STOP_NONE) strcpy(state_str, "T (stopped)");
        if (p->is_zombie) strcpy(state_str, "Z (zombie)");
        tracer_pid = p->tracer_pid;
        if (p->parent) ppid = p->parent->pid;
        for (thread_t *t = p->threads; t; t = t->proc_next) thread_cnt++;
        if (p->heap_end >= p->heap_start) {
            vmsize_kb = (p->heap_end - p->heap_start) / 1024 + 64;
        }
    }
    sched_unlock();

    if (!found) return (size_t)scnprintf(buf, max, "State: X (dead)\n");

    return (size_t)scnprintf(buf, max,
        "Name:   %s\n"
        "State:  %s\n"
        "Tgid:   %u\n"
        "Pid:    %u\n"
        "PPid:   %u\n"
        "TracerPid:%u\n"
        "Threads:%u\n"
        "VmSize: %8llu kB\n",
        name, state_str, pid, pid, ppid, tracer_pid, thread_cnt,
        (unsigned long long)vmsize_kb);
}

static size_t format_pid_cmdline(u32 pid, char *buf, size_t max)
{
    char name[32] = "";
    sched_lock();
    process_t *p = find_proc_by_pid(pid);
    if (p && p->name[0]) {
        strncpy(name, p->name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }
    sched_unlock();
    return (size_t)scnprintf(buf, max, "%s\n", name);
}

static size_t format_pid_stat(u32 pid, char *buf, size_t max)
{
    char name[32] = "app";
    char state = 'R';
    u32 ppid = 0;
    u64 minflt = 0, majflt = 0, utime = 0, stime = 0;
    bool found = false;

    sched_lock();
    process_t *p = find_proc_by_pid(pid);
    if (p) {
        found = true;
        if (p->name[0]) {
            strncpy(name, p->name, sizeof(name) - 1);
            name[sizeof(name) - 1] = '\0';
        }
        if (p->stop_state == PROC_STOP_PTRACE)    state = 't';
        else if (p->stop_state != PROC_STOP_NONE) state = 'T';
        if (p->is_zombie) state = 'Z';
        if (p->parent) ppid = p->parent->pid;
        minflt = p->nr_minor_faults;
        majflt = p->nr_major_faults;
        utime  = p->utime_ticks;
        stime  = p->stime_ticks;
    }
    sched_unlock();

    if (!found) return 0;

    /* Fields, in order: pid comm state ppid pgrp session tty tpgid flags
     * minflt cminflt majflt cmajflt utime stime — the ones this kernel tracks
     * are now real numbers instead of the zeros that used to fill them. */
    return (size_t)scnprintf(buf, max,
        "%u (%s) %c %u 0 0 0 0 0 %llu 0 %llu 0 %llu %llu "
        "0 0 20 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
        pid, name, state, ppid,
        (unsigned long long)minflt, (unsigned long long)majflt,
        (unsigned long long)utime,  (unsigned long long)stime);
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
        } else if (strcmp(name, "loadavg") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 107, S_IFREG | 0444, PROCFS_TYPE_LOADAVG, 0);
        } else if (strcmp(name, "mounts") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 108, S_IFREG | 0444, PROCFS_TYPE_MOUNTS, 0);
        } else if (strcmp(name, "filesystems") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 109, S_IFREG | 0444, PROCFS_TYPE_FILESYSTEMS, 0);
        } else if (strcmp(name, "cmdline") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 110, S_IFREG | 0444, PROCFS_TYPE_CMDLINE, 0);
        } else if (strcmp(name, "tcp") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 111, S_IFREG | 0444, PROCFS_TYPE_NET_TCP, 0);
        } else if (strcmp(name, "udp") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 112, S_IFREG | 0444, PROCFS_TYPE_NET_UDP, 0);
        } else if (strcmp(name, "devices") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 113, S_IFREG | 0444, PROCFS_TYPE_DEVICES, 0);
        } else if (strcmp(name, "interrupts") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 114, S_IFREG | 0444, PROCFS_TYPE_INTERRUPTS, 0);
        } else if (strcmp(name, "partitions") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 115, S_IFREG | 0444, PROCFS_TYPE_PARTITIONS, 0);
        } else if (strcmp(name, "swaps") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 116, S_IFREG | 0444, PROCFS_TYPE_SWAPS, 0);
        } else if (strcmp(name, "vulnerabilities") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 121, S_IFREG | 0444, PROCFS_TYPE_VULNERABILITIES, 0);
        } else if (strcmp(name, "security") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 122, S_IFREG | 0444, PROCFS_TYPE_SECURITY, 0);
        } else if (strcmp(name, "mcelog") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 123, S_IFREG | 0444, PROCFS_TYPE_MCELOG, 0);
        } else if (strcmp(name, "slabinfo") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 124, S_IFREG | 0444, PROCFS_TYPE_SLABINFO, 0);
        } else if (strcmp(name, "self") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 117, S_IFLNK | 0777, PROCFS_TYPE_SELF_SYMLINK, 0);
        } else if (strcmp(name, "sys") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 118, S_IFDIR | 0555, PROCFS_TYPE_SYS_DIR, 0);
        } else if (strcmp(name, "sysvipc") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 119, S_IFDIR | 0555, PROCFS_TYPE_SYSVIPC_DIR, 0);
        } else if (strcmp(name, "mqueues") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 120, S_IFREG | 0444, PROCFS_TYPE_MQUEUES, 0);
        } else {
            /* Check if numeric PID */
            bool is_num = true;
            u32 pid = 0;
            for (int i = 0; name[i]; i++) {
                if (name[i] < '0' || name[i] > '9') { is_num = false; break; }
                pid = pid * 10 + (u32)(name[i] - '0');
            }
            sched_lock();
            bool proc_exists = (find_proc_by_pid(pid) != NULL);
            sched_unlock();
            if (is_num && pid > 0 && proc_exists) {
                dentry->d_inode = procfs_alloc_inode(dir->i_sb, 1000 + pid, S_IFDIR | 0555, PROCFS_TYPE_PID_DIR, pid);
            }
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYSVIPC_DIR) {
        if (strcmp(name, "shm") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 320, S_IFREG | 0444, PROCFS_TYPE_SYSVIPC_SHM, 0);
        } else if (strcmp(name, "sem") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 321, S_IFREG | 0444, PROCFS_TYPE_SYSVIPC_SEM, 0);
        } else if (strcmp(name, "msg") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 322, S_IFREG | 0444, PROCFS_TYPE_SYSVIPC_MSG, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_DIR) {
        if (strcmp(name, "kernel") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 300, S_IFDIR | 0555, PROCFS_TYPE_SYS_KERNEL_DIR, 0);
        } else if (strcmp(name, "fs") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 301, S_IFDIR | 0555, PROCFS_TYPE_SYS_FS_DIR, 0);
        } else if (strcmp(name, "net") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 302, S_IFDIR | 0555, PROCFS_TYPE_SYS_NET_DIR, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_KERNEL_DIR) {
        if (strcmp(name, "osrelease") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 310, S_IFREG | 0444, PROCFS_TYPE_SYS_OSRELEASE, 0);
        } else if (strcmp(name, "ostype") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 311, S_IFREG | 0444, PROCFS_TYPE_SYS_OSTYPE, 0);
        } else if (strcmp(name, "hostname") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 312, S_IFREG | 0644, PROCFS_TYPE_SYS_HOSTNAME, 0);
        } else if (strcmp(name, "version") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 313, S_IFREG | 0444, PROCFS_TYPE_SYS_VERSION, 0);
        } else if (strcmp(name, "pid_max") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 314, S_IFREG | 0644, PROCFS_TYPE_SYS_PID_MAX, 0);
        } else if (strcmp(name, "random") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 315, S_IFDIR | 0555, PROCFS_TYPE_SYS_KERNEL_RANDOM_DIR, 0);
        } else if (strcmp(name, "dmesg_restrict") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 340, S_IFREG | 0644, PROCFS_TYPE_SYS_KERNEL_DMESG_RESTRICT, 0);
        } else if (strcmp(name, "kptr_restrict") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 341, S_IFREG | 0644, PROCFS_TYPE_SYS_KERNEL_KPTR_RESTRICT, 0);
        } else if (strcmp(name, "mmap_min_addr") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 342, S_IFREG | 0644, PROCFS_TYPE_SYS_KERNEL_MMAP_MIN_ADDR, 0);
        } else if (strcmp(name, "yama") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 343, S_IFDIR | 0555, PROCFS_TYPE_SYS_KERNEL_YAMA_DIR, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_KERNEL_YAMA_DIR) {
        if (strcmp(name, "ptrace_scope") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 344, S_IFREG | 0644, PROCFS_TYPE_SYS_KERNEL_YAMA_PTRACE_SCOPE, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_KERNEL_RANDOM_DIR) {
        if (strcmp(name, "boot_id") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 316, S_IFREG | 0444, PROCFS_TYPE_SYS_BOOT_ID, 0);
        } else if (strcmp(name, "uuid") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 317, S_IFREG | 0444, PROCFS_TYPE_SYS_UUID, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_FS_DIR) {
        if (strcmp(name, "file-max") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 320, S_IFREG | 0444, PROCFS_TYPE_SYS_FILEMAX, 0);
        } else if (strcmp(name, "inotify") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 321, S_IFDIR | 0555, PROCFS_TYPE_SYS_FS_INOTIFY_DIR, 0);
        } else if (strcmp(name, "protected_hardlinks") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 345, S_IFREG | 0644, PROCFS_TYPE_SYS_FS_PROTECTED_HARDLINKS, 0);
        } else if (strcmp(name, "protected_symlinks") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 346, S_IFREG | 0644, PROCFS_TYPE_SYS_FS_PROTECTED_SYMLINKS, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_FS_INOTIFY_DIR) {
        if (strcmp(name, "max_user_watches") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 322, S_IFREG | 0644, PROCFS_TYPE_SYS_INOTIFY_MAX_WATCHES, 0);
        } else if (strcmp(name, "max_user_instances") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 323, S_IFREG | 0644, PROCFS_TYPE_SYS_INOTIFY_MAX_INSTANCES, 0);
        } else if (strcmp(name, "max_queued_events") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 324, S_IFREG | 0644, PROCFS_TYPE_SYS_INOTIFY_MAX_EVENTS, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_NET_DIR) {
        if (strcmp(name, "ipv4") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 330, S_IFDIR | 0555, PROCFS_TYPE_SYS_NET_IPV4_DIR, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_SYS_NET_IPV4_DIR) {
        if (strcmp(name, "ip_forward") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 331, S_IFREG | 0644, PROCFS_TYPE_SYS_NET_IP_FORWARD, 0);
        } else if (strcmp(name, "tcp_syncookies") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 332, S_IFREG | 0644, PROCFS_TYPE_SYS_NET_TCP_SYNCOOKIES, 0);
        } else if (strcmp(name, "tcp_fin_timeout") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 333, S_IFREG | 0644, PROCFS_TYPE_SYS_NET_TCP_FIN_TIMEOUT, 0);
        }
    } else if (dir_priv->type == PROCFS_TYPE_PID_DIR) {
        u32 pid = dir_priv->pid;
        if (strcmp(name, "status") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 1, S_IFREG | 0444, PROCFS_TYPE_PID_STATUS, pid);
        } else if (strcmp(name, "cmdline") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 2, S_IFREG | 0444, PROCFS_TYPE_PID_CMDLINE, pid);
        } else if (strcmp(name, "stat") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 3, S_IFREG | 0444, PROCFS_TYPE_PID_STAT, pid);
        } else if (strcmp(name, "maps") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 4, S_IFREG | 0444, PROCFS_TYPE_PID_MAPS, pid);
        } else if (strcmp(name, "exe") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 5, S_IFLNK | 0777, PROCFS_TYPE_PID_EXE_SYMLINK, pid);
        } else if (strcmp(name, "cwd") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 6, S_IFLNK | 0777, PROCFS_TYPE_PID_CWD_SYMLINK, pid);
        } else if (strcmp(name, "fd") == 0) {
            dentry->d_inode = procfs_alloc_inode(dir->i_sb, 2000 + pid * 10 + 7, S_IFDIR | 0555, PROCFS_TYPE_PID_FD_DIR, pid);
        }
    } else if (dir_priv->type == PROCFS_TYPE_PID_FD_DIR) {
        u32 pid = dir_priv->pid;
        bool is_num = true;
        u32 fd_num = 0;
        for (int i = 0; name[i]; i++) {
            if (name[i] < '0' || name[i] > '9') { is_num = false; break; }
            fd_num = fd_num * 10 + (u32)(name[i] - '0');
            /* Stop before the multiply can wrap: "4294967296" wrapped to 0 and
             * the range check below then accepted it as descriptor 0. */
            if (fd_num >= PROC_MAX_FDS) { is_num = false; break; }
        }
        if (is_num && fd_num < PROC_MAX_FDS) {
            sched_lock();
            process_t *p = find_proc_by_pid(pid);
            bool fd_valid = (p && p->handle_table[fd_num] != NULL);
            if (fd_num == 0 || fd_num == 1 || fd_num == 2) fd_valid = true;
            sched_unlock();
            if (fd_valid) {
                dentry->d_inode = procfs_alloc_fd_inode(dir->i_sb, 5000 + pid * 100 + fd_num, pid, fd_num);
            }
        }
    }

    return dentry;
}

/* --------------------------------------------------------------------------
 * File Reading
 * -------------------------------------------------------------------------- */

/* Working buffer for the synthesised files. Sized so /proc/cpuinfo still fits
 * on a large SMP box rather than truncating mid-record. */
#define PROCFS_TMP_SIZE 16384

static s64 procfs_file_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    if (!filp || !buf || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)filp->f_inode->i_private;

    if (priv->type == PROCFS_TYPE_DMESG) {
        return console_read_klog(buf, len, offset);
    }

    /* Formatted on the heap, not the stack: /proc/cpuinfo alone now runs to
     * several hundred bytes per CPU, and a buffer big enough for a many-core
     * machine has no business sitting in a 16 KB ring-0 stack frame. */
    char *tmp = (char *)kmalloc(PROCFS_TMP_SIZE);
    if (!tmp) return -(s64)ENOMEM;
    size_t total_len = 0;

    switch (priv->type) {
    case PROCFS_TYPE_VERSION:
        total_len = format_proc_version(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_UPTIME:
        total_len = format_proc_uptime(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_MEMINFO:
        total_len = format_proc_meminfo(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_CPUINFO:
        total_len = format_proc_cpuinfo(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_VULNERABILITIES:
        total_len = format_proc_vulnerabilities(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SECURITY:
        total_len = format_proc_security(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_MCELOG:
        total_len = format_proc_mcelog(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SLABINFO:
        total_len = format_proc_slabinfo(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_STAT:
        total_len = format_proc_stat(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_NET_DEV:
        total_len = format_proc_net_dev(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_LOADAVG:
        total_len = format_proc_loadavg(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_MOUNTS:
        total_len = format_proc_mounts(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SYSVIPC_SHM:
        total_len = (size_t)sysvipc_proc_shm(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SYSVIPC_SEM:
        total_len = (size_t)sysvipc_proc_sem(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SYSVIPC_MSG:
        total_len = (size_t)sysvipc_proc_msg(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_MQUEUES:
        total_len = mqueue_format_proc(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_FILESYSTEMS:
        total_len = format_proc_filesystems(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_CMDLINE:
        total_len = format_proc_cmdline(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_NET_TCP:
        total_len = format_proc_net_tcp(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_NET_UDP:
        total_len = format_proc_net_udp(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_DEVICES:
        total_len = format_proc_devices(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_INTERRUPTS:
        total_len = format_proc_interrupts(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_PARTITIONS:
        total_len = format_proc_partitions(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SWAPS:
        total_len = format_proc_swaps(tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_SYS_OSRELEASE:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "7.0.0-posix\n");
        break;
    case PROCFS_TYPE_SYS_OSTYPE:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "AzamiOS\n");
        break;
    case PROCFS_TYPE_SYS_HOSTNAME:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "azami\n");
        break;
    case PROCFS_TYPE_SYS_VERSION:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "#1 SMP Limine 2026\n");
        break;
    case PROCFS_TYPE_SYS_FILEMAX:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "65536\n");
        break;
    case PROCFS_TYPE_SYS_PID_MAX:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "32768\n");
        break;
    case PROCFS_TYPE_SYS_BOOT_ID:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "e74c9d86-8125-49fa-96a0-c620b1846058\n");
        break;
    case PROCFS_TYPE_SYS_UUID:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "7f2c4a91-d309-41e2-b883-fa919f18a201\n");
        break;
    case PROCFS_TYPE_SYS_INOTIFY_MAX_WATCHES:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "8192\n");
        break;
    case PROCFS_TYPE_SYS_INOTIFY_MAX_INSTANCES:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "128\n");
        break;
    case PROCFS_TYPE_SYS_INOTIFY_MAX_EVENTS:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "16384\n");
        break;
    case PROCFS_TYPE_SYS_NET_IP_FORWARD:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "1\n");
        break;
    case PROCFS_TYPE_SYS_NET_TCP_SYNCOOKIES:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "1\n");
        break;
    case PROCFS_TYPE_SYS_NET_TCP_FIN_TIMEOUT:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "60\n");
        break;
    case PROCFS_TYPE_SYS_KERNEL_DMESG_RESTRICT:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%u\n", (unsigned int)g_dmesg_restrict);
        break;
    case PROCFS_TYPE_SYS_KERNEL_KPTR_RESTRICT:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%u\n", (unsigned int)g_kptr_restrict);
        break;
    case PROCFS_TYPE_SYS_KERNEL_MMAP_MIN_ADDR:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%llu\n", (unsigned long long)g_mmap_min_addr);
        break;
    case PROCFS_TYPE_SYS_KERNEL_YAMA_PTRACE_SCOPE:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%u\n", (unsigned int)g_yama_ptrace_scope);
        break;
    case PROCFS_TYPE_SYS_FS_PROTECTED_HARDLINKS:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%u\n", (unsigned int)g_protected_hardlinks);
        break;
    case PROCFS_TYPE_SYS_FS_PROTECTED_SYMLINKS:
        total_len = (size_t)scnprintf(tmp, PROCFS_TMP_SIZE, "%u\n", (unsigned int)g_protected_symlinks);
        break;
    case PROCFS_TYPE_PID_STATUS:
        total_len = format_pid_status(priv->pid, tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_PID_CMDLINE:
        total_len = format_pid_cmdline(priv->pid, tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_PID_STAT:
        total_len = format_pid_stat(priv->pid, tmp, PROCFS_TMP_SIZE);
        break;
    case PROCFS_TYPE_PID_MAPS:
        total_len = format_pid_maps(priv->pid, tmp, PROCFS_TMP_SIZE);
        break;
    default:
        kfree(tmp);
        return 0;
    }

    /* Every formatter above reports how much it wrote, but clamp anyway: a
     * length that exceeded the buffer would turn the memcpy() below into a
     * read past `tmp` and leak adjacent kernel stack to userspace. */
    if (total_len > PROCFS_TMP_SIZE) total_len = PROCFS_TMP_SIZE;

    if (*offset >= total_len) { kfree(tmp); return 0; }  /* EOF */

    size_t avail = total_len - (size_t)*offset;
    size_t copy_cnt = (len < avail) ? len : avail;
    memcpy(buf, tmp + *offset, copy_cnt);
    *offset += copy_cnt;

    kfree(tmp);
    return (s64)copy_cnt;
}

static s64 procfs_file_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !buf || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)filp->f_inode->i_private;
    process_t *proc = sched_current_process();
    if (!proc || (proc->euid != 0 && !security_check_permission(proc, CAP_SYS_ADMIN))) {
        return -(s64)EPERM;
    }

    if (len == 0) return 0;

    char kbuf[64];
    size_t copy_sz = (len < sizeof(kbuf) - 1) ? len : sizeof(kbuf) - 1;
    memcpy(kbuf, buf, copy_sz);
    kbuf[copy_sz] = '\0';

    size_t i = 0;
    while (kbuf[i] == ' ' || kbuf[i] == '\t' || kbuf[i] == '\r' || kbuf[i] == '\n') i++;
    u64 val = 0;
    while (kbuf[i] >= '0' && kbuf[i] <= '9') {
        val = val * 10 + (kbuf[i] - '0');
        i++;
    }

    switch (priv->type) {
    case PROCFS_TYPE_SYS_KERNEL_DMESG_RESTRICT:
        g_dmesg_restrict = (u32)val;
        return (s64)len;
    case PROCFS_TYPE_SYS_KERNEL_KPTR_RESTRICT:
        g_kptr_restrict = (u32)val;
        return (s64)len;
    case PROCFS_TYPE_SYS_KERNEL_MMAP_MIN_ADDR:
        g_mmap_min_addr = val;
        return (s64)len;
    case PROCFS_TYPE_SYS_KERNEL_YAMA_PTRACE_SCOPE:
        if (val <= 3) {
            g_yama_ptrace_scope = (u32)val;
            return (s64)len;
        }
        return -(s64)EINVAL;
    case PROCFS_TYPE_SYS_FS_PROTECTED_HARDLINKS:
        g_protected_hardlinks = (u32)val;
        return (s64)len;
    case PROCFS_TYPE_SYS_FS_PROTECTED_SYMLINKS:
        g_protected_symlinks = (u32)val;
        return (s64)len;
    default:
        return -(s64)EACCES;
    }
}

/* --------------------------------------------------------------------------
 * Directory Readdir
 * -------------------------------------------------------------------------- */

static const char *g_static_root_entries[] = {
    "version", "uptime", "meminfo", "cpuinfo", "stat", "dmesg", "net",
    "loadavg", "mounts", "filesystems", "cmdline", "tcp", "udp",
    "devices", "interrupts", "partitions", "swaps", "self", "sys", "sysvipc",
    "mqueues", "vulnerabilities", "security", "mcelog", "slabinfo"
};
#define NUM_ROOT_ENTRIES (sizeof(g_static_root_entries) / sizeof(g_static_root_entries[0]))

static s64 procfs_dir_readdir(struct file *filp, void *dirent_buf, size_t len, u64 *offset)
{
    if (!filp || !dirent_buf || len == 0 || !offset || !filp->f_inode || !filp->f_inode->i_private)
        return -(s64)EINVAL;

    procfs_priv_t *priv = (procfs_priv_t *)filp->f_inode->i_private;
    size_t written = 0;
    u8 *out_ptr = (u8 *)dirent_buf;
    u64 idx = *offset;

    if (priv->type == PROCFS_TYPE_ROOT_DIR) {
        u32 pids[64];
        u32 pid_count = 0;

        sched_lock();
        process_t *p = sched_get_process_list();
        while (p && pid_count < 64) {
            pids[pid_count++] = p->pid;
            p = p->next;
        }
        sched_unlock();

        u64 total_entries = 2 + NUM_ROOT_ENTRIES + pid_count;

        while (idx < total_entries) {
            const char *name = NULL;
            char pid_name[16];
            u8 dtype = DT_REG;

            if (idx == 0) {
                name = "."; dtype = DT_DIR;
            } else if (idx == 1) {
                name = ".."; dtype = DT_DIR;
            } else if (idx - 2 < NUM_ROOT_ENTRIES) {
                name = g_static_root_entries[idx - 2];
                if (strcmp(name, "sys") == 0) dtype = DT_DIR;
                else if (strcmp(name, "self") == 0) dtype = DT_LNK;
                else dtype = DT_REG;
            } else {
                u32 pidx = (u32)(idx - 2 - NUM_ROOT_ENTRIES);
                scnprintf(pid_name, sizeof(pid_name), "%u", pids[pidx]);
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
    } else if (priv->type == PROCFS_TYPE_SYSVIPC_DIR) {
        const char *sys_entries[] = { ".", "..", "shm", "sem", "msg" };
        u64 total_entries = 5;
        while (idx < total_entries) {
            const char *name = sys_entries[idx];
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
    } else if (priv->type == PROCFS_TYPE_SYS_DIR) {
        const char *sys_entries[] = { ".", "..", "kernel", "fs", "net" };
        u64 total_entries = 5;
        while (idx < total_entries) {
            const char *name = sys_entries[idx];
            u8 dtype = DT_DIR;
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
    } else if (priv->type == PROCFS_TYPE_SYS_KERNEL_DIR) {
        const char *kentries[] = { ".", "..", "osrelease", "ostype", "hostname", "version", "pid_max", "random", "dmesg_restrict", "kptr_restrict", "mmap_min_addr", "yama" };
        u64 total_entries = 12;
        while (idx < total_entries) {
            const char *name = kentries[idx];
            u8 dtype = (idx < 2 || strcmp(name, "random") == 0 || strcmp(name, "yama") == 0) ? DT_DIR : DT_REG;
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
    } else if (priv->type == PROCFS_TYPE_SYS_KERNEL_YAMA_DIR) {
        const char *yentries[] = { ".", "..", "ptrace_scope" };
        u64 total_entries = 3;
        while (idx < total_entries) {
            const char *name = yentries[idx];
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
    } else if (priv->type == PROCFS_TYPE_SYS_KERNEL_RANDOM_DIR) {
        const char *rentries[] = { ".", "..", "boot_id", "uuid" };
        u64 total_entries = 4;
        while (idx < total_entries) {
            const char *name = rentries[idx];
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
    } else if (priv->type == PROCFS_TYPE_SYS_FS_DIR) {
        const char *fsentries[] = { ".", "..", "file-max", "inotify", "protected_hardlinks", "protected_symlinks" };
        u64 total_entries = 6;
        while (idx < total_entries) {
            const char *name = fsentries[idx];
            u8 dtype = (idx < 2 || strcmp(name, "inotify") == 0) ? DT_DIR : DT_REG;
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
    } else if (priv->type == PROCFS_TYPE_SYS_FS_INOTIFY_DIR) {
        const char *in_entries[] = { ".", "..", "max_user_watches", "max_user_instances", "max_queued_events" };
        u64 total_entries = 5;
        while (idx < total_entries) {
            const char *name = in_entries[idx];
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
    } else if (priv->type == PROCFS_TYPE_SYS_NET_DIR) {
        const char *net_entries[] = { ".", "..", "ipv4" };
        u64 total_entries = 3;
        while (idx < total_entries) {
            const char *name = net_entries[idx];
            u8 dtype = DT_DIR;
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
    } else if (priv->type == PROCFS_TYPE_SYS_NET_IPV4_DIR) {
        const char *ipv4_entries[] = { ".", "..", "ip_forward", "tcp_syncookies", "tcp_fin_timeout" };
        u64 total_entries = 5;
        while (idx < total_entries) {
            const char *name = ipv4_entries[idx];
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
    } else if (priv->type == PROCFS_TYPE_PID_DIR) {
        const char *pid_entries[] = { ".", "..", "status", "cmdline", "stat", "maps", "exe", "cwd", "fd" };
        u64 total_entries = 9;

        while (idx < total_entries) {
            const char *name = pid_entries[idx];
            u8 dtype = DT_REG;
            if (idx < 2 || strcmp(name, "fd") == 0) dtype = DT_DIR;
            else if (strcmp(name, "exe") == 0 || strcmp(name, "cwd") == 0) dtype = DT_LNK;

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
    } else if (priv->type == PROCFS_TYPE_PID_FD_DIR) {
        u32 active_fds[PROC_MAX_FDS];
        u32 fd_cnt = 0;
        sched_lock();
        process_t *p = find_proc_by_pid(priv->pid);
        if (p) {
            for (u32 i = 0; i < PROC_MAX_FDS; i++) {
                if (p->handle_table[i] || i < 3) {
                    active_fds[fd_cnt++] = i;
                }
            }
        }
        sched_unlock();

        u64 total_entries = 2 + fd_cnt;
        while (idx < total_entries) {
            const char *name = NULL;
            char fd_str[16];
            u8 dtype = DT_LNK;

            if (idx == 0) {
                name = "."; dtype = DT_DIR;
            } else if (idx == 1) {
                name = ".."; dtype = DT_DIR;
            } else {
                scnprintf(fd_str, sizeof(fd_str), "%u", active_fds[idx - 2]);
                name = fd_str;
                dtype = DT_LNK;
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
