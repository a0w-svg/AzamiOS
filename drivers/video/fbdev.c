/* ============================================================================
 * AzamiOS — POSIX Linux-Compatible Framebuffer Device (/dev/fb0)
 * File: drivers/video/fbdev.c
 *
 * Implements the standard Linux /dev/fb0 character device supporting:
 *  - Unified hardware display backend (BGA / Limine Boot FB / VirtIO-GPU)
 *  - Arbitrary stream read/write/lseek to VRAM
 *  - Page-by-page physical VRAM mmap with Write-Combining cache attributes
 *  - Complete Linux fbdev ioctls (FBIOGET_VSCREENINFO, FBIOPUT_VSCREENINFO,
 *    FBIOGET_FSCREENINFO, FBIOPAN_DISPLAY for double-buffering page flips)
 *
 * ── Presenting without tearing ───────────────────────────────────────────────
 * When the adapter has video memory for more than one screen, the device
 * advertises yres_virtual as a multiple of yres and a ypanstep of 1, which is
 * how a Linux fbdev client is told "you may double-buffer".  The client then
 * draws into the half that is not on screen and calls FBIOPAN_DISPLAY, which
 * waits for the frame boundary before moving the scanout origin:
 *
 *     draw into buffer N ; FBIOPAN_DISPLAY(yoffset = N * yres)
 *
 * Because the swap is one register write timed against the vsync clock in
 * <azami/vsync.h>, the display never shows a frame that is half old and half
 * new — the flicker a single-buffered client gets from painting the live
 * scanout simply cannot happen.  FBIO_WAITFORVSYNC blocks on the same clock
 * for clients that pace themselves without panning.
 * ============================================================================ */

#include "../../include/azami/fb.h"
#include "../../include/azami/defs.h"
#include "../../fs/vfs.h"
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../arch/x86_64/boot/limine.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../misc/bga.h"
#include "virtio_gpu.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include <azami/debug.h>
#include <azami/vsync.h>

extern virtio_gpu_state_t g_gpu;
extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

typedef struct {
    phys_addr_t phys_addr;
    size_t      total_vram_size;
    size_t      single_fb_size;
    u32         width;
    u32         height;
    u32         pitch;
    u32         bpp;
    u32         y_offset;
    u32         buffers;        /* screens the video memory holds: 1 = no flip */
    bool        has_hw_flip;

    /* VirtIO-GPU backend: the framebuffer is a host-owned 2D resource, so a
     * guest write is invisible until TRANSFER_TO_HOST_2D + RESOURCE_FLUSH
     * copies it across. There is no scanout aperture to write to directly and
     * no pan register, so /dev/fb0 keeps the host in sync itself:
     *   - a client that pans or waits for vsync triggers a flush inline;
     *   - a client that only mmaps and draws is served by fbdev_flusher(),
     *     which pushes the scanout once per vblank while a mapping is live.
     * present_gen advances on every write()/pan so the flusher can skip a
     * frame that nothing touched. */
    bool        is_virtio;
    bool        mmap_active;
    u64         present_gen;
    u64         flushed_gen;

    /* Client-reported damage (FBIOAZ_DAMAGE), stored as an inclusive-exclusive
     * box and unioned until the next flush. When dmg_valid is set the present
     * worker transfers only this region instead of the whole scanout. */
    spinlock_t  dmg_lock;
    bool        dmg_valid;
    u32         dmg_x1, dmg_y1, dmg_x2, dmg_y2;
} fb_driver_state_t;

static fb_driver_state_t g_fb_state;

/* Push one rectangle of the scanout resource to the host. The copy is
 * guest-phys to host-phys inside the hypervisor, so the cost scales with the
 * rectangle, not the screen. */
