/* ============================================================================
 * AzamiOS — Bochs Graphics Adapter (BGA) Driver
 * File: drivers/bga.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "bga.h"
#include "../char/console.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/types.h"
#include "../../include/azami/fb.h"
#include "../../fs/vfs.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/hwaccel.h"
#include <azami/vsync.h>


/* BGA driver state */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    phys_addr_t fb_phys;
    virt_addr_t fb_virt;
    uint16_t version;
} bga_device_t;

static bga_device_t g_bga;

/* I/O Port Helper Functions */
static uint16_t bga_read_reg(uint16_t index)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bga_write_reg(uint16_t index, uint16_t value)
{
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

/* Initialization & Mode Setting */
void bga_set_video_mode(uint32_t width, uint32_t height, uint32_t bit_depth, uint8_t enable_lfb)
{
    /* Disable BGA to update registers safely */
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);

    bga_write_reg(VBE_DISPI_INDEX_XRES, width);
    bga_write_reg(VBE_DISPI_INDEX_YRES, height);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_WIDTH, width);
    bga_write_reg(VBE_DISPI_INDEX_VIRT_HEIGHT, height * 2); /* Double-buffered VRAM */
    bga_write_reg(VBE_DISPI_INDEX_X_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, 0);
    bga_write_reg(VBE_DISPI_INDEX_BPP, bit_depth);

    /* Construct the enable flags */
    uint16_t flags = VBE_DISPI_ENABLED | VBE_DISPI_NOCLEARMEM;
    if (enable_lfb) {
        flags |= VBE_DISPI_LFB_ENABLED;
    }

    bga_write_reg(VBE_DISPI_INDEX_ENABLE, flags);

    g_bga.width = width;
    g_bga.height = height;
    g_bga.pitch = width * (bit_depth / 8);
    g_bga.bpp = bit_depth;

    /* Update the OS console framebuffer */
    console_init_fb((void *)g_bga.fb_virt, width, height, g_bga.pitch, bit_depth);
    pr_debug("[BGA] Set mode to %ux%ux%u\n", width, height, bit_depth);
}

#include "../../kernel/uaccess.h"

/* Graphics Primitives */
void bga_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x >= g_bga.width || y >= g_bga.height || !g_bga.fb_virt) {
        return;
    }
    
    uint32_t offset = (y * g_bga.pitch) + (x * (g_bga.bpp / 8));
    
    if (g_bga.bpp == 32) {
        *(volatile uint32_t *)((uint8_t *)g_bga.fb_virt + offset) = color;
    } else if (g_bga.bpp == 16) {
        *(volatile uint16_t *)((uint8_t *)g_bga.fb_virt + offset) = (uint16_t)color;
    }
}

void bga_clear_screen(uint32_t color)
{
    if (!g_bga.fb_virt) return;

    if (g_bga.bpp == 32) {
        uint32_t pixels = g_bga.width * g_bga.height * 2; /* Clear both double-buffer pages */
        hw_fill_vram((void *)g_bga.fb_virt, color, pixels);
    } else {
        /* Fallback for other bpp */
        for (uint32_t y = 0; y < g_bga.height * 2; y++) {
            for (uint32_t x = 0; x < g_bga.width; x++) {
                bga_put_pixel(x, y, color);
            }
        }
    }
}

