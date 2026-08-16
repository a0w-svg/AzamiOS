/* ============================================================================
 * AzamiOS — Sound Subsystem Core Implementation
 * File: drivers/sound/sound.c
 * ============================================================================ */

#define DEBUG 1
#define DEBUG 1
#include "../../include/azami/debug.h"
#include "sound.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/syscall/syscall.h"
#include "../../fs/vfs.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static sound_device_t *g_active_sound_dev = NULL;
static spinlock_t g_sound_lock = SPINLOCK_INIT;

static s64 dev_dsp_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    
    irqflags_t irqf = spinlock_lock_irqsave(&g_sound_lock);
    sound_device_t *dev = g_active_sound_dev;
    spinlock_unlock_irqrestore(&g_sound_lock, irqf);

    if (!dev || !dev->ops || !dev->ops->write_pcm) {
        return -1;
    }

    return dev->ops->write_pcm((const u8 *)buf, len);
}

static s64 dev_dsp_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    irqflags_t irqf = spinlock_lock_irqsave(&g_sound_lock);
    sound_device_t *dev = g_active_sound_dev;
    spinlock_unlock_irqrestore(&g_sound_lock, irqf);

    if (!dev || !dev->ops || !dev->ops->ioctl) {
        return -1;
    }

    return dev->ops->ioctl((u64)cmd, (void *)(uintptr_t)arg);
}

static file_operations_t g_dsp_fops = {
    .read = NULL,
    .write = dev_dsp_write,
    .readdir = NULL,
    .ioctl = dev_dsp_ioctl,
    .mmap = NULL,
    .open = NULL,
    .release = NULL
};

void sound_register_device(sound_device_t *dev)
{
    spinlock_lock(&g_sound_lock);
    if (!g_active_sound_dev) {
        g_active_sound_dev = dev;
        
        devfs_register_device("dsp", &g_dsp_fops, dev);
        pr_debug("[SOUND] Registered %s as /dev/dsp\n", dev->name);
    }
    spinlock_unlock(&g_sound_lock);
}
