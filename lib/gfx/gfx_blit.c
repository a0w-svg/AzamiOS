/**
 * lib/gfx/gfx_blit.c  –  AzamiOS software renderer
 *
 * Pure blitting operations on a caller-supplied uint32_t backbuffer.
 * No VGA ports, no paging_map_page, no kernel headers.
 *
 * The font data (font8x8.h) is self-contained static tables and the
 * font_get_glyph() inline — also no external kernel deps.
 *
 * Compiles with: i686-elf-gcc -ffreestanding  OR  host gcc for testing.
 */
#include "gfx_blit.h"
#include <stdint.h>

/* ── Embedded 8×8 font (copied from kernel/drivers/include/font8x8.h) ── */

static const uint8_t font_digits[10][8] = {
    {0x3c,0x66,0x6e,0x76,0x66,0x66,0x3c,0x00}, // 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7e,0x00}, // 1
    {0x3c,0x66,0x0c,0x18,0x30,0x60,0x7e,0x00}, // 2
    {0x3c,0x66,0x0c,0x0c,0x06,0x66,0x3c,0x00}, // 3
    {0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00}, // 4
    {0x7e,0x60,0x7c,0x06,0x06,0x66,0x3c,0x00}, // 5
    {0x3c,0x66,0x60,0x7c,0x66,0x66,0x3c,0x00}, // 6
    {0x7e,0x66,0x0c,0x18,0x18,0x18,0x18,0x00}, // 7
    {0x3c,0x66,0x66,0x3c,0x66,0x66,0x3c,0x00}, // 8
    {0x3c,0x66,0x66,0x3e,0x06,0x66,0x3c,0x00}  // 9
};

static const uint8_t font_letters[26][8] = {
    {0x3c,0x66,0x66,0x7e,0x66,0x66,0x66,0x00}, // A
    {0x7c,0x66,0x66,0x7c,0x66,0x66,0x7c,0x00}, // B
    {0x3c,0x66,0x60,0x60,0x60,0x66,0x3c,0x00}, // C
    {0x78,0x6c,0x66,0x66,0x66,0x6c,0x78,0x00}, // D
    {0x7e,0x60,0x60,0x78,0x60,0x60,0x7e,0x00}, // E
    {0x7e,0x60,0x60,0x78,0x60,0x60,0x60,0x00}, // F
    {0x3c,0x66,0x60,0x6e,0x66,0x66,0x3c,0x00}, // G
    {0x66,0x66,0x66,0x7e,0x66,0x66,0x66,0x00}, // H
    {0x3c,0x18,0x18,0x18,0x18,0x18,0x3c,0x00}, // I
    {0x1e,0x0c,0x0c,0x0c,0x0c,0x6c,0x38,0x00}, // J
    {0x66,0x6c,0x78,0x70,0x78,0x6c,0x66,0x00}, // K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7e,0x00}, // L
    {0x63,0x77,0x7f,0x6b,0x63,0x63,0x63,0x00}, // M
    {0x66,0x76,0x7e,0x7e,0x6e,0x66,0x66,0x00}, // N
    {0x3c,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // O
    {0x7c,0x66,0x66,0x7c,0x60,0x60,0x60,0x00}, // P
    {0x3c,0x66,0x66,0x66,0x6a,0x6c,0x36,0x00}, // Q
    {0x7c,0x66,0x66,0x7c,0x78,0x6c,0x66,0x00}, // R
    {0x3c,0x66,0x60,0x3c,0x06,0x66,0x3c,0x00}, // S
    {0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3c,0x00}, // U
    {0x66,0x66,0x66,0x66,0x3c,0x3c,0x18,0x00}, // V
    {0x63,0x63,0x63,0x6b,0x7f,0x77,0x63,0x00}, // W
    {0x66,0x66,0x3c,0x18,0x3c,0x66,0x66,0x00}, // X
    {0x66,0x66,0x66,0x3c,0x18,0x18,0x18,0x00}, // Y
    {0x7e,0x0c,0x18,0x30,0x60,0xc0,0x7e,0x00}  // Z
};

static const uint8_t font_space[8]  = {0,0,0,0,0,0,0,0};
static const uint8_t font_colon[8]  = {0,0x18,0x18,0,0,0x18,0x18,0};
static const uint8_t font_dot[8]    = {0,0,0,0,0,0x18,0x18,0};
static const uint8_t font_dash[8]   = {0,0,0,0x7e,0,0,0,0};
static const uint8_t font_lbr[8]    = {0x3c,0x30,0x30,0x30,0x30,0x30,0x3c,0};
static const uint8_t font_rbr[8]    = {0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0};
static const uint8_t font_excl[8]   = {0x18,0x3c,0x3c,0x18,0x18,0,0x18,0};
static const uint8_t font_slash[8]  = {0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0};
static const uint8_t font_plus[8]   = {0,0x18,0x18,0x7e,0x18,0x18,0,0};
static const uint8_t font_pipe[8]   = {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0};
static const uint8_t font_star[8]   = {0,0x66,0x3c,0xff,0x3c,0x66,0,0};
static const uint8_t font_gt[8]     = {0x40,0x20,0x10,0x08,0x10,0x20,0x40,0};
static const uint8_t font_eq[8]     = {0,0,0x7e,0,0x7e,0,0,0};
static const uint8_t font_tilde[8]  = {0,0x54,0x78,0,0,0,0,0};
static const uint8_t font_at[8]     = {0x3c,0x42,0x9a,0xaa,0xaa,0x5c,0x38,0};
static const uint8_t font_dollar[8] = {0x18,0x3e,0x60,0x3c,0x06,0x7c,0x18,0};
static const uint8_t font_under[8]  = {0,0,0,0,0,0,0xff,0};

static inline const uint8_t* font_get_glyph(char c) {
    if (c >= '0' && c <= '9') return font_digits[c - '0'];
    if (c >= 'A' && c <= 'Z') return font_letters[c - 'A'];
    if (c >= 'a' && c <= 'z') return font_letters[c - 'a'];
    if (c == ':') return font_colon;
    if (c == '.') return font_dot;
    if (c == '-') return font_dash;
    if (c == '[') return font_lbr;
    if (c == ']') return font_rbr;
    if (c == '!') return font_excl;
    if (c == '/') return font_slash;
    if (c == '+') return font_plus;
    if (c == '|') return font_pipe;
    if (c == '*') return font_star;
    if (c == '>') return font_gt;
    if (c == '=') return font_eq;
    if (c == '~') return font_tilde;
    if (c == '@') return font_at;
    if (c == '$') return font_dollar;
    if (c == '_') return font_under;
    return font_space;
}

/* ── Drawing Operations ──────────────────────────────────────────── */

void gfx_blit_put_pixel(gfx_blit_ctx_t *ctx, int x, int y, uint32_t color) {
    if (x < 0 || x >= ctx->width || y < 0 || y >= ctx->height) return;
    ctx->backbuffer[y * ctx->width + x] = color;
}

void gfx_blit_rect(gfx_blit_ctx_t *ctx,
                   int x, int y, int w, int h, uint32_t color) {
    for (int iy = y; iy < y + h; iy++) {
        if (iy < 0 || iy >= ctx->height) continue;
        int sx = (x < 0)              ? 0          : x;
        int ex = (x + w > ctx->width) ? ctx->width : x + w;
        if (sx >= ex) continue;
        uint32_t *dest = &ctx->backbuffer[iy * ctx->width + sx];
        int cnt = ex - sx;
        /* Fast 32-bit fill via rep stosl */
        asm volatile("rep stosl" : "+D"(dest), "+c"(cnt) : "a"(color) : "memory");
    }
}

void gfx_blit_clear(gfx_blit_ctx_t *ctx, uint32_t color) {
    uint32_t *dest = ctx->backbuffer;
    int cnt = ctx->width * ctx->height;
    asm volatile("rep stosl" : "+D"(dest), "+c"(cnt) : "a"(color) : "memory");
}

void gfx_blit_char(gfx_blit_ctx_t *ctx,
                   int x, int y, char c,
                   uint32_t fg_color, uint32_t bg_color) {
    const uint8_t *glyph = font_get_glyph(c);
    for (int row = 0; row < 8; row++) {
        int py = y + row;
        if (py < 0 || py >= ctx->height) continue;
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= ctx->width) continue;
            if (bits & (0x80 >> col))
                ctx->backbuffer[py * ctx->width + px] = fg_color;
            else if (bg_color != 0xFFFFFFFF)
                ctx->backbuffer[py * ctx->width + px] = bg_color;
        }
    }
}

