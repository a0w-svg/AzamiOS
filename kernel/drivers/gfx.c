/**
 * gfx.c  –  High Performance Bochs VBE Linear Framebuffer Driver (640x480x32)
 */
#include "./include/gfx.h"
#include "./include/mouse.h"
#include "../klibc/include/port.h"
#include "../klibc/include/string.h"
#include "../klibc/include/stdio.h"
#include "./include/font8x8.h"
#include "../mem/include/paging.h"

#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

#define VBE_DISPI_INDEX_ID          0
#define VBE_DISPI_INDEX_XRES        1
#define VBE_DISPI_INDEX_YRES        2
#define VBE_DISPI_INDEX_BPP         3
#define VBE_DISPI_INDEX_ENABLE      4

#define VBE_DISPI_DISABLED          0x00
#define VBE_DISPI_ENABLED           0x01
#define VBE_DISPI_LFB_ENABLED       0x40

static bool g_gfx_enabled = false;
static uint32_t gfx_backbuffer[GFX_WIDTH * GFX_HEIGHT] __attribute__((aligned(16)));
static uint32_t *lfb = NULL;

static int g_cursor_x = GFX_WIDTH / 2;
static int g_cursor_y = GFX_HEIGHT / 2;

static void bga_write_reg(uint16_t index, uint16_t val) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, val);
}

static uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    outl(0xCF8, address);
    return inl(0xCFC);
}

static uint32_t bga_locate_lfb(void) {
    for (int slot = 0; slot < 32; slot++) {
        uint32_t vendor_dev = pci_config_read32(0, slot, 0, 0);
        if (vendor_dev == 0x11111234) { /* QEMU/Bochs VGA card */
            uint32_t bar0 = pci_config_read32(0, slot, 0, 0x10);
            return bar0 & 0xFFFFFFF0;
        }
    }
    return 0xFD000000; /* Fallback default QEMU LFB physical base */
}

bool gfx_is_enabled(void) {
    return g_gfx_enabled;
}

void gfx_put_pixel(int x, int y, uint32_t color) {
    if (x < 0 || x >= GFX_WIDTH || y < 0 || y >= GFX_HEIGHT) return;
    gfx_backbuffer[y * GFX_WIDTH + x] = color;
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color) {
    for (int iy = y; iy < y + h; iy++) {
        if (iy < 0 || iy >= GFX_HEIGHT) continue;
        int sx = (x < 0) ? 0 : x;
        int ex = (x + w > GFX_WIDTH) ? GFX_WIDTH : x + w;
        if (sx >= ex) continue;
        uint32_t *dest = &gfx_backbuffer[iy * GFX_WIDTH + sx];
        int cnt = ex - sx;
        asm volatile("rep stosl" : "+D"(dest), "+c"(cnt) : "a"(color) : "memory");
    }
}

void gfx_draw_char(int x, int y, char c, uint32_t color, uint32_t bg_color) {
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < 8; row++) {
        int py = y + row;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= GFX_WIDTH) continue;
            if (bits & (0x80 >> col)) {
                gfx_backbuffer[py * GFX_WIDTH + px] = color;
            } else if (bg_color != 0xFFFFFFFF) { /* 0xFFFFFFFF = transparent */
                gfx_backbuffer[py * GFX_WIDTH + px] = bg_color;
            }
        }
    }
}

void gfx_draw_text(int x, int y, const char *str, uint32_t color, uint32_t bg_color) {
    int cx = x;
    while (*str) {
        gfx_draw_char(cx, y, *str++, color, bg_color);
        cx += 8;
    }
}

void gfx_clear(uint32_t color) {
    uint32_t *dest = gfx_backbuffer;
    int cnt = GFX_WIDTH * GFX_HEIGHT;
    asm volatile("rep stosl" : "+D"(dest), "+c"(cnt) : "a"(color) : "memory");
}

void gfx_flip(void) {
    if (!g_gfx_enabled || !lfb) return;

    /* High speed 32-bit hardware burst transfer of entire 1.2 MB frame */
    uint32_t *dest = lfb;
    const uint32_t *src = gfx_backbuffer;
    int cnt = GFX_WIDTH * GFX_HEIGHT;
    asm volatile("rep movsd" : "+D"(dest), "+S"(src), "+c"(cnt) : : "memory");

    /* Render mouse arrow directly onto LFB */
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
    for (int y = 0; y < 12; y++) {
        int py = g_cursor_y + y;
        if (py < 0 || py >= GFX_HEIGHT) continue;
        for (int x = 0; x < 9; x++) {
            int px = g_cursor_x + x;
            if (px < 0 || px >= GFX_WIDTH) continue;
            uint32_t val = cursor_bmp[y][x];
            if (val == 1) lfb[py * GFX_WIDTH + px] = 0x00000000;
            else if (val == 2) lfb[py * GFX_WIDTH + px] = 0x00FFFFFF;
        }
    }
}

void gfx_on_mouse_move(int x, int y) {
    g_cursor_x = x;
    g_cursor_y = y;
    gfx_flip();
}

void gfx_init_bga(void) {
    kprintf("gfx: initializing Bochs VBE Linear Framebuffer (%dx%dx32)...\n", GFX_WIDTH, GFX_HEIGHT);

    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_DISABLED);
    bga_write_reg(VBE_DISPI_INDEX_XRES, GFX_WIDTH);
    bga_write_reg(VBE_DISPI_INDEX_YRES, GFX_HEIGHT);
    bga_write_reg(VBE_DISPI_INDEX_BPP, 32);
    bga_write_reg(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    uint32_t lfb_phys = bga_locate_lfb();
    kprintf("gfx: LFB located at physical RAM address 0x%x\n", lfb_phys);

    /* Map 1,228,800 bytes (300 pages of 4KB) into page tables */
    uint32_t fb_size_bytes = GFX_WIDTH * GFX_HEIGHT * 4;
    for (uint32_t off = 0; off < fb_size_bytes; off += 4096) {
        paging_map_page(lfb_phys + off, lfb_phys + off, 0, 1);
    }

    lfb = (uint32_t*)lfb_phys;
    g_gfx_enabled = true;

    gfx_clear(0x000F172A);
    gfx_flip();
    kprintf("gfx: Bochs VBE True Color Mode Active\n");
}

void gfx_init_mode13h(void) {
    gfx_init_bga();
}
