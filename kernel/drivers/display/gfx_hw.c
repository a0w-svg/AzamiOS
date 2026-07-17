/**
 * kernel/drivers/gfx_hw.c  –  Bochs VBE LFB hardware initialisation
 *
 * This file is the ONLY part of the graphics stack that needs kernel
 * privileges:
 *   - BGA register writes via outw() (port I/O)
 *   - PCI config space reads via pci_config_read32()
 *   - paging_map_page() to make the LFB accessible
 *   - Mouse cursor overlay on the raw LFB
 *
 * All pure pixel/text drawing is delegated to lib/gfx/gfx_blit.c
 * through the gfx_blit_ctx_t interface — no drawing logic lives here.
 *
 * Public API (declared in kernel/drivers/include/gfx.h) is unchanged,
 * so no other kernel code needs to be modified.
 */
#include "./include/gfx.h"
#include "./include/mouse.h"
#include "./include/pci.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
/* Include the portable blit layer directly (no separate .o needed) */
#include "../../lib/gfx/gfx_blit.c"
#include "../mem/include/paging.h"
#include "../include/virtio_gpu.h"
#include "../arch/include/spinlock.h"

/* ── BGA register indices & values ──────────────────────────────── */
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_ID      0
#define VBE_DISPI_INDEX_XRES    1
#define VBE_DISPI_INDEX_YRES    2
#define VBE_DISPI_INDEX_BPP     3
#define VBE_DISPI_INDEX_ENABLE  4
#define VBE_DISPI_DISABLED      0x00
#define VBE_DISPI_ENABLED       0x01
#define VBE_DISPI_LFB_ENABLED   0x40

/* ── Driver state ────────────────────────────────────────────────── */
static volatile int g_gfx_lock = 0;
static bool g_gfx_enabled = false;
static uint32_t gfx_backbuffer_data[GFX_WIDTH * GFX_HEIGHT] __attribute__((aligned(4096)));
static uint32_t *lfb = (void*)0;

/* Shared blitting context used by the public gfx_* API */
static gfx_blit_ctx_t g_ctx = {
    .backbuffer = gfx_backbuffer_data,
    .width      = GFX_WIDTH,
    .height     = GFX_HEIGHT,
};

/* Mouse cursor position (updated by gfx_on_mouse_move) */
static volatile int g_cursor_x = GFX_WIDTH  / 2;
static volatile int g_cursor_y = GFX_HEIGHT / 2;

/* ── BGA helpers ─────────────────────────────────────────────────── */
static void bga_write_reg(uint16_t index, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA,  val);
}

static uint32_t bga_locate_lfb(void) {
    for (int slot = 0; slot < 32; slot++) {
        uint32_t vendor_dev = pci_config_read32(0, slot, 0, 0);
        if (vendor_dev == 0x11111234) { /* QEMU/Bochs VGA */
            uint32_t bar0 = pci_config_read32(0, slot, 0, 0x10);
            return bar0 & 0xFFFFFFF0;
        }
    }
    return 0xFD000000; /* Fallback QEMU LFB base */
}

/* ── Public gfx API (forwards to gfx_blit_ctx_t) ────────────────── */

