/* ============================================================================
 * AzamiOS — Modular 2D Graphics & Rasterization Pipeline
 * File: userland/apps/shared/gfx_pipeline.h
 *
 * High-performance Linux-style 2D rendering pipeline supporting:
 *  - ARGB32 software rasterization with alpha blending (Porter-Duff SRC_OVER)
 *  - Antialiased lines, circles, rounded rectangles, gradients, and drop shadows
 *  - Bilinear and nearest-neighbor surface scaling & blitting
 *  - Dirty-rectangle damage clipping and incremental compositing
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define GFX_MAX_CLIPS 16

typedef struct {
    int x, y, w, h;
} gfx_rect_t;

typedef struct {
    uint32_t   *pixels;
    int         width;
    int         height;
    int         pitch;      /* in bytes */
    int         stride;     /* in 32-bit pixels (pitch / 4) */
    gfx_rect_t  clip;
    gfx_rect_t  clip_stack[GFX_MAX_CLIPS];
    int         clip_depth;
} gfx_surface_t;

typedef struct {
    gfx_rect_t rects[32];
    int        count;
    int        bounds_min_x, bounds_min_y;
    int        bounds_max_x, bounds_max_y;
    bool       has_damage;
} gfx_damage_tracker_t;

/* ── Color Helpers ────────────────────────────────────────────────────────── */
static inline uint32_t gfx_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t gfx_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static inline uint32_t gfx_blend_pixel(uint32_t dst, uint32_t src)
{
    uint32_t sa = (src >> 24) & 0xFF;
    if (sa == 255) return src;
    if (sa == 0)   return dst;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8)  & 0xFF;
    uint32_t sb = src         & 0xFF;

    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8)  & 0xFF;
    uint32_t db = dst         & 0xFF;
    uint32_t da = (dst >> 24) & 0xFF;

    uint32_t inv_a = 255 - sa;
    uint32_t out_r = (sr * sa + dr * inv_a + 127) / 255;
    uint32_t out_g = (sg * sa + dg * inv_a + 127) / 255;
    uint32_t out_b = (sb * sa + db * inv_a + 127) / 255;
    uint32_t out_a = sa + (da * inv_a + 127) / 255;

    return (out_a << 24) | (out_r << 16) | (out_g << 8) | out_b;
}

