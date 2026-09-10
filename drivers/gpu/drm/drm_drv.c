/* ============================================================================
 * AzamiOS — DRM: card registration and the /dev/dri nodes
 * File: drivers/gpu/drm/drm_drv.c
 *
 * Owns the device-node side of DRM: allocating cards, running driver->load(),
 * publishing /dev/dri/cardN and /dev/dri/renderD(128+N), and the file
 * operations behind them — open/release, ioctl (handed straight to
 * drm_ioctl.c), mmap of GEM objects, and the event stream that page-flip
 * clients read completion records from.
 *
 * Two node kinds share one file_operations table, distinguished by the minor
 * record stashed in the inode: a card node can become DRM master and drive
 * KMS, a render node can only allocate and share buffers.
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "drm_core.h"
#include "../../../fs/vfs.h"
#include "../../../kernel/mm/kmalloc.h"
#include "../../../kernel/lib/string.h"
#include "../../../kernel/sched/sched.h"
#include "../../../arch/x86_64/mm/vmm.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/syscall/syscall.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);
extern void console_disable_fb(void);

/* Linux's DRM character major, so /sys/class/drm reports familiar numbers. */
#define DRM_MAJOR            226
#define DRM_RENDER_MINOR_BASE 128

/* Identifies which node an open file came through. */
typedef struct drm_minor {
    drm_device_t *dev;
    bool          is_render;
} drm_minor_t;

static drm_device_t *g_drm_devices;
static u32           g_drm_card_count;
static dm_class_t    g_drm_class = { .name = "drm" };

dm_class_t *drm_class(void) { return &g_drm_class; }

drm_device_t *drm_dev_nth(u32 n)
{
    u32 i = 0;
    for (drm_device_t *d = g_drm_devices; d; d = d->next, i++) {
        if (i == n) return d;
    }
    return NULL;
}

u32 drm_dev_count(void) { return g_drm_card_count; }

/* ── Event queue ─────────────────────────────────────────────────────────── */

void drm_send_event(drm_file_t *file, drm_crtc_t *crtc, u32 type,
                    u64 user_data, u64 timestamp_ns, u64 sequence)
{
    if (!file || !crtc) return;
    drm_device_t *dev = crtc->dev;

    spinlock_lock(&dev->lock);
    u32 next = (file->event_head + 1) % DRM_MAX_EVENTS;
    /* A client that never drains its events must not stall the compositor;
     * the oldest record is dropped instead. */
    if (next == file->event_tail) {
        file->event_tail = (file->event_tail + 1) % DRM_MAX_EVENTS;
    }

    drm_pending_event_t *pe = &file->events[file->event_head];
    memset(pe, 0, sizeof(*pe));
    pe->ev.base.type   = type;
    pe->ev.base.length = sizeof(struct drm_event_vblank);
    pe->ev.user_data   = user_data;
    pe->ev.sequence    = (u32)sequence;
    pe->ev.crtc_id     = crtc->base.id;
    pe->ev.tv_sec      = (u32)(timestamp_ns / 1000000000ULL);
    pe->ev.tv_usec     = (u32)((timestamp_ns % 1000000000ULL) / 1000ULL);
    pe->valid          = true;

    file->event_head = next;
    spinlock_unlock(&dev->lock);

    /* Whoever is blocked in read() or poll() on this fd is waiting for
     * exactly this. */
    drm_wake(file);
}

/* ── PRIME: dma-buf file descriptors ─────────────────────────────────────── */

typedef struct drm_dmabuf {
    drm_device_t     *dev;
    drm_gem_object_t *obj;
} drm_dmabuf_t;

static s64 drm_dmabuf_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    if (!filp || !filp->private_data) return 0;
    drm_dmabuf_t *db = (drm_dmabuf_t *)filp->private_data;

    drm_gem_object_put(db->dev, db->obj);
    kfree(db);
    filp->private_data = NULL;
    return 0;
}

static file_operations_t g_dmabuf_fops = {
    .release = drm_dmabuf_release,
};