static s64 fbdev_virtio_present_rect(u32 x, u32 y, u32 w, u32 h)
{
    if (!g_fb_state.is_virtio) return 0;
    if (w == 0 || h == 0) return 0;
    if (x >= g_fb_state.width || y >= g_fb_state.height) return 0;
    if (x + w > g_fb_state.width)  w = g_fb_state.width  - x;
    if (y + h > g_fb_state.height) h = g_fb_state.height - y;

    u64 off = (u64)y * g_fb_state.pitch + (u64)x * 4;
    if (virtio_gpu_transfer_to_host_2d_rect(g_gpu.resource_id, x, y, w, h, off) < 0)
        return -(s64)EIO;
    if (virtio_gpu_resource_flush_rect(g_gpu.resource_id, x, y, w, h) < 0)
        return -(s64)EIO;
    g_fb_state.flushed_gen = __atomic_load_n(&g_fb_state.present_gen, __ATOMIC_RELAXED);
    return 0;
}

static s64 fbdev_virtio_present(void)
{
    return fbdev_virtio_present_rect(0, 0, g_fb_state.width, g_fb_state.height);
}

/*
 * Move the scanout origin to @buf_idx, at a frame boundary.
 *
 * Waiting first is the whole point: the adapter latches the offset whenever it
 * is written, so panning mid-frame shows the top of one buffer above the
 * bottom of the other for exactly one frame — a visible tear or, when it
 * happens every frame, a flicker.
 */
static s64 fbdev_flip_to(u32 buf_idx, bool wait_vsync)
{
    if (!g_fb_state.has_hw_flip || buf_idx >= g_fb_state.buffers) return -(s64)EINVAL;

    if (wait_vsync && bga_get_fb_phys() == 0) vsync_wait(0);

    if (g_fb_state.is_virtio) {
        u32 y_off = buf_idx * g_fb_state.height;
        u64 byte_off = (u64)y_off * g_fb_state.pitch;
        virtio_gpu_transfer_to_host_2d_rect(g_gpu.resource_id, 0, y_off,
                                            g_fb_state.width, g_fb_state.height, byte_off);
        virtio_gpu_set_scanout_offset(0, g_gpu.resource_id, 0, y_off,
                                      g_fb_state.width, g_fb_state.height);
        virtio_gpu_resource_flush_rect(g_gpu.resource_id, 0, y_off,
                                       g_fb_state.width, g_fb_state.height);
        g_fb_state.y_offset = y_off;
        return 0;
    }

    if (bga_flip_buffer(buf_idx) != 0) return -(s64)EIO;

    g_fb_state.y_offset = buf_idx * g_fb_state.height;
    return 0;
}

