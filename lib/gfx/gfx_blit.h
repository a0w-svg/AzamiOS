/**
 * lib/gfx/gfx_blit.h  –  AzamiOS software renderer (blitting layer)
 *
 * Kernel-independent: all drawing operates on a caller-supplied
 * uint32_t backbuffer array — no VGA ports, no paging_map_page, no MMIO.
 *
 * The kernel (gfx_hw.c) owns the hardware initialisation and the
 * physical LFB mapping; it passes the backbuffer pointer here.
 * Userspace or a future window-compositor could do the same.
 */
#ifndef LIB_GFX_BLIT_H
#define LIB_GFX_BLIT_H

#include <stdint.h>

/* Default resolution — override with -DGFX_WIDTH=… -DGFX_HEIGHT=… */
#ifndef GFX_WIDTH
#  define GFX_WIDTH  640
#endif
#ifndef GFX_HEIGHT
#  define GFX_HEIGHT 480
#endif

/**
 * gfx_blit_ctx_t – blitting context passed to every draw call.
 *
 * The kernel fills this from gfx_hw.c after the LFB is mapped.
 * Unit tests can fill it with a heap-allocated buffer.
 */
typedef struct {
    uint32_t *backbuffer; /* width × height array of ARGB pixels   */
    int        width;     /* framebuffer width  in pixels           */
    int        height;    /* framebuffer height in pixels           */
} gfx_blit_ctx_t;

/* ── Drawing primitives ───────────────────────────────────────────── */

/** Plot a single pixel into the backbuffer. Clips silently. */
void gfx_blit_put_pixel(gfx_blit_ctx_t *ctx, int x, int y, uint32_t color);

/** Fill an axis-aligned rectangle. Clips to framebuffer bounds. */
void gfx_blit_rect(gfx_blit_ctx_t *ctx,
                   int x, int y, int w, int h, uint32_t color);

/** Clear the entire backbuffer to color. */
void gfx_blit_clear(gfx_blit_ctx_t *ctx, uint32_t color);

/** Draw a single 8×8 glyph at (x,y). bg_color=0xFFFFFFFF → transparent. */
void gfx_blit_char(gfx_blit_ctx_t *ctx,
                   int x, int y, char c,
                   uint32_t fg_color, uint32_t bg_color);

/** Draw a NUL-terminated string, advancing 8px per glyph. */
void gfx_blit_text(gfx_blit_ctx_t *ctx,
                   int x, int y, const char *str,
                   uint32_t fg_color, uint32_t bg_color);

/** Draw a line from (x0,y0) to (x1,y1) using Bresenham's algorithm. */
void gfx_blit_line(gfx_blit_ctx_t *ctx, int x0, int y0, int x1, int y1, uint32_t color);


/** Draw a circle outline at (xc,yc) with radius r. */
void gfx_blit_circle(gfx_blit_ctx_t *ctx, int xc, int yc, int r, uint32_t color);

/** Fill a circle at (xc,yc) with radius r. */
void gfx_blit_fill_circle(gfx_blit_ctx_t *ctx, int xc, int yc, int r, uint32_t color);

/**
 * gfx_blit_flip – copy the backbuffer to the linear framebuffer.
 * @param lfb  destination (physical/virtual LFB pointer, set by gfx_hw.c).
 */
void gfx_blit_flip(gfx_blit_ctx_t *ctx, uint32_t *lfb);

#endif /* LIB_GFX_BLIT_H */