void gfx_blit_text(gfx_blit_ctx_t *ctx,
                   int x, int y, const char *str,
                   uint32_t fg_color, uint32_t bg_color) {
    int cx = x;
    while (*str) {
        gfx_blit_char(ctx, cx, y, *str++, fg_color, bg_color);
        cx += 8;
    }
}

static int _abs(int v) { return v < 0 ? -v : v; }

void gfx_blit_line(gfx_blit_ctx_t *ctx, int x0, int y0, int x1, int y1, uint32_t color) {
    int dx = _abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -_abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (1) {
        gfx_blit_put_pixel(ctx, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_blit_circle(gfx_blit_ctx_t *ctx, int xc, int yc, int r, uint32_t color) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        gfx_blit_put_pixel(ctx, xc + x, yc + y, color);
        gfx_blit_put_pixel(ctx, xc - x, yc + y, color);
        gfx_blit_put_pixel(ctx, xc + x, yc - y, color);
        gfx_blit_put_pixel(ctx, xc - x, yc - y, color);
        gfx_blit_put_pixel(ctx, xc + y, yc + x, color);
        gfx_blit_put_pixel(ctx, xc - y, yc + x, color);
        gfx_blit_put_pixel(ctx, xc + y, yc - x, color);
        gfx_blit_put_pixel(ctx, xc - y, yc - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

void gfx_blit_fill_circle(gfx_blit_ctx_t *ctx, int xc, int yc, int r, uint32_t color) {
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        gfx_blit_line(ctx, xc - x, yc + y, xc + x, yc + y, color);
        gfx_blit_line(ctx, xc - x, yc - y, xc + x, yc - y, color);
        gfx_blit_line(ctx, xc - y, yc + x, xc + y, yc + x, color);
        gfx_blit_line(ctx, xc - y, yc - x, xc + y, yc - x, color);
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
}

void gfx_blit_flip(gfx_blit_ctx_t *ctx, uint32_t *lfb) {
    if (!lfb) return;
    uint32_t       *dest = lfb;
    const uint32_t *src  = ctx->backbuffer;
    int             cnt  = ctx->width * ctx->height;
    asm volatile("rep movsl" : "+D"(dest), "+S"(src), "+c"(cnt) : : "memory");
}

