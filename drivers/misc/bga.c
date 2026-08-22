/* ============================================================================
 * AzamiOS — Bochs Graphics Adapter (BGA) Driver
 * File: drivers/bga.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "bga.h"
#include "../char/console.h"
#include "../../hal/pci.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/types.h"
#include "../../include/azami/fb.h"
#include "../../fs/vfs.h"
#include "../../kernel/sched/sched.h"


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
        volatile uint32_t *fb = (volatile uint32_t *)g_bga.fb_virt;
        uint32_t pixels = g_bga.width * g_bga.height * 2; /* Clear both double-buffer pages */
        for (uint32_t i = 0; i < pixels; i++) {
            fb[i] = color;
        }
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
        default:
            return -1;
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

static void bga_scan_tree(device_t *node)
{
    if (!node) return;

    pci_device_info_t *pci = pci_get_device_info(node);
    if (pci) {
        if (pci->vendor_id == 0x1234 && pci->device_id == 0x1111) {
            pr_debug("[BGA] Found Bochs Graphics Adapter at PCI %02x:%02x.%x\n",
                     pci->bus, pci->slot, pci->func);

            /* Enable Memory Space and Bus Mastering so BAR0 responds to writes */
            pci_enable_bus_mastering(node);

            /* Extract LFB physical base from BAR0 */
            phys_addr_t fb_phys = pci_get_bar(node, 0);
            if (!fb_phys) {
                pr_debug("[BGA] Error: Invalid BAR0 for framebuffer.\n");
                return;
            }

            g_bga.fb_phys = fb_phys;
            
            /* Map it outside of HHDM to avoid shattering HHDM huge pages.
             * 0xFFFFC00000000000 is safely above the HHDM limit. */
            phys_addr_t fb_aligned = ALIGN_DOWN(fb_phys, 4096);
            virt_addr_t fb_virt = 0xFFFFC00000000000;
            
            for (uint32_t offset = 0; offset < 16 * 1024 * 1024; offset += 4096) {
                vmm_map(0, fb_virt + offset, fb_aligned + offset, VMM_MMIO);
            }
            g_bga.fb_virt = fb_virt;

            uint16_t id = bga_read_reg(VBE_DISPI_INDEX_ID);
            g_bga.version = id;
            pr_debug("[BGA] Device Version ID: 0x%04X\n", id);

            /* Set up display resolution (1280x800x32, enable LFB) */
            bga_set_video_mode(1280, 800, 32, 1);

            /* Clear screen to black as an example usage */
            bga_clear_screen(0x00000000);

            device_create("BGA0", DEVICE_TYPE_DISPLAY, node);
            devfs_register_device("fb1", &bga_fops, &g_bga);
            return;
        }
    }

    device_t *child = node->children;
    while (child) {
        bga_scan_tree(child);
        child = child->sibling;
    }
}

void bga_init(void)
{
    pr_debug("[BGA] Probing for Bochs Graphics Adapter...\n");
    bga_scan_tree(device_tree_root());
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

uint32_t    bga_get_width(void)   { return g_bga.width; }
uint32_t    bga_get_height(void)  { return g_bga.height; }
uint32_t    bga_get_pitch(void)   { return g_bga.pitch; }
uint8_t     bga_get_bpp(void)     { return g_bga.bpp; }