static void fbdev_probe_hardware(void)
{
    __builtin_memset(&g_fb_state, 0, sizeof(g_fb_state));

    /* 1. Try Bochs Graphics Adapter (BGA) / QEMU Standard VGA */
    phys_addr_t bga_phys = bga_get_fb_phys();
    if (bga_phys != 0) {
        g_fb_state.phys_addr       = bga_phys;
        g_fb_state.width           = bga_get_width() ? bga_get_width() : 1280;
        g_fb_state.height          = bga_get_height() ? bga_get_height() : 800;
        g_fb_state.pitch           = bga_get_pitch() ? bga_get_pitch() : (g_fb_state.width * 4);
        g_fb_state.bpp             = bga_get_bpp() ? bga_get_bpp() : 32;
        g_fb_state.single_fb_size  = bga_get_fb_size();
        g_fb_state.total_vram_size = bga_get_fb_total_size();

        /* Panning is only offered when the memory to pan into is really
         * there; claiming it otherwise hands clients a buffer that overlaps
         * the visible one, which looks exactly like the tearing it was
         * supposed to prevent. */
        size_t vram = bga_get_vram_size();
        g_fb_state.buffers = 1;
        if (g_fb_state.single_fb_size && vram / g_fb_state.single_fb_size >= 2) {
            g_fb_state.buffers = 2;
        }
        if (g_fb_state.total_vram_size > vram) g_fb_state.total_vram_size = vram;

        g_fb_state.has_hw_flip     = (g_fb_state.buffers > 1);
        g_fb_state.y_offset        = 0;
        pr_debug("[FBDEV] Active Backend: Bochs Graphics Adapter (%ux%ux%u, %u buffer%s, LFB: 0x%016llx)\n",
                 g_fb_state.width, g_fb_state.height, g_fb_state.bpp,
                 g_fb_state.buffers, g_fb_state.buffers == 1 ? "" : "s",
                 (unsigned long long)bga_phys);
        return;
    }

    /* 2. Try VirtIO GPU — before the Limine GOP fallback. When a virtio-gpu
     * was initialised its backing is a host-owned 2D resource, and the
     * damage-scoped TRANSFER_TO_HOST path plus the hardware-cursor overlay
     * are what this driver drives it with. If QEMU also exposed a std-VGA the
     * Limine GOP below would point at *that* aperture and hide the virtio
     * device entirely, so this check has to come first. */
    if (g_gpu.framebuffer_phys != 0) {
        g_fb_state.phys_addr       = g_gpu.framebuffer_phys;
        g_fb_state.width           = g_gpu.screen_width ? g_gpu.screen_width : 1280;
        g_fb_state.height          = g_gpu.screen_height ? g_gpu.screen_height : 800;
        g_fb_state.pitch           = g_fb_state.width * 4;
        g_fb_state.bpp             = 32;
        g_fb_state.single_fb_size  = (size_t)g_fb_state.pitch * g_fb_state.height;
        g_fb_state.total_vram_size = g_gpu.framebuffer_size;
        g_fb_state.buffers         = (g_fb_state.single_fb_size && g_gpu.framebuffer_size >= g_fb_state.single_fb_size * 2) ? 2 : 1;
        g_fb_state.has_hw_flip     = (g_fb_state.buffers > 1);
        g_fb_state.y_offset        = 0;
        g_fb_state.is_virtio       = true;
        pr_debug("[FBDEV] Active Backend: VirtIO-GPU (%ux%ux32, %u buffer%s, resource %u, LFB: 0x%016llx)\n",
                 g_fb_state.width, g_fb_state.height,
                 g_fb_state.buffers, g_fb_state.buffers == 1 ? "" : "s",
                 g_gpu.resource_id,
                 (unsigned long long)g_gpu.framebuffer_phys);
        return;
    }

    /* 3. Try Limine Bootloader Linear Framebuffer */
    struct limine_framebuffer *lfb = az_boot_framebuffer();
    if (lfb && lfb->address) {
        g_fb_state.phys_addr       = VIRT_TO_PHYS((virt_addr_t)lfb->address);
        g_fb_state.width           = (u32)lfb->width;
        g_fb_state.height          = (u32)lfb->height;
        g_fb_state.pitch           = (u32)lfb->pitch;
        g_fb_state.bpp             = (u32)lfb->bpp;
        g_fb_state.single_fb_size  = (size_t)lfb->pitch * lfb->height;
        g_fb_state.total_vram_size = g_fb_state.single_fb_size;
        g_fb_state.buffers         = 1;
        g_fb_state.has_hw_flip     = false;
        g_fb_state.y_offset        = 0;
        pr_debug("[FBDEV] Active Backend: Limine GOP Framebuffer (%ux%ux%u, LFB: 0x%016llx)\n",
                 g_fb_state.width, g_fb_state.height, g_fb_state.bpp,
                 (unsigned long long)g_fb_state.phys_addr);
        return;
    }
}

static s64 fbdev_open(struct inode *inode, struct file *filp)
{
    (void)inode;
    if (g_fb_state.phys_addr == 0) {
        fbdev_probe_hardware();
    }
    filp->private_data = &g_fb_state;
    return 0;
}

static s64 fbdev_release(struct inode *inode, struct file *filp)
{
    (void)inode;
    (void)filp;
    return 0;
}