static inline uint32_t gfx_lerp_color(uint32_t c1, uint32_t c2, int t, int max_t)
{
    if (max_t <= 0) return c1;
    if (t <= 0) return c1;
    if (t >= max_t) return c2;

    int a1 = (c1 >> 24) & 0xFF, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
    int a2 = (c2 >> 24) & 0xFF, r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;

    int a = a1 + ((a2 - a1) * t) / max_t;
    int r = r1 + ((r2 - r1) * t) / max_t;
    int g = g1 + ((g2 - g1) * t) / max_t;
    int b = b1 + ((b2 - b1) * t) / max_t;

    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* ── Surface Lifecycle ────────────────────────────────────────────────────── */
static inline void gfx_surface_init(gfx_surface_t *surf, uint32_t *pixels, int w, int h, int pitch)
{
    surf->pixels     = pixels;
    surf->width      = w;
    surf->height     = h;
    surf->pitch      = pitch;
    surf->stride     = pitch / 4;
    surf->clip       = (gfx_rect_t){ .x = 0, .y = 0, .w = w, .h = h };
    surf->clip_depth = 0;
}

static inline void gfx_push_clip(gfx_surface_t *surf, gfx_rect_t r)
{
    if (surf->clip_depth < GFX_MAX_CLIPS) {
        surf->clip_stack[surf->clip_depth++] = surf->clip;
    }
    /* Intersect current clip with new rect */
    int x1 = surf->clip.x > r.x ? surf->clip.x : r.x;
    int y1 = surf->clip.y > r.y ? surf->clip.y : r.y;
    int x2 = (surf->clip.x + surf->clip.w) < (r.x + r.w) ? (surf->clip.x + surf->clip.w) : (r.x + r.w);
    int y2 = (surf->clip.y + surf->clip.h) < (r.y + r.h) ? (surf->clip.y + surf->clip.h) : (r.y + r.h);

    if (x2 < x1) x2 = x1;
    if (y2 < y1) y2 = y1;
    surf->clip = (gfx_rect_t){ .x = x1, .y = y1, .w = x2 - x1, .h = y2 - y1 };
}

static inline void gfx_pop_clip(gfx_surface_t *surf)
{
    if (surf->clip_depth > 0) {
        surf->clip = surf->clip_stack[--surf->clip_depth];
    }
}

/* ── Primitives ───────────────────────────────────────────────────────────── */

static inline void gfx_draw_pixel(gfx_surface_t *surf, int x, int y, uint32_t color)
{
    if (x < surf->clip.x || x >= surf->clip.x + surf->clip.w ||
        y < surf->clip.y || y >= surf->clip.y + surf->clip.h) return;

    uint32_t *p = &surf->pixels[y * surf->stride + x];
    *p = gfx_blend_pixel(*p, color);
}

static inline void gfx_clear(gfx_surface_t *surf, uint32_t color)
{
    int stride = surf->stride;
    int h = surf->height;
    int w = surf->width;
    for (int y = 0; y < h; y++) {
        uint32_t *row = &surf->pixels[y * stride];
        for (int x = 0; x < w; x++) {
            row[x] = color;
        }
    }
}

static inline void gfx_fill_rect(gfx_surface_t *surf, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0) return;
    int x1 = x < surf->clip.x ? surf->clip.x : x;
    int y1 = y < surf->clip.y ? surf->clip.y : y;
    int x2 = (x + w) > (surf->clip.x + surf->clip.w) ? (surf->clip.x + surf->clip.w) : (x + w);
    int y2 = (y + h) > (surf->clip.y + surf->clip.h) ? (surf->clip.y + surf->clip.h) : (y + h);

    if (x1 >= x2 || y1 >= y2) return;

    bool opaque = ((color >> 24) & 0xFF) == 255;
    int stride = surf->stride;

    for (int py = y1; py < y2; py++) {
        uint32_t *row = &surf->pixels[py * stride + x1];
        int count = x2 - x1;
        if (opaque) {
            for (int px = 0; px < count; px++) row[px] = color;
        } else {
            for (int px = 0; px < count; px++) row[px] = gfx_blend_pixel(row[px], color);
        }
    }
}

static inline void gfx_draw_rect(gfx_surface_t *surf, int x, int y, int w, int h, int thickness, uint32_t color)
{
    if (thickness <= 0) return;
    gfx_fill_rect(surf, x, y, w, thickness, color);                         /* Top */
    gfx_fill_rect(surf, x, y + h - thickness, w, thickness, color);         /* Bottom */
    gfx_fill_rect(surf, x, y + thickness, thickness, h - 2 * thickness, color); /* Left */
    gfx_fill_rect(surf, x + w - thickness, y + thickness, thickness, h - 2 * thickness, color); /* Right */
}

static inline void gfx_draw_line(gfx_surface_t *surf, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        gfx_draw_pixel(surf, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

static inline void gfx_draw_gradient_linear(gfx_surface_t *surf, int x, int y, int w, int h,
                                           uint32_t col_start, uint32_t col_end, bool vertical)
{
    if (w <= 0 || h <= 0) return;
    if (vertical) {
        for (int i = 0; i < h; i++) {
            uint32_t c = gfx_lerp_color(col_start, col_end, i, h - 1);
            gfx_fill_rect(surf, x, y + i, w, 1, c);
        }
    } else {
        for (int i = 0; i < w; i++) {
            uint32_t c = gfx_lerp_color(col_start, col_end, i, w - 1);
            gfx_fill_rect(surf, x + i, y, 1, h, c);
        }
    }
}

static inline void gfx_fill_rounded_rect(gfx_surface_t *surf, int x, int y, int w, int h, int r, uint32_t color)
{
    if (r <= 0) {
        gfx_fill_rect(surf, x, y, w, h, color);
        return;
    }
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;

    /* Central and side rectangles */
    gfx_fill_rect(surf, x + r, y, w - 2 * r, h, color);
    gfx_fill_rect(surf, x, y + r, r, h - 2 * r, color);
    gfx_fill_rect(surf, x + w - r, y + r, r, h - 2 * r, color);

    /* 4 Corner Quarters */
    int r2 = r * r;
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int cx = r - 1 - dx;
            int cy = r - 1 - dy;
            if (cx * cx + cy * cy <= r2) {
                gfx_draw_pixel(surf, x + dx, y + dy, color);                 /* Top-Left */
                gfx_draw_pixel(surf, x + w - 1 - dx, y + dy, color);         /* Top-Right */
                gfx_draw_pixel(surf, x + dx, y + h - 1 - dy, color);         /* Bottom-Left */
                gfx_draw_pixel(surf, x + w - 1 - dx, y + h - 1 - dy, color); /* Bottom-Right */
            }
        }
    }
}