/* PCI Device Discovery and Setup */
static s64 bga_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    (void)filp;
    switch (cmd) {
        case FBIOGET_VSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_var_screeninfo var;
            __builtin_memset(&var, 0, sizeof(var));
            var.xres = g_bga.width;
            var.yres = g_bga.height;
            var.xres_virtual = g_bga.width;
            var.yres_virtual = g_bga.height;
            var.bits_per_pixel = g_bga.bpp;
            
            var.red.offset = 16; var.red.length = 8;
            var.green.offset = 8; var.green.length = 8;
            var.blue.offset = 0; var.blue.length = 8;
            var.transp.offset = 24; var.transp.length = 8;

            if (copy_to_user((void *)(uintptr_t)arg, &var, sizeof(var)) != 0) return -(s64)EFAULT;
            return 0;
        }
        case FBIOGET_FSCREENINFO: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_fix_screeninfo fix;
            __builtin_memset(&fix, 0, sizeof(fix));
            fix.smem_start = g_bga.fb_phys;
            fix.smem_len = g_bga.pitch * g_bga.height;
            fix.line_length = g_bga.pitch;
            fix.visual = 2; /* FB_VISUAL_TRUECOLOR */

            if (copy_to_user((void *)(uintptr_t)arg, &fix, sizeof(fix)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case FBIOPAN_DISPLAY: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_var_screeninfo var;
            if (copy_from_user(&var, (void *)(uintptr_t)arg, sizeof(var)) != 0) return -(s64)EFAULT;
            u32 buf_idx = var.yoffset / (g_bga.height ? g_bga.height : 1);
            if (bga_flip_buffer(buf_idx) != 0) return -(s64)EINVAL;
            return 0;
        }

        case FBIO_WAITFORVSYNC: {
            vsync_wait(0);
            return 0;
        }

        case FBIOAZ_GET_CAPS: {
            if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;
            struct fb_az_caps caps;
            memset(&caps, 0, sizeof(caps));
            caps.buffers = 2;
            caps.max_width = g_bga.width;
            caps.max_height = g_bga.height;
            caps.pitch = g_bga.pitch;
            caps.caps = FB_AZ_CAP_DOUBLEBUF | FB_AZ_CAP_HW_FLIP | FB_AZ_CAP_ACCEL_2D;
            if (copy_to_user((void *)(uintptr_t)arg, &caps, sizeof(caps)) != 0) return -(s64)EFAULT;
            return 0;
        }

        case FBIOAZ_ACCEL_FILL: {
            struct fb_az_fill fl;
            if (copy_from_user(&fl, (void *)(uintptr_t)arg, sizeof(fl)) != 0) return -(s64)EFAULT;
            if (fl.buffer_idx >= 2 || fl.w == 0 || fl.h == 0) return -(s64)EINVAL;
            if (fl.x >= g_bga.width || fl.y >= g_bga.height) return -(s64)EINVAL;

            u32 w = fl.w, h = fl.h;
            if (fl.x + w > g_bga.width)  w = g_bga.width  - fl.x;
            if (fl.y + h > g_bga.height) h = g_bga.height - fl.y;

            u32 y_base = fl.buffer_idx * g_bga.height + fl.y;
            u8 *vram_base = (u8 *)g_bga.fb_virt;

            for (u32 row = 0; row < h; row++) {
                u32 *row_ptr = (u32 *)(vram_base + (size_t)(y_base + row) * g_bga.pitch + (size_t)fl.x * 4);
                hw_fill_vram(row_ptr, fl.color, w);
            }
            return 0;
        }

        case FBIOAZ_ACCEL_COPY: {
            struct fb_az_copy cp;
            if (copy_from_user(&cp, (void *)(uintptr_t)arg, sizeof(cp)) != 0) return -(s64)EFAULT;
            if (cp.src_buf >= 2 || cp.dst_buf >= 2 || cp.w == 0 || cp.h == 0) return -(s64)EINVAL;
            if (cp.src_x >= g_bga.width || cp.src_y >= g_bga.height) return -(s64)EINVAL;
            if (cp.dst_x >= g_bga.width || cp.dst_y >= g_bga.height) return -(s64)EINVAL;

            u32 w = cp.w, h = cp.h;
            if (cp.src_x + w > g_bga.width)  w = g_bga.width  - cp.src_x;
            if (cp.src_y + h > g_bga.height) h = g_bga.height - cp.src_y;
            if (cp.dst_x + w > g_bga.width)  w = g_bga.width  - cp.dst_x;
            if (cp.dst_y + h > g_bga.height) h = g_bga.height - cp.dst_y;

            u32 src_y_base = cp.src_buf * g_bga.height + cp.src_y;
            u32 dst_y_base = cp.dst_buf * g_bga.height + cp.dst_y;
            u8 *vram_base = (u8 *)g_bga.fb_virt;

            for (u32 row = 0; row < h; row++) {
                const u8 *src_row = vram_base + (size_t)(src_y_base + row) * g_bga.pitch + (size_t)cp.src_x * 4;
                u8 *dst_row = vram_base + (size_t)(dst_y_base + row) * g_bga.pitch + (size_t)cp.dst_x * 4;
                hw_copy_to_vram(dst_row, src_row, (size_t)w * 4);
            }
            return 0;
        }

        default:
            return -(s64)EINVAL;
    }
}

static s64 bga_mmap(struct file *filp, virt_addr_t vaddr, size_t len, u32 prot, u32 flags, u64 offset)
{
    (void)filp; (void)prot; (void)flags;
    if (offset + len > g_bga.pitch * g_bga.height) return -1;

    process_t *proc = sched_current_process();
    if (!proc || !proc->pml4_phys) return -1;

    u64 vmm_flags = VMM_F_PRESENT | VMM_F_USER | VMM_F_WRITE | VMM_F_NX | VMM_F_SHARED | VMM_F_PWT;
    size_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    phys_addr_t phys_base = g_bga.fb_phys + offset;

    for (size_t i = 0; i < pages; i++) {
        vmm_map(proc->pml4_phys, vaddr + i * PAGE_SIZE,
                phys_base + i * PAGE_SIZE, vmm_flags);
    }
    return 0;
}

