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
static bool g_gfx_enabled = false;
static uint32_t gfx_backbuffer_data[GFX_WIDTH * GFX_HEIGHT] __attribute__((aligned(16)));
static uint32_t *lfb = (void*)0;

/* Shared blitting context used by the public gfx_* API */
static gfx_blit_ctx_t g_ctx = {
    .backbuffer = gfx_backbuffer_data,
    .width      = GFX_WIDTH,
    .height     = GFX_HEIGHT,
};

/* Mouse cursor position (updated by gfx_on_mouse_move) */
static int g_cursor_x = GFX_WIDTH  / 2;
static int g_cursor_y = GFX_HEIGHT / 2;

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

bool gfx_is_enabled(void) { return g_gfx_enabled; }

void gfx_put_pixel(int x, int y, uint32_t color) {
    gfx_blit_put_pixel(&g_ctx, x, y, color);
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    gfx_blit_rect(&g_ctx, x, y, w, h, color);
}

void gfx_draw_char(int x, int y, char c, uint32_t color, uint32_t bg_color) {
    gfx_blit_char(&g_ctx, x, y, c, color, bg_color);
}

void gfx_draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    gfx_blit_text(&g_ctx, x, y, str, color, bg_color);
}

void gfx_draw_line(int x0, int y0, int x1, int y1, uint32_t color) {
    gfx_blit_line(&g_ctx, x0, y0, x1, y1, color);
}

void gfx_draw_circle(int xc, int yc, int r, uint32_t color) {
    gfx_blit_circle(&g_ctx, xc, yc, r, color);
}

void gfx_fill_circle(int xc, int yc, int r, uint32_t color) {
    gfx_blit_fill_circle(&g_ctx, xc, yc, r, color);
}

void gfx_clear(uint32_t color) {
    gfx_blit_clear(&g_ctx, color);
}

void gfx_flip(void) {
    if (!g_gfx_enabled || !lfb) return;

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

    /* Save backbuffer pixels and overlay cursor directly in RAM */
    for (int y = 0; y < 12; y++) {
        int py = g_cursor_y + y;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int x = 0; x < 9; x++) {
            int px = g_cursor_x + x;
            if (px < 0 || px >= GFX_WIDTH) continue;
            uint32_t idx = py * GFX_WIDTH + px;
            saved_bg[y][x] = gfx_backbuffer_data[idx];
            uint32_t val = cursor_bmp[y][x];
            if      (val == 1) gfx_backbuffer_data[idx] = 0x00000000;
            else if (val == 2) gfx_backbuffer_data[idx] = 0x00FFFFFF;
        }
    }

    /* Single high-speed burst DMA transfer to video memory */
    gfx_blit_flip(&g_ctx, lfb);

    /* Restore backbuffer RAM so drawing operations are unaffected */
    for (int y = 0; y < 12; y++) {
        int py = g_cursor_y + y;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int x = 0; x < 9; x++) {
            int px = g_cursor_x + x;
            if (px < 0 || px >= GFX_WIDTH) continue;
            gfx_backbuffer_data[py * GFX_WIDTH + px] = saved_bg[y][x];
        }
    }
}


void gfx_on_mouse_move(int x, int y) {
    g_cursor_x = x;
    g_cursor_y = y;
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

    /* Map the framebuffer into the page tables (300 × 4 KB pages = 1.2 MB) */
    uint32_t fb_size_bytes = GFX_WIDTH * GFX_HEIGHT * 4;
    for (uint32_t off = 0; off < fb_size_bytes; off += 4096)
        paging_map_page(lfb_phys + off, lfb_phys + off, 0, 1);

    lfb = (uint32_t*)lfb_phys;
    g_gfx_enabled = true;

    kprintf("gfx: Testing LFB read/write at 0x%x...\n", lfb_phys);
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kprintf("gfx: current cr3=0x%x\n", cr3);
    volatile uint32_t *test_ptr = (volatile uint32_t *)lfb_phys;
    *test_ptr = 0x12345678;
    kprintf("gfx: successfully wrote to LFB, read back: 0x%x\n", *test_ptr);

    gfx_clear(0x000F172A);
    gfx_flip();
    kprintf("gfx: Bochs VBE True Color Mode Active\n");
}

void gfx_init_mode13h(void) {
    gfx_init_bga();
}