static inline void gfx_draw_shadow(gfx_surface_t *surf, int x, int y, int w, int h, int radius, uint8_t max_alpha)
{
    if (radius <= 0) return;
    for (int i = 1; i <= radius; i++) {
        uint8_t a = (uint8_t)((max_alpha * (radius - i + 1)) / (radius * 2));
        uint32_t col = ((uint32_t)a << 24);
        gfx_fill_rounded_rect(surf, x - i, y - i + 2, w + 2 * i, h + 2 * i, 10 + i, col);
    }
}

/* ── Surface Blitting & Scaling ───────────────────────────────────────────── */

static inline void gfx_blit_surface(gfx_surface_t *dst, int dx, int dy,
                                   const gfx_surface_t *src, int sx, int sy, int sw, int sh)
{
    if (!dst || !src || sw <= 0 || sh <= 0) return;

    for (int r = 0; r < sh; r++) {
        int py = dy + r;
        int s_py = sy + r;
        if (py < dst->clip.y || py >= dst->clip.y + dst->clip.h) continue;
        if (s_py < 0 || s_py >= src->height) continue;

        for (int c = 0; c < sw; c++) {
            int px = dx + c;
            int s_px = sx + c;
            if (px < dst->clip.x || px >= dst->clip.x + dst->clip.w) continue;
            if (s_px < 0 || s_px >= src->width) continue;

            uint32_t src_col = src->pixels[s_py * src->stride + s_px];
            uint32_t *dst_ptr = &dst->pixels[py * dst->stride + px];
            *dst_ptr = gfx_blend_pixel(*dst_ptr, src_col);
        }
    }
}

static inline void gfx_scale_surface_nearest(gfx_surface_t *dst, const gfx_surface_t *src)
{
    if (!dst || !src || dst->width <= 0 || dst->height <= 0 || src->width <= 0 || src->height <= 0) return;

    int dw = dst->width, dh = dst->height;
    int sw = src->width, sh = src->height;

    for (int y = 0; y < dh; y++) {
        int sy = (y * sh) / dh;
        const uint32_t *src_row = &src->pixels[sy * src->stride];
        uint32_t *dst_row = &dst->pixels[y * dst->stride];
        for (int x = 0; x < dw; x++) {
            int sx = (x * sw) / dw;
            dst_row[x] = src_row[sx];
        }
    }
}

/* ── Damage Tracking ──────────────────────────────────────────────────────── */

static inline void gfx_damage_init(gfx_damage_tracker_t *dt)
{
    dt->count        = 0;
    dt->bounds_min_x = 0;
    dt->bounds_min_y = 0;
    dt->bounds_max_x = 0;
    dt->bounds_max_y = 0;
    dt->has_damage   = false;
}

static inline void gfx_damage_add(gfx_damage_tracker_t *dt, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return;
    int x2 = x + w;
    int y2 = y + h;

    if (!dt->has_damage) {
        dt->bounds_min_x = x;
        dt->bounds_min_y = y;
        dt->bounds_max_x = x2;
        dt->bounds_max_y = y2;
        dt->has_damage   = true;
    } else {
        if (x < dt->bounds_min_x) dt->bounds_min_x = x;
        if (y < dt->bounds_min_y) dt->bounds_min_y = y;
        if (x2 > dt->bounds_max_x) dt->bounds_max_x = x2;
        if (y2 > dt->bounds_max_y) dt->bounds_max_y = y2;
    }

    if (dt->count < 32) {
        dt->rects[dt->count++] = (gfx_rect_t){ .x = x, .y = y, .w = w, .h = h };
    }
}