bool gfx_is_enabled(void) {
    return __atomic_load_n(&g_gfx_enabled, __ATOMIC_ACQUIRE);
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_put_pixel(&g_ctx, x, y, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_rect(&g_ctx, x, y, w, h, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_draw_char(int x, int y, char c, uint32_t color, uint32_t bg_color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_char(&g_ctx, x, y, c, color, bg_color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_text(&g_ctx, x, y, str, color, bg_color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_line(&g_ctx, x0, y0, x1, y1, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_draw_circle(int xc, int yc, int r, uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_circle(&g_ctx, xc, yc, r, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_fill_circle(int xc, int yc, int r, uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_fill_circle(&g_ctx, xc, yc, r, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_clear(uint32_t color) {
    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);
    gfx_blit_clear(&g_ctx, color);
    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_vsync(void) {
    if (!gfx_is_enabled()) return;
    int guard = 0;
    while ((inb(0x3DA) & 0x08) && guard++ < 1000000) {
        asm volatile("pause" ::: "memory");
    }
    guard = 0;
    while (!(inb(0x3DA) & 0x08) && guard++ < 1000000) {
        asm volatile("pause" ::: "memory");
    }
}

void gfx_flip(void) {
    if (!gfx_is_enabled() || !lfb) return;

    unsigned long flags;
    spinlock_acquire_irqsave(&g_gfx_lock, &flags);

    static const uint32_t cursor_bmp[12][9] = {
        {1,0,0,0,0,0,0,0,0},
        {1,1,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0},
        {1,2,2,1,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0},
        {1,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,0,0},
        {1,2,2,2,2,2,2,1,0},
        {1,2,2,2,1,1,1,1,1},
        {1,2,1,2,1,0,0,0,0},
        {1,1,0,1,2,1,0,0,0},
        {1,0,0,1,1,0,0,0,0}
    };

    uint32_t saved_bg[12][9];
    int cx = __atomic_load_n(&g_cursor_x, __ATOMIC_ACQUIRE);
    int cy = __atomic_load_n(&g_cursor_y, __ATOMIC_ACQUIRE);

    /* Save backbuffer pixels and overlay cursor directly in RAM */
    for (int y = 0; y < 12; y++) {
        int py = cy + y;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int x = 0; x < 9; x++) {
            int px = cx + x;
            if (px < 0 || px >= GFX_WIDTH) continue;
            uint32_t idx = py * GFX_WIDTH + px;
            saved_bg[y][x] = gfx_backbuffer_data[idx];
            uint32_t val = cursor_bmp[y][x];
            if      (val == 1) gfx_backbuffer_data[idx] = 0x00000000;
            else if (val == 2) gfx_backbuffer_data[idx] = 0x00FFFFFF;
        }
    }

    /* Single high-speed burst DMA transfer to video memory */
    if (virtio_gpu_is_active()) {
        virtio_gpu_blit(gfx_backbuffer_data, GFX_WIDTH, GFX_HEIGHT);
    } else {
        gfx_blit_flip(&g_ctx, lfb);
    }

    /* Restore backbuffer RAM so drawing operations are unaffected */
    for (int y = 0; y < 12; y++) {
        int py = cy + y;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int x = 0; x < 9; x++) {
            int px = cx + x;
            if (px < 0 || px >= GFX_WIDTH) continue;
            gfx_backbuffer_data[py * GFX_WIDTH + px] = saved_bg[y][x];
        }
    }

    spinlock_release_irqrestore(&g_gfx_lock, flags);
}

void gfx_on_mouse_move(int x, int y) {
    __atomic_store_n(&g_cursor_x, x, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cursor_y, y, __ATOMIC_RELEASE);
    gfx_flip();
}

/* ── Hardware initialisation ─────────────────────────────────────── */
void gfx_init_bga(void) {
    kprintf("gfx: initializing Bochs VBE Linear Framebuffer (%dx%dx32)...\n",
            GFX_WIDTH, GFX_HEIGHT);

    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_reg(VBE_DISPI_INDEX_XRES,   GFX_WIDTH);
    bga_write_reg(VBE_DISPI_INDEX_YRES,   GFX_HEIGHT);
    bga_write_reg(VBE_DISPI_INDEX_BPP,    32);
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    uint32_t lfb_phys = bga_locate_lfb();
    kprintf("gfx: LFB located at physical address 0x%x\n", lfb_phys);

    /* Map MMIO LFB using a dedicated kernel page table. */
    uint32_t fb_size_bytes = GFX_WIDTH * GFX_HEIGHT * 4;
    paging_map_framebuffer(lfb_phys, fb_size_bytes);

    lfb = (uint32_t*)(uintptr_t)lfb_phys;
    __atomic_store_n(&g_gfx_enabled, true, __ATOMIC_RELEASE);

    gfx_clear(0x000F172A);
    gfx_flip();
    kprintf("gfx: Bochs VBE True Color Mode Active\n");
}

void gfx_init_mode13h(void) {
    gfx_init_bga();
}

/**
 * gfx_init_auto — Auto-detect GPU: try Intel i915 first, fallback to Bochs VBE.
 * Called from userspace init_graphics() or kernel boot.
 */
void gfx_init_auto(void) {
    /* Try Intel i915 KMS first (no-op if no Intel GPU found) */
    extern void gfx_init_i915(void);
    gfx_init_i915();

    /* Always init BGA as the actual framebuffer driver for QEMU */
    gfx_init_bga();
}

uint32_t gfx_map_backbuffer(void) {
    uintptr_t phys = (uintptr_t)gfx_backbuffer_data;
    uintptr_t virt_base = 0xB0000000;
    uint32_t num_pages = (GFX_WIDTH * GFX_HEIGHT * 4 + 4095) / 4096;
    for (uint32_t i = 0; i < num_pages; i++) {
        paging_map_page(phys + i * 4096, virt_base + i * 4096, 0, 1);
    }
    kprintf("gfx: mapped backbuffer 0x%x to ring-3 virt 0x%x (%d pages)\n", (uint32_t)phys, (uint32_t)virt_base, num_pages);
    return (uint32_t)virt_base;
}