int drm_prime_export_fd(drm_device_t *dev, drm_gem_object_t *obj)
{
    process_t *proc = sched_current_process();
    if (!proc) return -EPERM;

    drm_dmabuf_t *db = (drm_dmabuf_t *)kzalloc(sizeof(drm_dmabuf_t));
    if (!db) return -ENOMEM;

    file_t *filp = (file_t *)kzalloc(sizeof(file_t));
    if (!filp) { kfree(db); return -ENOMEM; }

    db->dev = dev;
    db->obj = obj;

    filp->f_op         = &g_dmabuf_fops;
    filp->private_data = db;
    filp->f_flags      = O_RDWR;
    filp->f_count      = 1;

    /* Install through the syscall layer's helper rather than scanning
     * handle_table[] and assigning into it directly. That open-coded version
     * ran without g_fd_lock, so two threads exporting at once picked the same
     * slot: one file_t was overwritten and leaked along with its GEM
     * reference, and a close() racing the scan could free the slot's previous
     * occupant underneath it. */
    s64 fd = syscall_install_fd(proc, filp, 0);
    if (fd < 0) {
        kfree(filp);
        kfree(db);
        return (int)fd;
    }

    /* Take the GEM reference only once the fd is committed — an early
     * reference on the -EMFILE path was never dropped. */
    drm_gem_object_get(obj);
    return (int)fd;
}

drm_gem_object_t *drm_prime_import_fd(drm_device_t *dev, int fd)
{
    process_t *proc = sched_current_process();
    if (!proc || fd < 0 || fd >= PROC_MAX_FDS) return NULL;

    file_t *filp = (file_t *)proc->handle_table[fd];
    if (!filp || filp->f_op != &g_dmabuf_fops || !filp->private_data) return NULL;

    drm_dmabuf_t *db = (drm_dmabuf_t *)filp->private_data;
    /* Importing across cards would need a real dma-buf attachment layer. */
    if (db->dev != dev) return NULL;
    return db->obj;
}

/* ── File operations ─────────────────────────────────────────────────────── */

static s64 drm_open(inode_t *inode, file_t *filp)
{
    drm_minor_t *minor = inode ? (drm_minor_t *)inode->i_private : NULL;
    if (!minor || !minor->dev) return -(s64)ENODEV;

    drm_file_t *file = (drm_file_t *)kzalloc(sizeof(drm_file_t));
    if (!file) return -(s64)ENOMEM;

    drm_device_t *dev = minor->dev;
    file->dev            = dev;
    file->is_render_node = minor->is_render;
    file->next_handle    = 1;

    spinlock_lock(&dev->lock);
    static u32 s_magic_seq = 0x1000;
    file->magic = s_magic_seq++;

    /* The first client to open the card node becomes master, so a lone
     * compositor never has to ask for it. */
    if (!minor->is_render && !dev->master) {
        dev->master         = file;
        file->is_master     = true;
        file->authenticated = true;
    }
    spinlock_unlock(&dev->lock);

    filp->private_data = file;
    return 0;
}

static s64 drm_release(inode_t *inode, file_t *filp)
{
    (void)inode;
    drm_file_t *file = filp ? (drm_file_t *)filp->private_data : NULL;
    if (!file) return 0;

    drm_device_t *dev = file->dev;

    drm_vblank_file_closed(dev, file);

    spinlock_lock(&dev->lock);
    if (dev->master == file) dev->master = NULL;
    spinlock_unlock(&dev->lock);

    drm_gem_release_all(file);
    kfree(file);
    filp->private_data = NULL;
    return 0;
}

static s64 drm_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    drm_file_t *file = filp ? (drm_file_t *)filp->private_data : NULL;
    if (!file) return -(s64)EBADF;
    return drm_ioctl_dispatch(file->dev, file, cmd, arg);
}

/*
 * Reading a DRM fd yields whole event records — never a partial one, which is
 * what libdrm's event handler relies on.  A blocking fd waits for the next
 * frame rather than spinning: that is how a compositor paces itself to the
 * display instead of to the CPU.
 */
