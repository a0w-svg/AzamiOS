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
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/sched/sched.h"
#include "../../arch/x86_64/boot/limine.h"
#include "../../arch/x86_64/boot/limine_req.h"
#include "../misc/bga.h"
#include "virtio_gpu.h"
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
} fb_driver_state_t;

static fb_driver_state_t g_fb_state;

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

    if (wait_vsync) vsync_wait(0);
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

    /* 2. Try Limine Bootloader Linear Framebuffer */
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

    /* 3. Try VirtIO GPU */
    if (g_gpu.framebuffer_phys != 0) {
        g_fb_state.phys_addr       = g_gpu.framebuffer_phys;
        g_fb_state.width           = g_gpu.screen_width ? g_gpu.screen_width : 1280;
        g_fb_state.height          = g_gpu.screen_height ? g_gpu.screen_height : 800;
        g_fb_state.pitch           = g_fb_state.width * 4;
        g_fb_state.bpp             = 32;
        g_fb_state.single_fb_size  = g_gpu.framebuffer_size;
        g_fb_state.total_vram_size = g_gpu.framebuffer_size;
        g_fb_state.buffers         = 1;
        g_fb_state.has_hw_flip     = false;
        g_fb_state.y_offset        = 0;
        pr_debug("[FBDEV] Active Backend: VirtIO-GPU (1280x800x32, LFB: 0x%016llx)\n",
                 (unsigned long long)g_gpu.framebuffer_phys);
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
    /* Kernel buffer — see fs/vfs.h. */
    memcpy(dst, buf, to_write);

    *offset += to_write;
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
             * top of one rather than in the middle. */
            vsync_wait(0);
            return 0;

        case FBIOBLANK:
            return 0;

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

    /* Write-combining, so a client's stores to VRAM burst across the bus
     * instead of trickling out one at a time. */
    u64 vmm_flags = VMM_USER_WC | VMM_F_SHARED;

    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t phys_base = g_fb_state.phys_addr + offset;

    for (size_t i = 0; i < pages; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE,
                phys_base + i * PAGE_SIZE, vmm_flags);
    }

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

void fbdev_init(void)
{
    fbdev_probe_hardware();
    devfs_register_device("fb0", &fbdev_fops, &g_fb_state);
    pr_debug("[FBDEV] Linux-compatible Framebuffer Device /dev/fb0 registered successfully\n");
}
