/* ============================================================================
 * AzamiOS — Loopback Block Device Driver
 * File: drivers/block/loop.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "loop.h"
#include "block.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/syscall/syscall.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"

#define MAX_LOOP_DEVS 8

typedef struct loop_device {
    block_dev_t bdev;
    file_t *backing_file;
    struct loop_info64 info;
    spinlock_t lock;
    bool is_bound;
} loop_device_t;

static loop_device_t g_loop_devs[MAX_LOOP_DEVS];
static spinlock_t g_loop_ctrl_lock = SPINLOCK_INIT;

static s64 loop_read_sectors(struct block_dev *dev, u64 lba, u32 count, void *buf)
{
    loop_device_t *ld = (loop_device_t *)dev->driver_data;
    if (!ld || !ld->is_bound || !ld->backing_file) return -1;

    u64 byte_offset = ld->info.lo_offset + (lba * dev->sector_size);
    u64 bytes_to_read = (u64)count * dev->sector_size;

    spinlock_lock(&ld->lock);
    u64 saved_pos = ld->backing_file->f_pos;
    ld->backing_file->f_pos = byte_offset;
    s64 nread = vfs_read(ld->backing_file, buf, bytes_to_read);
    ld->backing_file->f_pos = saved_pos;
    spinlock_unlock(&ld->lock);

    return nread;
}

static s64 loop_write_sectors(struct block_dev *dev, u64 lba, u32 count, const void *buf)
{
    loop_device_t *ld = (loop_device_t *)dev->driver_data;
    if (!ld || !ld->is_bound || !ld->backing_file) return -1;
    if (ld->info.lo_flags & LO_FLAGS_READ_ONLY) return -1;

    u64 byte_offset = ld->info.lo_offset + (lba * dev->sector_size);
    u64 bytes_to_write = (u64)count * dev->sector_size;

    spinlock_lock(&ld->lock);
    u64 saved_pos = ld->backing_file->f_pos;
    ld->backing_file->f_pos = byte_offset;
    s64 nwritten = vfs_write(ld->backing_file, buf, bytes_to_write);
    ld->backing_file->f_pos = saved_pos;
    spinlock_unlock(&ld->lock);

    return nwritten;
}

static block_ops_t g_loop_ops = {
    .read_sectors = loop_read_sectors,
    .write_sectors = loop_write_sectors,
};

static s64 loop_fops_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    block_dev_t *bdev = (block_dev_t *)filp->private_data;
    if (!bdev) return -(s64)EINVAL;
    loop_device_t *ld = (loop_device_t *)bdev->driver_data;
    if (!ld) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    switch (cmd) {
    case LOOP_SET_FD: {
        int fd = (int)arg;
        if (fd < 0 || fd >= 64 || !proc->handle_table[fd]) return -(s64)EBADF;

        spinlock_lock(&ld->lock);
        if (ld->is_bound) {
            spinlock_unlock(&ld->lock);
            return -(s64)EBUSY;
        }

        file_t *file = (file_t *)proc->handle_table[fd];
        ld->backing_file = file;
        ld->is_bound = true;
        ld->info.lo_offset = 0;
        ld->info.lo_sizelimit = 0;
        ld->info.lo_flags = 0;

        if (file->f_inode) {
            ld->bdev.sector_count = file->f_inode->i_size / ld->bdev.sector_size;
            ld->info.lo_inode = file->f_inode->i_ino;
        } else {
            ld->bdev.sector_count = 0;
        }

        spinlock_unlock(&ld->lock);
        pr_debug("[LOOP] %s bound to fd %d (%llu sectors)\n",
                 ld->bdev.name, fd, (unsigned long long)ld->bdev.sector_count);
        return 0;
    }

    case LOOP_CLR_FD: {
        spinlock_lock(&ld->lock);
        if (!ld->is_bound) {
            spinlock_unlock(&ld->lock);
            return -(s64)EINVAL;
        }
        ld->backing_file = NULL;
        ld->is_bound = false;
        ld->bdev.sector_count = 0;
        memset(&ld->info, 0, sizeof(ld->info));
        spinlock_unlock(&ld->lock);
        pr_debug("[LOOP] %s unbound\n", ld->bdev.name);
        return 0;
    }

    case LOOP_GET_STATUS64: {
        if (!arg || arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        spinlock_lock(&ld->lock);
        if (!ld->is_bound) {
            spinlock_unlock(&ld->lock);
            return -(s64)ENXIO;
        }
        struct loop_info64 kinfo = ld->info;
        spinlock_unlock(&ld->lock);

        if (copy_to_user((void *)arg, &kinfo, sizeof(kinfo)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case LOOP_SET_STATUS64: {
        if (!arg || arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
        struct loop_info64 kinfo;
        if (copy_from_user(&kinfo, (const void *)arg, sizeof(kinfo)) != 0)
            return -(s64)EFAULT;

        spinlock_lock(&ld->lock);
        if (!ld->is_bound) {
            spinlock_unlock(&ld->lock);
            return -(s64)ENXIO;
        }
        ld->info.lo_offset = kinfo.lo_offset;
        ld->info.lo_sizelimit = kinfo.lo_sizelimit;
        ld->info.lo_flags = kinfo.lo_flags;
        strncpy((char *)ld->info.lo_file_name, (const char *)kinfo.lo_file_name, LO_NAME_SIZE - 1);
        spinlock_unlock(&ld->lock);
        return 0;
    }

    case LOOP_SET_CAPACITY: {
        spinlock_lock(&ld->lock);
        if (ld->is_bound && ld->backing_file && ld->backing_file->f_inode) {
            ld->bdev.sector_count = ld->backing_file->f_inode->i_size / ld->bdev.sector_size;
        }
        spinlock_unlock(&ld->lock);
        return 0;
    }

    default:
        return -(s64)EINVAL;
    }
}

static s64 loop_ctrl_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    (void)arg;
    if (cmd == LOOP_CTL_GET_FREE) {
        spinlock_lock(&g_loop_ctrl_lock);
        for (int i = 0; i < MAX_LOOP_DEVS; i++) {
            if (!g_loop_devs[i].is_bound) {
                spinlock_unlock(&g_loop_ctrl_lock);
                return (s64)i;
            }
        }
        spinlock_unlock(&g_loop_ctrl_lock);
        return -(s64)ENOSPC;
    }
    return -(s64)EINVAL;
}

static file_operations_t g_loop_fops = {
    .ioctl = loop_fops_ioctl,
};

static file_operations_t g_loop_ctrl_fops = {
    .ioctl = loop_ctrl_ioctl,
};

void loop_init(void)
{
    for (int i = 0; i < MAX_LOOP_DEVS; i++) {
        loop_device_t *ld = &g_loop_devs[i];
        memset(ld, 0, sizeof(*ld));
        ld->lock = (spinlock_t)SPINLOCK_INIT;
        snprintf(ld->bdev.name, sizeof(ld->bdev.name), "loop%d", i);
        ld->bdev.sector_size = 512;
        ld->bdev.sector_count = 0;
        ld->bdev.ops = &g_loop_ops;
        ld->bdev.driver_data = ld;
        ld->info.lo_number = (u32)i;

        block_dev_register(&ld->bdev);
        devfs_register_device(ld->bdev.name, &g_loop_fops, &ld->bdev);
    }

    devfs_register_device("loop-control", &g_loop_ctrl_fops, NULL);
    pr_debug("[LOOP] Initialized 8 loopback block devices (/dev/loop0..7, /dev/loop-control)\n");
}