static s64 drm_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    drm_file_t *file = filp ? (drm_file_t *)filp->private_data : NULL;
    if (!file || !buf) return -(s64)EINVAL;
    if (len < sizeof(struct drm_event_vblank)) return -(s64)EINVAL;

    drm_device_t *dev = file->dev;
    bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;

    for (;;) {
        /* Drain into a kernel staging buffer under the lock, then copy out
         * once it is dropped — user memory may fault, and faulting with a
         * spinlock held would take the whole card down with it. */
        struct drm_event_vblank staged[DRM_MAX_EVENTS];
        size_t count = 0;
        size_t room  = len / sizeof(struct drm_event_vblank);
        if (room > DRM_MAX_EVENTS) room = DRM_MAX_EVENTS;

        spinlock_lock(&dev->lock);
        while (file->event_tail != file->event_head && count < room) {
            drm_pending_event_t *pe = &file->events[file->event_tail];
            staged[count++]  = pe->ev;
            pe->valid        = false;
            file->event_tail = (file->event_tail + 1) % DRM_MAX_EVENTS;
        }
        spinlock_unlock(&dev->lock);

        if (count) {
            size_t bytes = count * sizeof(struct drm_event_vblank);
            /* Kernel buffer — see fs/vfs.h. The staging copy above is still
             * worth keeping: it is what lets the device lock be dropped
             * before this copy runs. */
            memcpy(buf, staged, bytes);
            return (s64)bytes;
        }
        if (nonblock) return -(s64)EAGAIN;

        drm_wait_on(file);
    }
}

static int drm_poll(file_t *filp)
{
    drm_file_t *file = filp ? (drm_file_t *)filp->private_data : NULL;
    if (!file) return 0x0020 /* POLLNVAL */;
    return (file->event_tail != file->event_head) ? (0x0001 | 0x0040) : 0;
}

/*
 * mmap() offsets on a DRM fd are the fake offsets handed out by MAP_DUMB, not
 * addresses.  Pages go in write-combining so a client's software rendering
 * into VRAM bursts across PCIe instead of trickling.
 */
static s64 drm_mmap(file_t *filp, virt_addr_t vaddr, size_t len,
                    u32 prot, u32 flags, u64 offset)
{
    (void)prot; (void)flags;
    drm_file_t *file = filp ? (drm_file_t *)filp->private_data : NULL;
    if (!file) return -(s64)EBADF;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    drm_device_t *dev = file->dev;

    spinlock_lock(&dev->lock);
    drm_gem_object_t *obj = drm_gem_object_by_mmap_offset(dev, offset);
    if (!obj) {
        spinlock_unlock(&dev->lock);
        return -(s64)EINVAL;
    }

    size_t first = (offset - obj->mmap_offset) / PAGE_SIZE;
    size_t want  = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    if (first >= obj->npages) {
        spinlock_unlock(&dev->lock);
        return -(s64)EINVAL;
    }
    if (want > obj->npages - first) want = obj->npages - first;

    for (size_t i = 0; i < want; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE, obj->pages[first + i], VMM_USER_WC);
    }
    bool in_vram = obj->in_vram;
    spinlock_unlock(&dev->lock);

    /* Once userspace owns a scanout buffer, the kernel console must stop
     * painting over it. */
    if (in_vram) console_disable_fb();

    return 0;
}

static file_operations_t g_drm_fops = {
    .read    = drm_read,
    .ioctl   = drm_ioctl,
    .mmap    = drm_mmap,
    .open    = drm_open,
    .release = drm_release,
    .poll    = drm_poll,
};

/* ── Card lifecycle ──────────────────────────────────────────────────────── */

drm_device_t *drm_dev_alloc(const drm_driver_t *driver, dm_device_t *dm)
{
    if (!driver) return NULL;

    drm_device_t *dev = (drm_device_t *)kzalloc(sizeof(drm_device_t));
    if (!dev) return NULL;

    dev->driver         = driver;
    dev->dm             = dm;
    dev->next_object_id = 1;
    dev->min_width      = 640;
    dev->min_height     = 480;
    dev->max_width      = 3840;
    dev->max_height     = 2160;
    dev->cursor_width   = 64;
    dev->cursor_height  = 64;
    spinlock_init(&dev->lock);

    if (dm) {
        snprintf(dev->unique, sizeof(dev->unique), "%s:%s", dm->bus ? dm->bus->name : "drm", dm->name);
    } else {
        snprintf(dev->unique, sizeof(dev->unique), "platform:drm");
    }
    return dev;
}

