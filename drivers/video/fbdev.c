/* ============================================================================
 * AzamiOS — POSIX Framebuffer Device (fbdev)
 * File: drivers/video/fbdev.c
 *
 * Implements the POSIX /dev/fb0 character device for the VirtIO GPU.
 * ============================================================================ */

#include "virtio_gpu.h"
#include "../../fs/vfs.h"
#include <azami/fb.h>
#include <azami/debug.h>
#include "../../kernel/lib/string.h"
#include "../../kernel/uaccess.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/sched/sched.h"

extern virtio_gpu_state_t g_gpu;

/* We defined devfs_register_device in devfs.c */
extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

static s64 fbdev_open(struct inode *inode, struct file *filp)
{
    /* The private_data contains the virtio_gpu_state_t pointer */
    filp->private_data = inode->i_private;
    return 0;
}

static s64 fbdev_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static s64 fbdev_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    virtio_gpu_state_t *gpu = (virtio_gpu_state_t *)filp->private_data;
    if (!gpu) return -1;

    switch (cmd) {
        case FBIOGET_VSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -1;
            struct fb_var_screeninfo var;
            memset(&var, 0, sizeof(var));
            var.xres = gpu->screen_width;
            var.yres = gpu->screen_height;
            var.xres_virtual = gpu->screen_width;
            var.yres_virtual = gpu->screen_height;
            var.bits_per_pixel = 32;
            
            var.red.offset = 16; var.red.length = 8;
            var.green.offset = 8; var.green.length = 8;
            var.blue.offset = 0; var.blue.length = 8;
            var.transp.offset = 24; var.transp.length = 8;
            if (copy_to_user((void *)(uintptr_t)arg, &var, sizeof(var)) != 0) return -1;
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -1;
            struct fb_fix_screeninfo fix;
            memset(&fix, 0, sizeof(fix));
            strncpy(fix.id, "VirtIO GPU", sizeof(fix.id) - 1);
            fix.smem_start = gpu->framebuffer_phys;
            fix.smem_len = gpu->framebuffer_size;
            fix.line_length = gpu->screen_width * 4;
            fix.visual = 2; /* FB_VISUAL_TRUECOLOR */
            if (copy_to_user((void *)(uintptr_t)arg, &fix, sizeof(fix)) != 0) return -1;
            return 0;
        }
        default:
            return -1;
    }
}

static s64 fbdev_mmap(struct file *filp, virt_addr_t vaddr, size_t len, u32 prot, u32 flags, u64 offset)
{
    virtio_gpu_state_t *gpu = (virtio_gpu_state_t *)filp->private_data;
    if (!gpu) return -1;

    if (offset + len > gpu->framebuffer_size) {
        return -1; /* Out of bounds */
    }

    /* H-08: Actually map the physical framebuffer into the user's address space.
     * Map page-by-page using the current process's PML4. */
    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -1;

    /* Determine VMM flags: user-accessible, writable, write-combining (PWT+PCD) */
    u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE | VMM_F_NX | VMM_F_PWT;
    (void)prot; (void)flags; /* simplified: always map RW for framebuffer */

    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t phys_base = gpu->framebuffer_phys + offset;

    for (size_t i = 0; i < pages; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE,
                phys_base + i * PAGE_SIZE, vmm_flags);
    }

    return 0;
}

static file_operations_t fbdev_fops = {
    .open = fbdev_open,
    .release = fbdev_release,
    .ioctl = fbdev_ioctl,
    .mmap = fbdev_mmap,
};

void fbdev_init(void)
{
    devfs_register_device("fb0", &fbdev_fops, &g_gpu);
}
