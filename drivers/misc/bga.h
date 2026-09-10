/* ============================================================================
 * AzamiOS — Bochs Graphics Adapter (BGA) Driver
 * File: drivers/misc/bga.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"
#include "../../hal/device.h"

/* I/O Ports */
#define VBE_DISPI_IOPORT_INDEX 0x01CE
#define VBE_DISPI_IOPORT_DATA  0x01CF

/* BGA Registers */
#define VBE_DISPI_INDEX_ID           0x0
#define VBE_DISPI_INDEX_XRES         0x1
#define VBE_DISPI_INDEX_YRES         0x2
#define VBE_DISPI_INDEX_BPP          0x3
#define VBE_DISPI_INDEX_ENABLE       0x4
#define VBE_DISPI_INDEX_BANK         0x5
#define VBE_DISPI_INDEX_VIRT_WIDTH   0x6
#define VBE_DISPI_INDEX_VIRT_HEIGHT  0x7
#define VBE_DISPI_INDEX_X_OFFSET     0x8
#define VBE_DISPI_INDEX_Y_OFFSET     0x9
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 0xA

/* BGA Magic IDs */
#define VBE_DISPI_ID0          0xB0C0
#define VBE_DISPI_ID4          0xB0C4
#define VBE_DISPI_ID5          0xB0C5

/* BGA Enable bits */
#define VBE_DISPI_DISABLED     0x00
#define VBE_DISPI_ENABLED      0x01
#define VBE_DISPI_GETCAPS      0x02
#define VBE_DISPI_8BIT_DAC     0x20
#define VBE_DISPI_LFB_ENABLED  0x40
#define VBE_DISPI_NOCLEARMEM   0x80

/* External Architecture-Specific Dependencies */
extern void outw(uint16_t port, uint16_t value);
extern uint16_t inw(uint16_t port);

/* Bytes of the adapter aperture the driver maps into kernel virtual space. */
#define BGA_APERTURE_SIZE  (16 * 1024 * 1024)

/* Driver API */
void bga_init(void);
void bga_set_video_mode(uint32_t width, uint32_t height, uint32_t bit_depth, uint8_t enable_lfb);
void bga_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void bga_clear_screen(uint32_t color);

/** bga_get_fb_phys() — Physical base address of the BGA LFB, or 0 if not initialised. */
phys_addr_t bga_get_fb_phys(void);

/** bga_get_fb_size() — Byte size of a single BGA framebuffer page (pitch * height). */
size_t bga_get_fb_size(void);

/** bga_get_fb_total_size() — Total double-buffered VRAM size (2 * pitch * height). */
size_t bga_get_fb_total_size(void);

/**
 * bga_get_fb_virt() — Kernel virtual base of the LFB aperture.
 *
 * The driver maps the adapter's memory outside the HHDM (which only covers
 * the boot framebuffer), so anything writing past the first screen must go
 * through this pointer rather than PHYS_TO_VIRT().  Returns 0 if the adapter
 * was not found.
 */
virt_addr_t bga_get_fb_virt(void);

/**
 * bga_get_vram_size() — Bytes of video memory actually present.
 *
 * Read from the adapter rather than inferred from the mode, and capped to the
 * aperture the driver mapped.
 */
size_t bga_get_vram_size(void);

/** bga_flip_buffer(buffer_index) — Zero-copy hardware flip to VRAM buffer 0 or 1. */
int bga_flip_buffer(uint32_t buffer_index);

uint32_t bga_get_width(void);
uint32_t bga_get_height(void);
uint32_t bga_get_pitch(void);
uint8_t  bga_get_bpp(void);