int drm_dev_register(drm_device_t *dev)
{
    if (!dev) return -EINVAL;
    if (g_drm_card_count >= DRM_MAX_CARDS) return -ENOSPC;

    dev->index = (int)g_drm_card_count;

    if (dev->driver->load) {
        int ret = dev->driver->load(dev);
        if (ret != 0) {
            pr_debug("[DRM] %s: load failed (%d)\n", dev->driver->name, ret);
            return ret;
        }
    }

    /* One minor record per node; the inode hands it back on open(). */
    drm_minor_t *card_minor = (drm_minor_t *)kzalloc(sizeof(drm_minor_t));
    if (!card_minor) return -ENOMEM;
    card_minor->dev = dev;

    char path[32];
    snprintf(path, sizeof(path), "dri/card%d", dev->index);
    devfs_register_device(path, &g_drm_fops, card_minor);

    /* /dev/card0 as well, for clients that predate the /dev/dri layout. */
    if (dev->index == 0) {
        devfs_register_device("card0", &g_drm_fops, card_minor);
    }

    if (dev->driver->features & DRIVER_RENDER) {
        drm_minor_t *render_minor = (drm_minor_t *)kzalloc(sizeof(drm_minor_t));
        if (render_minor) {
            render_minor->dev       = dev;
            render_minor->is_render = true;
            snprintf(path, sizeof(path), "dri/renderD%d", DRM_RENDER_MINOR_BASE + dev->index);
            devfs_register_device(path, &g_drm_fops, render_minor);
        }
    }

    /* Publish the card in the driver model so it shows up under
     * /sys/class/drm/cardN with the Linux major:minor pair. */
    snprintf(path, sizeof(path), "card%d", dev->index);
    dm_device_t *card_dev = dm_device_alloc(path, NULL);
    if (card_dev) {
        card_dev->parent = dev->dm;
        snprintf(card_dev->modalias, sizeof(card_dev->modalias), "drm:%s", dev->driver->name);
        if (dm_device_register(card_dev) == 0) {
            dm_device_add_class(card_dev, &g_drm_class,
                                (DRM_MAJOR << 20) | (u32)dev->index);
        }
    }

    dev->next     = NULL;
    drm_device_t **tail = &g_drm_devices;
    while (*tail) tail = &(*tail)->next;
    *tail = dev;
    g_drm_card_count++;

    pr_debug("[DRM] card%d: %s (%s) — %u crtc, %u connector, %u plane, %s\n",
             dev->index, dev->driver->name, dev->unique,
             dev->num_crtc, dev->num_connector, dev->num_plane,
             (dev->driver->features & DRIVER_RENDER) ? "render node" : "no render node");
    return 0;
}

void drm_dev_unregister(drm_device_t *dev)
{
    if (!dev) return;

    if (dev->driver->unload) dev->driver->unload(dev);

    drm_device_t **pp = &g_drm_devices;
    while (*pp) {
        if (*pp == dev) { *pp = dev->next; g_drm_card_count--; break; }
        pp = &(*pp)->next;
    }
}

/* ── Subsystem entry point ───────────────────────────────────────────────── */

void drm_subsystem_init(void)
{
    dm_class_register(&g_drm_class);

    /* Drivers are offered the hardware in preference order: a real GPU first,
     * the Bochs adapter next, and the bootloader framebuffer as the fallback
     * that always works.  Each declines if its hardware is absent. */
    virtgpu_drm_init();
    vmwgfx_drm_init();
    bochs_drm_init();
    simpledrm_init();

    if (g_drm_card_count == 0) {
        pr_debug("[DRM] no display hardware claimed — /dev/dri is empty\n");
        return;
    }

    /* Nothing reaches the screen at a frame boundary until this is running. */
    drm_vblank_init();
}