static s64 fbdev_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || !offset) return -(s64)EFAULT;
    if (*offset >= g_fb_state.total_vram_size) return 0;

    size_t avail = g_fb_state.total_vram_size - (size_t)*offset;
    size_t to_read = (len > avail) ? avail : len;

    u8 *src = (u8 *)PHYS_TO_VIRT(g_fb_state.phys_addr + *offset);
    /* Kernel buffer — see fs/vfs.h. read(2) on /dev/fb0 returned -EFAULT for
     * every caller until this was a plain copy. */
    memcpy(buf, src, to_read);

    *offset += to_read;
    return (s64)to_read;
}

static s64 fbdev_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || !offset) return -(s64)EFAULT;
    if (*offset >= g_fb_state.total_vram_size) return -(s64)ENOSPC;

    size_t avail = g_fb_state.total_vram_size - (size_t)*offset;
    size_t to_write = (len > avail) ? avail : len;

    u8 *dst = (u8 *)PHYS_TO_VIRT(g_fb_state.phys_addr + *offset);
    /* High-performance non-temporal / ERMS VRAM copy */
    hw_copy_to_vram(dst, buf, to_write);

    *offset += to_write;
    __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
    return (s64)to_write;
}

static s64 fbdev_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    if (g_fb_state.phys_addr == 0) {
        fbdev_probe_hardware();
    }

    switch (cmd) {
        case FBIOGET_VSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_var_screeninfo var;
            __builtin_memset(&var, 0, sizeof(var));
            var.xres           = g_fb_state.width;
            var.yres           = g_fb_state.height;
            var.xres_virtual   = g_fb_state.width;
            var.yres_virtual   = g_fb_state.height * g_fb_state.buffers;
            var.xoffset        = 0;
            var.yoffset        = g_fb_state.y_offset;
            var.bits_per_pixel = g_fb_state.bpp;
            
            /* Standard 32-bit ARGB/XRGB bitfields */
            var.red.offset     = 16; var.red.length     = 8;
            var.green.offset   = 8;  var.green.length   = 8;
            var.blue.offset    = 0;  var.blue.length    = 8;
            var.transp.offset  = 24; var.transp.length  = 8;

            if (copy_to_user((void *)(uintptr_t)arg, &var, sizeof(var)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case FBIOPUT_VSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_var_screeninfo var;
            if (copy_from_user(&var, (void *)(uintptr_t)arg, sizeof(var)) != 0) return -(s64)EFAULT;

            /* The mode itself is fixed; the one field a client may change
             * here is where the scanout starts. */
            if (g_fb_state.has_hw_flip && var.yres > 0) {
                bool at_vbl = (var.activate & FB_ACTIVATE_VBL) != 0 ||
                              (var.activate & FB_ACTIVATE_MASK) == FB_ACTIVATE_NOW;
                fbdev_flip_to(var.yoffset / var.yres, at_vbl);
            }
            return 0;
        }

        case FBIOGET_FSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_fix_screeninfo fix;
            __builtin_memset(&fix, 0, sizeof(fix));
            strncpy(fix.id, "AzamiFB", sizeof(fix.id) - 1);
            fix.smem_start  = g_fb_state.phys_addr;
            fix.smem_len    = (u32)g_fb_state.total_vram_size;
            fix.type        = FB_TYPE_PACKED_PIXELS;
            fix.visual      = FB_VISUAL_TRUECOLOR;
            fix.line_length = g_fb_state.pitch;
            fix.xpanstep    = 0;
            fix.ypanstep    = g_fb_state.has_hw_flip ? 1 : 0;
            fix.accel       = 0;

            if (copy_to_user((void *)(uintptr_t)arg, &fix, sizeof(fix)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case FBIOPAN_DISPLAY: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_var_screeninfo var;
            if (copy_from_user(&var, (void *)(uintptr_t)arg, sizeof(var)) != 0) return -(s64)EFAULT;

            /* VirtIO-GPU has one scanout resource and no pan register: a pan
             * request is just "present what I have drawn" at the frame edge. */
            if (g_fb_state.is_virtio) {
                vsync_wait(0);
                __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
                return fbdev_virtio_present();
            }

            if (!g_fb_state.has_hw_flip || g_fb_state.height == 0) return 0;
            if (var.yoffset % g_fb_state.height) return -(s64)EINVAL;

            u32 buf_idx = var.yoffset / g_fb_state.height;
            if (buf_idx >= g_fb_state.buffers) return -(s64)EINVAL;

            /* The swap lands between frames, and the call returns once it
             * has: a client that pans every frame is paced by the display. */
            return fbdev_flip_to(buf_idx, true);
        }

        case FBIOGETCMAP:
        case FBIOPUTCMAP:
            /* Truecolor / 32-bit direct color visual - colormap is identity */
            return 0;

        case FBIOGET_CON2FBMAP: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_con2fbmap c2f;
            c2f.console = 0;
            c2f.framebuffer = 0;
            if (copy_to_user((void *)(uintptr_t)arg, &c2f, sizeof(c2f)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case FBIOPUT_CON2FBMAP:
            return 0;

        case FBIO_WAITFORVSYNC:
            /* Block until the next frame starts, so a client that draws
             * straight into the scanout at least starts each frame at the
             * top of one rather than in the middle. On VirtIO-GPU there is no
             * live scanout to draw into, so this also pushes the frame the
             * client just finished. */
            vsync_wait(0);
            if (g_fb_state.is_virtio) {
                __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
                return fbdev_virtio_present();
            }
            return 0;

        case FBIOBLANK:
            return 0;

        case FBIOAZ_HWCURSOR_SET: {
            /* Only VirtIO-GPU has a cursor overlay here; -ENOTTY tells the
             * client to keep compositing the pointer itself. */
            if (!g_fb_state.is_virtio) return -(s64)ENOTTY;

            struct fb_az_hwcursor c;
            if (copy_from_user(&c, (void *)(uintptr_t)arg, sizeof(c)) != 0) return -(s64)EFAULT;
            if (c.width == 0 || c.height == 0 ||
                c.width  > FB_AZ_HWCURSOR_MAX || c.height > FB_AZ_HWCURSOR_MAX ||
                c.hot_x >= c.width || c.hot_y >= c.height || c.image == 0)
                return -(s64)EINVAL;

            u32 slot_px = VIRTIO_GPU_CURSOR_W * VIRTIO_GPU_CURSOR_H;
            u32 *img = (u32 *)kzalloc(slot_px * 4);
            if (!img) return -(s64)ENOMEM;

            /* The overlay is a fixed 64x64; drop the client image into its
             * top-left and leave the rest transparent. */
            for (u32 y = 0; y < c.height; y++) {
                if (copy_from_user(&img[y * VIRTIO_GPU_CURSOR_W],
                                   (void *)(uintptr_t)(c.image + (u64)y * c.width * 4),
                                   c.width * 4) != 0) {
                    kfree(img);
                    return -(s64)EFAULT;
                }
            }

            int r = virtio_gpu_cursor_define(img, c.hot_x, c.hot_y);
            kfree(img);
            return r ? -(s64)EIO : 0;
        }

        case FBIOAZ_HWCURSOR_MOVE: {
            if (!g_fb_state.is_virtio) return -(s64)ENOTTY;
            struct fb_az_hwcursor_pos p;
            if (copy_from_user(&p, (void *)(uintptr_t)arg, sizeof(p)) != 0) return -(s64)EFAULT;
            u32 x = p.x < 0 ? 0u : (u32)p.x;
            u32 y = p.y < 0 ? 0u : (u32)p.y;
            return virtio_gpu_cursor_move(x, y) ? -(s64)EIO : 0;
        }

        case FBIOAZ_HWCURSOR_HIDE:
            if (!g_fb_state.is_virtio) return -(s64)ENOTTY;
            return virtio_gpu_cursor_hide() ? -(s64)EIO : 0;

        case FBIOAZ_DAMAGE: {
            if (!g_fb_state.is_virtio) return 0;   /* nothing to sync; harmless */
            struct fb_az_rect r;
            if (copy_from_user(&r, (void *)(uintptr_t)arg, sizeof(r)) != 0) return -(s64)EFAULT;
            if (r.w == 0 || r.h == 0) return 0;

            u32 x1 = r.x, y1 = r.y;
            u32 x2 = r.x + r.w, y2 = r.y + r.h;
            if (x2 > g_fb_state.width)  x2 = g_fb_state.width;
            if (y2 > g_fb_state.height) y2 = g_fb_state.height;
            if (x1 >= x2 || y1 >= y2) return 0;

            irqflags_t f = spinlock_lock_irqsave(&g_fb_state.dmg_lock);
            if (!g_fb_state.dmg_valid) {
                g_fb_state.dmg_x1 = x1; g_fb_state.dmg_y1 = y1;
                g_fb_state.dmg_x2 = x2; g_fb_state.dmg_y2 = y2;
                g_fb_state.dmg_valid = true;
            } else {
                if (x1 < g_fb_state.dmg_x1) g_fb_state.dmg_x1 = x1;
                if (y1 < g_fb_state.dmg_y1) g_fb_state.dmg_y1 = y1;
                if (x2 > g_fb_state.dmg_x2) g_fb_state.dmg_x2 = x2;
                if (y2 > g_fb_state.dmg_y2) g_fb_state.dmg_y2 = y2;
            }
            spinlock_unlock_irqrestore(&g_fb_state.dmg_lock, f);
            __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
            return 0;
        }

        case FBIOAZ_ACCEL_FILL: {
            struct fb_az_fill fill;
            if (copy_from_user(&fill, (void *)(uintptr_t)arg, sizeof(fill)) != 0) return -(s64)EFAULT;
            if (fill.w == 0 || fill.h == 0) return 0;
            if (fill.x >= g_fb_state.width || fill.y >= g_fb_state.height) return -(s64)EINVAL;
            if (fill.buffer_idx >= g_fb_state.buffers) return -(s64)EINVAL;

            u32 w = fill.w, h = fill.h;
            if (fill.x + w > g_fb_state.width)  w = g_fb_state.width  - fill.x;
            if (fill.y + h > g_fb_state.height) h = g_fb_state.height - fill.y;

            u32 y_base = fill.buffer_idx * g_fb_state.height + fill.y;
            u8 *vram_base = (u8 *)PHYS_TO_VIRT(g_fb_state.phys_addr);

            for (u32 row = 0; row < h; row++) {
                u32 *row_dst = (u32 *)(vram_base + (size_t)(y_base + row) * g_fb_state.pitch + (size_t)fill.x * 4);
                hw_fill_vram(row_dst, fill.color, w);
            }

            if (g_fb_state.is_virtio && fill.buffer_idx == (g_fb_state.y_offset / (g_fb_state.height ? g_fb_state.height : 1))) {
                fbdev_virtio_present_rect(fill.x, fill.y, w, h);
            }
            __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
            return 0;
        }

        case FBIOAZ_ACCEL_COPY: {
            struct fb_az_copy cp;
            if (copy_from_user(&cp, (void *)(uintptr_t)arg, sizeof(cp)) != 0) return -(s64)EFAULT;
            if (cp.w == 0 || cp.h == 0) return 0;
            if (cp.src_buf >= g_fb_state.buffers || cp.dst_buf >= g_fb_state.buffers) return -(s64)EINVAL;
            if (cp.src_x >= g_fb_state.width || cp.src_y >= g_fb_state.height) return -(s64)EINVAL;
            if (cp.dst_x >= g_fb_state.width || cp.dst_y >= g_fb_state.height) return -(s64)EINVAL;

            u32 w = cp.w, h = cp.h;
            if (cp.src_x + w > g_fb_state.width)  w = g_fb_state.width  - cp.src_x;
            if (cp.src_y + h > g_fb_state.height) h = g_fb_state.height - cp.src_y;
            if (cp.dst_x + w > g_fb_state.width)  w = g_fb_state.width  - cp.dst_x;
            if (cp.dst_y + h > g_fb_state.height) h = g_fb_state.height - cp.dst_y;

            u32 src_y_base = cp.src_buf * g_fb_state.height + cp.src_y;
            u32 dst_y_base = cp.dst_buf * g_fb_state.height + cp.dst_y;
            u8 *vram_base = (u8 *)PHYS_TO_VIRT(g_fb_state.phys_addr);

            for (u32 row = 0; row < h; row++) {
                const u8 *src_row = vram_base + (size_t)(src_y_base + row) * g_fb_state.pitch + (size_t)cp.src_x * 4;
                u8 *dst_row = vram_base + (size_t)(dst_y_base + row) * g_fb_state.pitch + (size_t)cp.dst_x * 4;
                hw_copy_to_vram(dst_row, src_row, (size_t)w * 4);
            }

            if (g_fb_state.is_virtio && cp.dst_buf == (g_fb_state.y_offset / (g_fb_state.height ? g_fb_state.height : 1))) {
                fbdev_virtio_present_rect(cp.dst_x, cp.dst_y, w, h);
            }
            __atomic_add_fetch(&g_fb_state.present_gen, 1, __ATOMIC_RELAXED);
            return 0;
        }

        case FBIOAZ_GET_CAPS: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_az_caps caps;
            memset(&caps, 0, sizeof(caps));
            caps.buffers = g_fb_state.buffers;
            caps.max_width = g_fb_state.width;
            caps.max_height = g_fb_state.height;
            caps.pitch = g_fb_state.pitch;
            caps.caps = (g_fb_state.buffers > 1 ? FB_AZ_CAP_DOUBLEBUF : 0) |
                        (g_fb_state.has_hw_flip ? FB_AZ_CAP_HW_FLIP : 0) |
                        (g_fb_state.is_virtio ? (FB_AZ_CAP_VIRTIO | FB_AZ_CAP_HW_CURSOR) : 0) |
                        FB_AZ_CAP_ACCEL_2D;
            if (copy_to_user((void *)(uintptr_t)arg, &caps, sizeof(caps)) != 0) return -(s64)EFAULT;
            return 0;
        }

        default:
            return -(s64)EINVAL;
    }
}


static s64 fbdev_mmap(struct file *filp, virt_addr_t vaddr, size_t len, u32 prot, u32 flags, u64 offset)
{
    (void)filp;
    (void)prot;
    (void)flags;

    if (g_fb_state.phys_addr == 0) {
        fbdev_probe_hardware();
    }

    if (g_fb_state.phys_addr == 0) {
        return -(s64)ENODEV;
    }

    if (offset >= g_fb_state.total_vram_size) {
        return -(s64)EINVAL;
    }

    if (offset + len > g_fb_state.total_vram_size) {
        len = g_fb_state.total_vram_size - offset;
    }

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -(s64)EPERM;

    /* BGA / Limine: real VRAM behind a bus, so write-combining lets a client's
     * stores burst instead of trickling out. VirtIO-GPU: the "framebuffer" is
     * ordinary guest RAM the hypervisor reads on TRANSFER_TO_HOST, so map it
     * write-back — there is no bus to combine towards and WB keeps it plainly
     * coherent with the transfer. */
    u64 vmm_flags = g_fb_state.is_virtio ? (VMM_USER_RW | VMM_F_SHARED)
                                         : (VMM_USER_WC | VMM_F_SHARED);

    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t phys_base = g_fb_state.phys_addr + offset;

    for (size_t i = 0; i < pages; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE,
                phys_base + i * PAGE_SIZE, vmm_flags);
    }

    if (g_fb_state.is_virtio)
        __atomic_store_n(&g_fb_state.mmap_active, true, __ATOMIC_RELAXED);

    extern void console_disable_fb(void);
    console_disable_fb();

    return 0;
}

static file_operations_t fbdev_fops = {
    .open    = fbdev_open,
    .release = fbdev_release,
    .read    = fbdev_read,
    .write   = fbdev_write,
    .ioctl   = fbdev_ioctl,
    .mmap    = fbdev_mmap,
};

/* Cheap fingerprint of the scanout: FNV-1a over a few thousand u32s spread
 * across the frame. Enough to notice any real repaint without walking 4 MiB
 * every vblank. */
static u32 fbdev_scanout_sample(void)
{
    const u32 *p = (const u32 *)PHYS_TO_VIRT(g_fb_state.phys_addr);
    size_t words = g_fb_state.single_fb_size / 4;
    size_t step  = words / 4096;
    if (step == 0) step = 1;

    u32 h = 0x811c9dc5u;
    for (size_t i = 0; i < words; i += step)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

/*
 * A mmap client (the compositor) draws straight into the resource backing and
 * the kernel never sees the stores, so this worker pushes the scanout to the
 * host once per vblank while a mapping is live — skipping a frame whose
 * fingerprint and present generation are both unchanged, so an idle desktop
 * costs one sparse read and nothing on the virtqueue.
 */
static void fbdev_flusher(void *arg)
{
    (void)arg;
    u32  last_sample = 0;
    bool have_sample = false;

    for (;;) {
        vsync_wait(0);

        /* is_virtio is settled by fbdev_probe_hardware(), which can run after
         * fbdev_init() (on the first open of /dev/fb0), so this is re-checked
         * every iteration rather than gating whether the worker exists. */
        if (!g_fb_state.is_virtio ||
            !__atomic_load_n(&g_fb_state.mmap_active, __ATOMIC_RELAXED))
            continue;

        /* Precise path: the client told us exactly what it drew (FBIOAZ_DAMAGE).
         * Transfer just that box — a moved window edge or a blinking caret is
         * a few KiB across the virtqueue, not 4 MiB. */
        irqflags_t df = spinlock_lock_irqsave(&g_fb_state.dmg_lock);
        bool have_dmg = g_fb_state.dmg_valid;
        u32 dx = g_fb_state.dmg_x1, dy = g_fb_state.dmg_y1;
        u32 dw = have_dmg ? g_fb_state.dmg_x2 - g_fb_state.dmg_x1 : 0;
        u32 dh = have_dmg ? g_fb_state.dmg_y2 - g_fb_state.dmg_y1 : 0;
        g_fb_state.dmg_valid = false;
        spinlock_unlock_irqrestore(&g_fb_state.dmg_lock, df);

        u64 gen = __atomic_load_n(&g_fb_state.present_gen, __ATOMIC_RELAXED);

        if (have_dmg) {
            __atomic_store_n(&g_fb_state.present_gen, gen, __ATOMIC_RELAXED);
            fbdev_virtio_present_rect(dx, dy, dw, dh);
            continue;
        }

        /* Fallback: a client that maps and draws without reporting damage —
         * detect a change with a sparse fingerprint and push the whole frame. */
        u32 sample = fbdev_scanout_sample();
        if (have_sample && sample == last_sample && gen == g_fb_state.flushed_gen)
            continue;

        last_sample = sample;
        have_sample = true;
        __atomic_store_n(&g_fb_state.present_gen, gen, __ATOMIC_RELAXED);
        fbdev_virtio_present();
    }
}

void fbdev_init(void)
{
    fbdev_probe_hardware();
    devfs_register_device("fb0", &fbdev_fops, &g_fb_state);

    /* Always spawn the present worker: whether the active backend turns out to
     * be VirtIO-GPU may not be known until /dev/fb0 is first opened, and the
     * worker idles cheaply (one vsync wait per frame) until then. */
    process_t *kproc = sched_kernel_process();
    if (kproc)
        thread_create(kproc, (uintptr_t)fbdev_flusher, 0, true);

    pr_debug("[FBDEV] Linux-compatible Framebuffer Device /dev/fb0 registered "
             "(present worker running)\n");
}