static file_operations_t bga_fops = {
    .ioctl = bga_ioctl,
    .mmap = bga_mmap,
};

static int bga_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    /* Single global instance: refuse a second card rather than remapping
     * the aperture out from under the first. */
    if (g_bga.fb_phys) return -EBUSY;

    device_t *node = dm->hal;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci) return -ENODEV;

    pr_debug("[BGA] Found Bochs Graphics Adapter at PCI %02x:%02x.%x\n",
             pci->bus, pci->slot, pci->func);

    /* Enable Memory Space and Bus Mastering so BAR0 responds to writes */
    pci_enable_bus_mastering(node);

    /* Extract LFB physical base from BAR0 */
    phys_addr_t fb_phys = pci_get_bar(node, 0);
    if (!fb_phys) {
        pr_debug("[BGA] Error: Invalid BAR0 for framebuffer.\n");
        return -ENODEV;
    }

    g_bga.fb_phys = fb_phys;

    /* Map it outside of HHDM to avoid shattering HHDM huge pages.
     * 0xFFFFC00000000000 is safely above the HHDM limit. */
    phys_addr_t fb_aligned = ALIGN_DOWN(fb_phys, 4096);
    virt_addr_t fb_virt = 0xFFFFC00000000000;

    for (uint32_t offset = 0; offset < BGA_APERTURE_SIZE; offset += 4096) {
        vmm_map(0, fb_virt + offset, fb_aligned + offset, VMM_MMIO);
    }
    g_bga.fb_virt = fb_virt;

    uint16_t bga_id = bga_read_reg(VBE_DISPI_INDEX_ID);
    g_bga.version = bga_id;
    pr_debug("[BGA] Device Version ID: 0x%04X\n", bga_id);

    /* Set up display resolution (1280x800x32, enable LFB) */
    bga_set_video_mode(1280, 800, 32, 1);

    /* Clear screen to black as an example usage */
    bga_clear_screen(0x00000000);

    device_create("BGA0", DEVICE_TYPE_DISPLAY, node);
    devfs_register_device("fb1", &bga_fops, &g_bga);
    dm_set_drvdata(dm, &g_bga);
    return 0;
}

static void bga_remove(dm_device_t *dm)
{
    (void)dm;
}

static const pci_device_id_t bga_pci_ids[] = {
    { PCI_DEVICE(0x1234, 0x1111) },   /* QEMU/Bochs standard VGA */
    { 0 }
};

static pci_driver_t bga_pci_driver = {
    .drv      = { .name = "bga" },
    .id_table = bga_pci_ids,
    .probe    = bga_probe,
    .remove   = bga_remove,
};

/** bga_init() — Register the PCI driver; probe() binds to a matching Bochs
 *  Graphics Adapter automatically, so this is safe to call whether or not
 *  the host has one. */
void bga_init(void)
{
    pci_driver_register(&bga_pci_driver);
}

phys_addr_t bga_get_fb_phys(void) { return g_bga.fb_phys; }
size_t      bga_get_fb_size(void) { return (size_t)(g_bga.pitch * g_bga.height); }
size_t      bga_get_fb_total_size(void) { return (size_t)(g_bga.pitch * g_bga.height * 2); }

int bga_flip_buffer(uint32_t buffer_index)
{
    if (buffer_index > 1) return -1;
    bga_write_reg(VBE_DISPI_INDEX_Y_OFFSET, (uint16_t)(buffer_index * g_bga.height));
    return 0;
}

virt_addr_t bga_get_fb_virt(void) { return g_bga.fb_virt; }

size_t bga_get_vram_size(void)
{
    if (!g_bga.fb_phys) return 0;

    /* The adapter reports its memory in 64 KiB units.  Nothing beyond the
     * mapped aperture is reachable, so that is the ceiling. */
    size_t size = (size_t)bga_read_reg(VBE_DISPI_INDEX_VIDEO_MEMORY_64K) * 64 * 1024;
    if (size == 0 || size > BGA_APERTURE_SIZE) size = BGA_APERTURE_SIZE;
    return size;
}

uint32_t    bga_get_width(void)   { return g_bga.width; }
uint32_t    bga_get_height(void)  { return g_bga.height; }
uint32_t    bga_get_pitch(void)   { return g_bga.pitch; }
uint8_t     bga_get_bpp(void)     { return g_bga.bpp; }

