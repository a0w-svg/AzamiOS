/* ============================================================================
 * AzamiOS Desktop Environment — Shared UI Toolkit
 * File: userland/apps/shared/ui_kit.h
 *
 * Header-only toolkit used by all DE apps.
 * Include this after including the azwm protocol headers.
 *
 * Usage:
 *   #include "../azwm/protocol.h"
 *   #include "../azwm/de_protocol.h"
 *   #include "../azwm/de_font.h"
 *   #include "../shared/ui_kit.h"
 *
 * Every function is static inline to avoid ODR issues across TUs.
 * ============================================================================ */
#pragma once

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdbool.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#if __has_include(<azami/font.h>)
#include <azami/font.h>
#elif __has_include("az_font.h")
#include "az_font.h"
#elif __has_include("../../libc/include/azami/font.h")
#include "../../libc/include/azami/font.h"
#endif
#include "de_log.h"
#if defined(__x86_64__)
#include <emmintrin.h>
#include <immintrin.h>

static inline int uk_cpu_has_avx2(void)
{
    static int cached = -1;
    if (cached >= 0) return cached;

    unsigned int eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(1), "c"(0));
    if (!(ecx & (1u << 27)) || !(ecx & (1u << 28))) { cached = 0; return 0; }

    unsigned int xcr0_lo, xcr0_hi;
    __asm__ __volatile__("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6u) != 0x6u) { cached = 0; return 0; }

    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                                 : "a"(7), "c"(0));
    cached = (ebx & (1u << 5)) ? 1 : 0;
    return cached;
}

__attribute__((target("avx2")))
static inline void uk_fill_span_avx2(unsigned int *dst, unsigned int col, int count)
{
    __m256i c256 = _mm256_set1_epi32((int)col);
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        _mm256_storeu_si256((__m256i *)(dst + i), c256);
    }
    if (i + 4 <= count) {
        _mm_storeu_si128((__m128i *)(dst + i), _mm256_castsi256_si128(c256));
        i += 4;
    }
    while (i < count) {
        dst[i] = col;
        i++;
    }
}

static inline void uk_fill_span(unsigned int *dst, unsigned int col, int count)
{
    if (count <= 0) return;
    if (uk_cpu_has_avx2()) {
        uk_fill_span_avx2(dst, col, count);
        return;
    }
    int i = 0;
    __m128i col128 = _mm_set1_epi32((int)col);
    for (; i + 4 <= count; i += 4) {
        _mm_storeu_si128((__m128i *)(dst + i), col128);
    }
    while (i < count) {
        dst[i] = col;
        i++;
    }
}

__attribute__((target("avx2")))
static inline void uk_commit_frame_avx2(unsigned int *dst, const unsigned int *src, size_t count)
{
    size_t i = 0;
    while (i < count && ((uintptr_t)(dst + i) & 31)) {
        dst[i] = src[i];
        i++;
    }
    for (; i + 8 <= count; i += 8) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
        _mm256_stream_si256((__m256i *)(dst + i), v);
    }
    _mm_sfence();
    for (; i < count; i++) {
        dst[i] = src[i];
    }
}

static inline void uk_commit_frame(unsigned int *dst, const unsigned int *src, size_t count)
{
    if (uk_cpu_has_avx2()) {
        uk_commit_frame_avx2(dst, src, count);
        return;
    }
    size_t i = 0;
    while (i < count && ((uintptr_t)(dst + i) & 15)) {
        dst[i] = src[i];
        i++;
    }
    for (; i + 4 <= count; i += 4) {
        __m128i v = _mm_loadu_si128((const __m128i *)(src + i));
        _mm_stream_si128((__m128i *)(dst + i), v);
    }
    _mm_sfence();
    for (; i < count; i++) {
        dst[i] = src[i];
    }
}

__attribute__((target("avx2")))
static inline void uk_apply_alpha_avx2(unsigned int *pixels, size_t total, unsigned int alpha)
{
    __m256i a_vec = _mm256_set1_epi16((short)alpha);
    __m256i color_mask = _mm256_set1_epi32((int)0x00FFFFFF);
    size_t i = 0;
    for (; i + 8 <= total; i += 8) {
        __m256i c = _mm256_loadu_si256((const __m256i *)(pixels + i));
        __m256i ca = _mm256_srli_epi32(c, 24);
        __m256i mult = _mm256_mullo_epi16(ca, a_vec);
        __m256i na = _mm256_srli_epi16(_mm256_add_epi16(mult, _mm256_set1_epi16(128)), 8);
        __m256i na_shifted = _mm256_slli_epi32(na, 24);
        __m256i res = _mm256_or_si256(_mm256_and_si256(c, color_mask), na_shifted);
        _mm256_storeu_si256((__m256i *)(pixels + i), res);
    }
    for (; i < total; i++) {
        unsigned int c = pixels[i];
        unsigned int ca = (c >> 24) & 0xFF;
        if (ca > 0) {
            unsigned int na = (ca * alpha + 128) >> 8;
            pixels[i] = (na << 24) | (c & 0x00FFFFFF);
        }
    }
}

static inline void uk_apply_alpha(unsigned int *pixels, size_t total, unsigned int alpha)
{
    if (uk_cpu_has_avx2()) {
        uk_apply_alpha_avx2(pixels, total, alpha);
        return;
    }
    __m128i a_vec = _mm_set1_epi16((short)alpha);
    __m128i color_mask = _mm_set1_epi32((int)0x00FFFFFF);
    size_t i = 0;
    for (; i + 4 <= total; i += 4) {
        __m128i c = _mm_loadu_si128((const __m128i *)(pixels + i));
        __m128i ca = _mm_srli_epi32(c, 24);
        __m128i mult = _mm_mullo_epi16(ca, a_vec);
        __m128i na = _mm_srli_epi16(_mm_add_epi16(mult, _mm_set1_epi16(128)), 8);
        __m128i na_shifted = _mm_slli_epi32(na, 24);
        __m128i res = _mm_or_si128(_mm_and_si128(c, color_mask), na_shifted);
        _mm_storeu_si128((__m128i *)(pixels + i), res);
    }
    for (; i < total; i++) {
        unsigned int c = pixels[i];
        unsigned int ca = (c >> 24) & 0xFF;
        if (ca > 0) {
            unsigned int na = (ca * alpha + 128) >> 8;
            pixels[i] = (na << 24) | (c & 0x00FFFFFF);
        }
    }
}
#else
static inline void uk_fill_span(unsigned int *dst, unsigned int col, int count)
{
    if (count <= 0) return;
    int i = 0;
    unsigned long long col64 = ((unsigned long long)col << 32) | (unsigned long long)col;
    if (((unsigned long)dst & 7) && count > 0) {
        dst[i++] = col;
    }
    unsigned long long *dst64 = (unsigned long long *)&dst[i];
    int count64 = (count - i) / 2;
    for (int j = 0; j < count64; j++) {
        dst64[j] = col64;
    }
    i += count64 * 2;
    while (i < count) {
        dst[i++] = col;
    }
}

static inline void uk_commit_frame(unsigned int *dst, const unsigned int *src, size_t count)
{
    memcpy(dst, src, count * sizeof(unsigned int));
}

static inline void uk_apply_alpha(unsigned int *pixels, size_t total, unsigned int alpha)
{
    for (size_t i = 0; i < total; i++) {
        unsigned int c = pixels[i];
        unsigned int ca = (c >> 24) & 0xFF;
        if (ca > 0) {
            unsigned int na = (ca * alpha + 128) >> 8;
            pixels[i] = (na << 24) | (c & 0x00FFFFFF);
        }
    }
}
#endif

/* ============================================================================
 * Color definitions & Multi-Theme System
 * ============================================================================ */
#define UK_CRUST      0xFF11111B
#define UK_MANTLE     0xFF181825
#define UK_BASE       0xFF1E1E2E
#define UK_SURFACE0   0xFF313244
#define UK_SURFACE1   0xFF45475A
#define UK_SURFACE2   0xFF585B70
#define UK_OVERLAY0   0xFF6C7086
#define UK_OVERLAY1   0xFF7F849C
#define UK_OVERLAY2   0xFF9399B2
#define UK_SUBTEXT0   0xFFA6ADC8
#define UK_SUBTEXT1   0xFFBAC2DE
#define UK_TEXT       0xFFCDD6F4
#define UK_LAVENDER   0xFFB4BEFE
#define UK_BLUE       0xFF89B4FA
#define UK_SAPPHIRE   0xFF74C7EC
#define UK_SKY        0xFF89DCEB
#define UK_TEAL       0xFF94E2D5
#define UK_GREEN      0xFFA6E3A1
#define UK_YELLOW     0xFFF9E2AF
#define UK_PEACH      0xFFFAB387
#define UK_MAROON     0xFFEBA0AC
#define UK_RED        0xFFF38BA8
#define UK_MAUVE      0xFFCBA6F7
#define UK_PINK       0xFFF5C2E7
#define UK_FLAMINGO   0xFFF2CDCD
#define UK_ROSEWATER  0xFFF5E0DC

typedef struct {
    const char   *name;
    unsigned int  crust;
    unsigned int  mantle;
    unsigned int  base;
    unsigned int  surface0;
    unsigned int  surface1;
    unsigned int  surface2;
    unsigned int  overlay0;
    unsigned int  overlay1;
    unsigned int  text;
    unsigned int  accent;
    unsigned int  accent_sec;
    unsigned int  red;
    unsigned int  green;
    unsigned int  yellow;
    unsigned int  blue;
} uk_theme_palette_t;

static const uk_theme_palette_t g_uk_themes[AZ_THEME_COUNT] = {
    [AZ_THEME_MOCHA] = {
        .name       = "Catppuccin Mocha",
        .crust      = 0xFF11111B, .mantle     = 0xFF181825, .base       = 0xFF1E1E2E,
        .surface0   = 0xFF313244, .surface1   = 0xFF45475A, .surface2   = 0xFF585B70,
        .overlay0   = 0xFF6C7086, .overlay1   = 0xFF7F849C, .text       = 0xFFCDD6F4,
        .accent     = 0xFFCBA6F7, .accent_sec = 0xFFFAB387,
        .red        = 0xFFF38BA8, .green      = 0xFFA6E3A1, .yellow     = 0xFFF9E2AF, .blue = 0xFF89B4FA,
    },
    [AZ_THEME_LATTE] = {
        .name       = "Catppuccin Latte",
        .crust      = 0xFFDCE0E8, .mantle     = 0xFFE6E9EF, .base       = 0xFFEFF1F5,
        .surface0   = 0xFFCCD0DA, .surface1   = 0xFFBCC0CC, .surface2   = 0xFFACB0BE,
        .overlay0   = 0xFF9CA0B0, .overlay1   = 0xFF8C8FA1, .text       = 0xFF4C4F69,
        .accent     = 0xFF8839EF, .accent_sec = 0xFFFE640B,
        .red        = 0xFFD20F39, .green      = 0xFF40A02B, .yellow     = 0xFFDF8E1D, .blue = 0xFF1E66F5,
    },
    [AZ_THEME_NORD] = {
        .name       = "Nord Arctic",
        .crust      = 0xFF242933, .mantle     = 0xFF2E3440, .base       = 0xFF3B4252,
        .surface0   = 0xFF434C5E, .surface1   = 0xFF4C566A, .surface2   = 0xFF5A657D,
        .overlay0   = 0xFF7885A0, .overlay1   = 0xFF9AA7C0, .text       = 0xFFECEFF4,
        .accent     = 0xFF88C0D0, .accent_sec = 0xFF81A1C1,
        .red        = 0xFFBF616A, .green      = 0xFFA3BE8C, .yellow     = 0xFFEBCB8B, .blue = 0xFF5E81AC,
    },
    [AZ_THEME_CYBERPUNK] = {
        .name       = "Cyberpunk Neon",
        .crust      = 0xFF05050A, .mantle     = 0xFF0D0D18, .base       = 0xFF141424,
        .surface0   = 0xFF202038, .surface1   = 0xFF2E2E50, .surface2   = 0xFF424270,
        .overlay0   = 0xFF6868A0, .overlay1   = 0xFF8F8FD0, .text       = 0xFFF0F6FC,
        .accent     = 0xFF00FFCC, .accent_sec = 0xFFFF007F,
        .red        = 0xFFFF2A6D, .green      = 0xFF05FFA1, .yellow     = 0xFFFFE600, .blue = 0xFF00F0FF,
    },
    [AZ_THEME_OLED] = {
        .name       = "OLED Pure Dark",
        .crust      = 0xFF000000, .mantle     = 0xFF050505, .base       = 0xFF0A0A0A,
        .surface0   = 0xFF181818, .surface1   = 0xFF242424, .surface2   = 0xFF323232,
        .overlay0   = 0xFF555555, .overlay1   = 0xFF777777, .text       = 0xFFFFFFFF,
        .accent     = 0xFF3B82F6, .accent_sec = 0xFF10B981,
        .red        = 0xFFEF4444, .green      = 0xFF22C55E, .yellow     = 0xFFEAB308, .blue = 0xFF60A5FA,
    },
};

static inline const uk_theme_palette_t *uk_get_theme_palette(unsigned int theme_id)
{
    if (theme_id >= AZ_THEME_COUNT) theme_id = AZ_THEME_MOCHA;
    return &g_uk_themes[theme_id];
}

/* ============================================================================
 * Window connection state
 * ============================================================================ */

/* Where per-window private drawing buffers are mapped, one 16 MB slot each. */
#define UK_BACKBUF_BASE  0x78000000UL
#define UK_BACKBUF_STEP  0x01000000UL

/* Depth of the uk_push_clip()/uk_pop_clip() stack — plenty for any nesting
 * a DE app actually does (a scroll viewport inside a card inside a modal is
 * three), see the clip_stack field of uk_window_t below. */
#define UK_CLIP_STACK_MAX 8

/*
 * A window is two buffers, not one.
 *
 * `pixels` is where the application draws — a private buffer the compositor
 * never looks at.  `shared` is the surface azwm composites from.  Drawing a
 * frame takes many calls (clear, then panels, then text, then icons), and the
 * compositor runs whenever it likes; if those calls landed straight on the
 * shared surface, azwm would regularly composite a frame that was cleared but
 * not yet redrawn, and the window's contents would flicker.  uk_invalidate()
 * copies the finished frame across in one pass and only then tells azwm, so
 * what gets composited is always a whole frame.
 */
typedef struct {
    unsigned int *pixels;       /* Private drawing buffer                 */
    unsigned int *shared;       /* Surface the compositor reads           */
    unsigned int  width;        /* Client area width                      */
    unsigned int  height;       /* Client area height                     */
    unsigned int  wid;          /* Window ID assigned by compositor       */
    int           client_chan;  /* Our reply channel                      */
    int           server_chan;  /* azwm server channel (always 1)         */
    int           x;            /* Window X position                      */
    int           y;            /* Window Y position                      */
    /* shmem ids behind `shared`/`pixels`, kept so a live resize
     * (uk_handle_resize()) can unmap exactly what it mapped rather than
     * guessing. backbuf_shmem_id is 0 when `pixels == shared` — no private
     * backbuffer exists (its allocation failed at connect time). */
    unsigned int  shmem_id;
    unsigned int  backbuf_shmem_id;
    /* Scissor rect: every fill/shape primitive below clips to
     * [clip_x0,clip_x1) x [clip_y0,clip_y1) instead of the full window.
     * Always kept within window bounds; set to the whole window by
     * uk_window_connect() and reset there again by uk_handle_resize().
     * clip_stack/clip_depth back uk_push_clip()/uk_pop_clip() below. */
    int           clip_x0, clip_y0, clip_x1, clip_y1;
    int           clip_stack[UK_CLIP_STACK_MAX][4];
    int           clip_depth;
    az_font_t    *font;
} uk_window_t;

/* ============================================================================
 * Drawing primitives
 * ============================================================================ */

/*
 * uk_push_clip(win, x, y, w, h) — intersect the active clip with the given
 * rect (in window-local coordinates) and make the result active; the
 * matching uk_pop_clip() restores whatever was active before this push.
 * Every fill/shape primitive below (uk_put_pixel, uk_fill_rect and anything
 * built on it — rounded rects, gradients, circles, buttons, panels, lines,
 * ...) honours the active clip; text does not, since de_font_draw_char() has
 * no clip-rect parameter of its own — scroll a list of text the way the
 * existing DE apps already do, by drawing only the rows known to be inside
 * the visible range and letting uk_draw_text_clip()'s max_px bound each
 * row's width. uk_draw_line_aa() also writes pixels directly for its
 * antialiasing and is not scissored.
 *
 * Pushing past UK_CLIP_STACK_MAX or popping an empty stack is a silent
 * no-op rather than corrupting state — an unmatched call just stops
 * narrowing (or fails to widen) the clip instead of crashing.
 */
static inline void uk_push_clip(uk_window_t *w, int x, int y, int cw, int ch)
{
    if (w->clip_depth < UK_CLIP_STACK_MAX) {
        w->clip_stack[w->clip_depth][0] = w->clip_x0;
        w->clip_stack[w->clip_depth][1] = w->clip_y0;
        w->clip_stack[w->clip_depth][2] = w->clip_x1;
        w->clip_stack[w->clip_depth][3] = w->clip_y1;
        w->clip_depth++;
    }
    int x0 = x, y0 = y, x1 = x + cw, y1 = y + ch;
    if (x0 < w->clip_x0) x0 = w->clip_x0;
    if (y0 < w->clip_y0) y0 = w->clip_y0;
    if (x1 > w->clip_x1) x1 = w->clip_x1;
    if (y1 > w->clip_y1) y1 = w->clip_y1;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    w->clip_x0 = x0; w->clip_y0 = y0; w->clip_x1 = x1; w->clip_y1 = y1;
}

static inline void uk_pop_clip(uk_window_t *w)
{
    if (w->clip_depth > 0) {
        w->clip_depth--;
        w->clip_x0 = w->clip_stack[w->clip_depth][0];
        w->clip_y0 = w->clip_stack[w->clip_depth][1];
        w->clip_x1 = w->clip_stack[w->clip_depth][2];
        w->clip_y1 = w->clip_stack[w->clip_depth][3];
    }
}

static inline void uk_put_pixel(uk_window_t *w, int x, int y, unsigned int col)
{
    if (x < w->clip_x0 || y < w->clip_y0 || x >= w->clip_x1 || y >= w->clip_y1)
        return;
    w->pixels[(unsigned int)y * w->width + (unsigned int)x] = col;
}

static inline unsigned int uk_blend(unsigned int dst, unsigned int src, unsigned int a)
{
    if (a == 0) return dst;
    if (a >= 255) return src;
    unsigned int inv_a = 255 - a;
    unsigned int rb = (((src & 0x00FF00FF) * a + (dst & 0x00FF00FF) * inv_a) >> 8) & 0x00FF00FF;
    unsigned int g  = (((src & 0x0000FF00) * a + (dst & 0x0000FF00) * inv_a) >> 8) & 0x0000FF00;
    return 0xFF000000 | rb | g;
}

/* ============================================================================
 * Shared motion helpers
 *
 * Fixed-point ease-out-quadratic + linear interpolation, both in the 0..256
 * range — the same curve azwm's own compositor uses for window open/
 * minimize/restore (see compositor_animate_step()). Apps that drive their
 * own small UI animations (hover lifts, sliding indicators, modal pop-ins)
 * off a timer tick should use these so in-app motion feels like the same
 * system as window-level motion, not a different one bolted on.
 * ============================================================================ */

/* `t` is elapsed progress 0..256 (0 = just started, 256 = done); returns the
 * eased progress, also 0..256, front-loaded (fast start, gentle settle). */
static inline int uk_ease_out_quad(int t)
{
    if (t < 0) t = 0;
    if (t > 256) t = 256;
    return (t * (512 - t)) / 256;
}

/* Interpolates the integer range [a, b] at eased progress `ease256` (0..256). */
static inline int uk_lerp(int a, int b, int ease256)
{
    if (ease256 < 0) ease256 = 0;
    if (ease256 > 256) ease256 = 256;
    return a + ((b - a) * ease256) / 256;
}

static inline void uk_fill_rect(uk_window_t *w,
                                int rx, int ry, int rw, int rh,
                                unsigned int col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < w->clip_x0 ? w->clip_x0 : rx;
    int y0 = ry < w->clip_y0 ? w->clip_y0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > w->clip_x1) x1 = w->clip_x1;
    if (y1 > w->clip_y1) y1 = w->clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    int fill_w = x1 - x0;
    for (int y = y0; y < y1; y++) {
        unsigned int *dst = &w->pixels[(unsigned int)y * w->width + (unsigned int)x0];
        uk_fill_span(dst, col, fill_w);
    }
}

/* Vertical gradient fill (top→bottom) */
static inline void uk_gradient_v(uk_window_t *w,
                                 int rx, int ry, int rw, int rh,
                                 unsigned int top_col, unsigned int bot_col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < w->clip_x0 ? w->clip_x0 : rx;
    int y0 = ry < w->clip_y0 ? w->clip_y0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > w->clip_x1) x1 = w->clip_x1;
    if (y1 > w->clip_y1) y1 = w->clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    int fill_w = x1 - x0;
    for (int y = y0; y < y1; y++) {
        unsigned int t = (rh > 1) ? (unsigned int)((y - ry) * 255 / (rh - 1)) : 0;
        unsigned int col = uk_blend(top_col, bot_col, t);
        unsigned int *dst = &w->pixels[(unsigned int)y * w->width + (unsigned int)x0];
        uk_fill_span(dst, col, fill_w);
    }
}

/* Horizontal gradient fill (left→right) */
static inline void uk_gradient_h(uk_window_t *w,
                                 int rx, int ry, int rw, int rh,
                                 unsigned int left_col, unsigned int right_col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < w->clip_x0 ? w->clip_x0 : rx;
    int y0 = ry < w->clip_y0 ? w->clip_y0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if (x1 > w->clip_x1) x1 = w->clip_x1;
    if (y1 > w->clip_y1) y1 = w->clip_y1;
    if (x0 >= x1 || y0 >= y1) return;

    int fill_w = x1 - x0;
    #define UK_GRAD_STACK_MAX 1024
    unsigned int stack_row[UK_GRAD_STACK_MAX];
    int chunk = (fill_w < UK_GRAD_STACK_MAX) ? fill_w : UK_GRAD_STACK_MAX;
    for (int off = 0; off < fill_w; off += chunk) {
        int seg = fill_w - off;
        if (seg > chunk) seg = chunk;
        for (int i = 0; i < seg; i++) {
            int x = x0 + off + i;
            unsigned int t = (rw > 1) ? (unsigned int)((x - rx) * 255 / (rw - 1)) : 0;
            stack_row[i] = uk_blend(left_col, right_col, t);
        }
        for (int y = y0; y < y1; y++) {
            unsigned int *dst = &w->pixels[(unsigned int)y * w->width + (unsigned int)x0 + off];
            memcpy(dst, stack_row, (size_t)seg * sizeof(unsigned int));
        }
    }
}

/* Draw a circle (filled) — high-speed scanline rasterizer */
static inline void uk_fill_circle(uk_window_t *w, int cx, int cy, int r, unsigned int col)
{
    if (r <= 0 || !w || !w->pixels) return;
    int r2 = r * r;

    for (int y = -r; y <= r; y++) {
        int py = cy + y;
        if (py < w->clip_y0 || py >= w->clip_y1) continue;
        int rem = r2 - y * y;
        if (rem < 0) continue;
        int max_x = 0;
        while ((max_x + 1) * (max_x + 1) <= rem) max_x++;

        int x0 = cx - max_x;
        int x1 = cx + max_x + 1;
        if (x0 < w->clip_x0) x0 = w->clip_x0;
        if (x1 > w->clip_x1) x1 = w->clip_x1;
        if (x0 >= x1) continue;

        unsigned int *dst = &w->pixels[(unsigned int)py * w->width + (unsigned int)x0];
        uk_fill_span(dst, col, x1 - x0);
    }
}

static inline void uk_fill_rounded_rect(uk_window_t *w,
                                        int rx, int ry, int rw, int rh,
                                        int radius, unsigned int col)
{
    if (rw <= 0 || rh <= 0) return;
    if (radius * 2 > rw) radius = rw / 2;
    if (radius * 2 > rh) radius = rh / 2;
    if (radius <= 0) {
        uk_fill_rect(w, rx, ry, rw, rh, col);
        return;
    }
    uk_fill_rect(w, rx + radius, ry, rw - 2 * radius, rh, col);
    uk_fill_rect(w, rx, ry + radius, radius, rh - 2 * radius, col);
    uk_fill_rect(w, rx + rw - radius, ry + radius, radius, rh - 2 * radius, col);
    uk_fill_circle(w, rx + radius,          ry + radius,          radius, col);
    uk_fill_circle(w, rx + rw - radius - 1, ry + radius,          radius, col);
    uk_fill_circle(w, rx + radius,          ry + rh - radius - 1, radius, col);
    uk_fill_circle(w, rx + rw - radius - 1, ry + rh - radius - 1, radius, col);
}

/* ============================================================================
 * Text rendering (uses de_font.h)
 * ============================================================================ */

static inline void uk_draw_char(uk_window_t *w, int x, int y, char c, unsigned int col)
{
    if (w && w->font)
        az_font_draw_char(w->pixels, w->width, w->width, w->height, x, y, c, col, w->font, 1, false);
    else
        de_font_draw_char(w->pixels, w->width, w->width, w->height, x, y, c, col);
}

static inline void uk_draw_text(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    if (w && w->font) {
        az_font_draw_str(w->pixels, w->width, w->width, w->height, x, y, s, col, w->font, 1, false);
    } else {
        int i;
        for (i = 0; s[i]; i++)
            uk_draw_char(w, x + i * 8, y, s[i], col);
    }
}

static inline void uk_draw_text_2x(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    if (w && w->font)
        az_font_draw_str(w->pixels, w->width, w->width, w->height, x, y, s, col, w->font, 2, false);
    else
        de_font_draw_str_2x(w->pixels, w->width, w->width, w->height, x, y, s, col);
}

static inline az_font_t *uk_load_font(const char *path_or_name)
{
    return az_font_load_by_name(path_or_name);
}

static inline void uk_set_font(uk_window_t *w, az_font_t *font)
{
    if (w) w->font = font;
}

static inline const az_font_t *uk_get_font(const uk_window_t *w)
{
    return (w && w->font) ? w->font : NULL;
}

static inline void uk_draw_text_font(uk_window_t *w, int x, int y, const char *s, unsigned int col,
                                     const az_font_t *font, int scale, bool bold)
{
    if (!w || !s) return;
    az_font_draw_str(w->pixels, w->width, w->width, w->height, x, y, s, col, font, scale, bold);
}

static inline int uk_text_width_font(const az_font_t *font, const char *s, int scale)
{
    return az_font_str_width(font, s, scale);
}

/*
 * uk_draw_text_ex() — the general entry point every uk_draw_text*() helper
 * above reduces to: any of de_font.h's font faces (&de_font_regular,
 * &de_font_small, or an app's own de_font_t), any integer scale, optional
 * synthetic bold. Reach for this directly when a widget wants a face other
 * than plain Regular; the plain helpers stay the short spelling for the
 * overwhelmingly common "Regular, 1x, normal weight" case.
 */
static inline void uk_draw_text_ex(uk_window_t *w, int x, int y, const char *s, unsigned int col,
                                   const de_font_t *font, int scale, bool bold)
{
    de_font_draw_str_ex(w->pixels, w->width, w->width, w->height, x, y, s, col, font, scale, bold);
}

/* Compact companion face (de_font_small, 8x8) for status rows, table cells
 * and anywhere Regular's 16px line height doesn't fit. */
static inline void uk_draw_text_small(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    uk_draw_text_ex(w, x, y, s, col, &de_font_small, 1, false);
}

/* Regular face with a synthetic-bold pass (see de_font_draw_char_ex()) —
 * for emphasis (section headers, the focused item in a list) without
 * pulling in a whole second hand-authored font. */
static inline void uk_draw_text_bold(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    uk_draw_text_ex(w, x, y, s, col, &de_font_regular, 1, true);
}

/* Draw text clipped to max_px width */
static inline void uk_draw_text_clip(uk_window_t *w, int x, int y,
                                      const char *s, unsigned int col, int max_px)
{
    int i;
    for (i = 0; s[i] && i * 8 + 8 <= max_px; i++)
        uk_draw_char(w, x + i * 8, y, s[i], col);
}

/* String length (no strlen in minimal libc) */
static inline int uk_strlen(const char *s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
}

/* Pixel width of `s` set in `font` at `scale` — use this instead of
 * hand-multiplying by 8 when centring/clipping anything drawn with
 * uk_draw_text_ex()/uk_draw_text_small(), since de_font_small's cells
 * are the same 8px width as Regular's but a font of your own may not be. */
static inline int uk_text_width(const de_font_t *font, const char *s, int scale)
{
    return uk_strlen(s) * (int)font->glyph_w * scale;
}

/* Centre text horizontally in a region */
static inline void uk_draw_text_centred(uk_window_t *w, int cx, int y,
                                         const char *s, unsigned int col)
{
    int len = uk_strlen(s);
    uk_draw_text(w, cx - (len * 8) / 2, y, s, col);
}

/* ============================================================================
 * Button widget
 * ============================================================================ */

typedef enum {
    UK_BTN_NORMAL = 0,
    UK_BTN_HOVER,
    UK_BTN_PRESSED,
    UK_BTN_DISABLED
} uk_btn_state_t;

static inline void uk_draw_button(uk_window_t *w,
                                  int bx, int by, int bw, int bh,
                                  const char *label, uk_btn_state_t state)
{
    if (bw <= 0 || bh <= 0) return;
    unsigned int top_bg, bot_bg, fg, border, highlight;
    switch (state) {
    case UK_BTN_HOVER:
        top_bg = UK_SURFACE2; bot_bg = UK_SURFACE1; fg = UK_TEXT;    border = UK_BLUE;     highlight = 0x50FFFFFF; break;
    case UK_BTN_PRESSED:
        top_bg = UK_MAUVE;    bot_bg = 0xFFB48EAD;  fg = UK_BASE;    border = UK_MAUVE;    highlight = 0x20000000; break;
    case UK_BTN_DISABLED:
        top_bg = UK_SURFACE0; bot_bg = UK_SURFACE0; fg = UK_OVERLAY0; border = UK_SURFACE1; highlight = 0x00000000; break;
    default: /* NORMAL */
        top_bg = UK_SURFACE1; bot_bg = UK_SURFACE0; fg = UK_TEXT;    border = UK_SURFACE2; highlight = 0x30FFFFFF; break;
    }

    /* Drop shadow — the same elevation cue window titlebars and launcher
     * tiles use, so a button reads as a raised surface instead of a flat
     * patch of the panel behind it. Skipped when disabled: a disabled
     * control sits flush with the panel, not above it. */
    if (state != UK_BTN_DISABLED) {
        uk_fill_rounded_rect(w, bx + 1, by + 2, bw, bh, 5, 0x30000000);
    }

    uk_fill_rounded_rect(w, bx, by, bw, bh, 5, bot_bg);
    uk_gradient_v(w, bx + 1, by + 1, bw - 2, bh - 2, top_bg, bot_bg);

    /* Specular highlight line */
    if (highlight && bh > 4) {
        for (int x = bx + 4; x < bx + bw - 4; x++) {
            if (x >= 0 && (unsigned int)x < w->width && (by + 1) >= 0 && (unsigned int)(by + 1) < w->height) {
                unsigned int *p = &w->pixels[(by + 1) * w->width + x];
                *p = uk_blend(*p, 0xFFFFFFFF, (highlight >> 24) & 0xFF);
            }
        }
    }

    /* 1px Border */
    for (int x = bx + 3; x < bx + bw - 3; x++) {
        uk_put_pixel(w, x, by,          border);
        uk_put_pixel(w, x, by + bh - 1, border);
    }
    for (int y = by + 3; y < by + bh - 3; y++) {
        uk_put_pixel(w, bx,          y, border);
        uk_put_pixel(w, bx + bw - 1, y, border);
    }

    /* Centred label with subtle text drop shadow */
    int len = uk_strlen(label);
    int tx = bx + bw / 2 - (len * 8) / 2;
    int ty = by + (bh - 16) / 2;
    if (state != UK_BTN_PRESSED && state != UK_BTN_DISABLED) {
        uk_draw_text(w, tx + 1, ty + 1, label, 0xFF11111B);
    }
    uk_draw_text(w, tx, ty, label, fg);
}

/* ============================================================================
 * Panel / section header
 * ============================================================================ */

static inline void uk_draw_panel(uk_window_t *w,
                                  int px, int py, int pw, int ph,
                                  unsigned int bg)
{
    uk_fill_rounded_rect(w, px, py, pw, ph, 6, bg);
}

static inline void uk_draw_section_header(uk_window_t *w,
                                            int x, int y, int width,
                                            const char *title,
                                            unsigned int accent)
{
    /* Accent bar */
    uk_fill_rect(w, x, y, 3, 16, accent);
    /* Title text — synthetic bold for real visual hierarchy against the
     * plain-weight body text section headers sit above. Same advance width
     * as regular (see de_font_draw_char_ex()), so nothing below needs to
     * account for a wider title: this is a pure weight change. */
    uk_draw_text_bold(w, x + 8, y, title, UK_TEXT);
    /* Divider line */
    uk_fill_rect(w, x, y + 18, width, 1, UK_SURFACE1);
}

/* ============================================================================
 * Scrollbar (simple vertical)
 * ============================================================================ */

static inline void uk_draw_scrollbar(uk_window_t *w,
                                      int x, int y, int h,
                                      int thumb_pos, int thumb_h)
{
    if (!w || !w->pixels || h <= 0) return;
    if (thumb_h < 6) thumb_h = 6;
    if (thumb_h > h) thumb_h = h;
    if (thumb_pos < 0) thumb_pos = 0;
    if (thumb_pos + thumb_h > h) thumb_pos = h - thumb_h;

    uk_fill_rounded_rect(w, x, y, 8, h, 3, UK_SURFACE0);
    uk_fill_rounded_rect(w, x + 1, y + thumb_pos, 6, thumb_h, 2, UK_SURFACE2);
}

/* ============================================================================
 * Tab bar (horizontal)
 * ============================================================================ */

#define UK_MAX_TABS  8

static inline void uk_draw_tab_bar(uk_window_t *w,
                                    int x, int y, int tab_w, int tab_h,
                                    const char **labels, int count, int active)
{
    int i;
    for (i = 0; i < count; i++) {
        int tx = x + i * (tab_w + 2);
        unsigned int bg     = (i == active) ? UK_SURFACE1 : UK_SURFACE0;
        unsigned int fg     = (i == active) ? UK_TEXT     : UK_OVERLAY1;
        unsigned int accent = (i == active) ? UK_MAUVE    : UK_BASE;

        uk_fill_rect(w, tx, y, tab_w, tab_h, bg);
        /* Active tab: bottom accent bar */
        uk_fill_rect(w, tx, y + tab_h - 2, tab_w, 2, accent);

        int llen = uk_strlen(labels[i]);
        uk_draw_text(w, tx + tab_w / 2 - (llen * 8) / 2,
                     y + (tab_h - 16) / 2, labels[i], fg);
    }
}

/* ============================================================================
 * Real icon files (".icn": a raw 32×32 ARGB dump, no header)
 *
 * Every app can ship an optional <appname>.icn next to its source (see e.g.
 * apps/filemanager/filemanager.icn) — `make icons` copies these into
 * /usr/share/icons/. Loading one here, rather than each app hand-rolling its
 * own open/read/fallback-path dance, is what lets callers draw a real icon
 * when the artwork exists and fall back to a generated glyph/badge when it
 * doesn't, without duplicating that logic per app.
 * ============================================================================ */
#define UK_ICON_DIM    32
#define UK_ICON_PIXELS (UK_ICON_DIM * UK_ICON_DIM)
#define UK_ICON_BYTES  (UK_ICON_PIXELS * (int)sizeof(unsigned int))

/*
 * uk_load_icon32(app_name, fallback_dir, out) — load a 32x32 ARGB icon into
 * `out` (must hold UK_ICON_PIXELS unsigned ints). Tries
 * /usr/share/icons/<app_name>.icn first, then <fallback_dir>/<app_name>.icn
 * (pass NULL to skip that second try — e.g. when the app's own directory
 * isn't meaningful, such as for a dock entry named after a bare command).
 * Returns true and fills `out` on success; returns false (leaving `out`
 * untouched) if no icon file exists or it isn't exactly the expected size,
 * so the caller can draw its own fallback instead.
 */
static inline bool uk_load_icon32(const char *app_name, const char *fallback_dir, unsigned int *out)
{
    char path[160];
    snprintf(path, sizeof(path), "/usr/share/icons/%s.icn", app_name);
    int fd = sys_open(path, 0, 0);
    if (fd < 0 && fallback_dir) {
        snprintf(path, sizeof(path), "%s/%s.icn", fallback_dir, app_name);
        fd = sys_open(path, 0, 0);
    }
    if (fd < 0) return false;

    int nr = sys_read(fd, out, (size_t)UK_ICON_BYTES);
    sys_close(fd);
    return nr == UK_ICON_BYTES;
}

/*
 * Shared blit core for uk_draw_icon32()/uk_draw_png(): alpha-composites a
 * src_w x src_h ARGB image at (x, y), one row of pixels at a time.
 *
 * Both call sites used to run uk_put_pixel() per pixel — itself a 4-way
 * clip-bounds branch plus a fresh `y * width + x` row computation every
 * single pixel — and, on top of that, the alpha-blend branch re-checked
 * `dx/dy` against w->width/height by hand before calling it, even though
 * clip_x1/clip_y1 are always <= width/height (see the clip-rect comment on
 * uk_window_t above), so that second check could never itself reject a
 * pixel the first one hadn't already caught. Icons and images redraw on
 * essentially every frame that touches a taskbar, launcher, file list or
 * about/settings panel, so that per-pixel duplication was pure waste.
 *
 * This clips the destination rectangle once up front, then walks only the
 * pixels that survive it — a straight-line loop with a single per-pixel
 * branch (skip fully transparent, store fully opaque, else blend) and no
 * bounds checking left inside it at all.
 */
static inline void uk_blit_argb(uk_window_t *w, int x, int y,
                                const unsigned int *src, int src_w, int src_h)
{
    int cx0 = x > w->clip_x0 ? x : w->clip_x0;
    int cy0 = y > w->clip_y0 ? y : w->clip_y0;
    int cx1 = (x + src_w) < w->clip_x1 ? (x + src_w) : w->clip_x1;
    int cy1 = (y + src_h) < w->clip_y1 ? (y + src_h) : w->clip_y1;
    if (cx0 >= cx1 || cy0 >= cy1) return;

    for (int dy = cy0; dy < cy1; dy++) {
        const unsigned int *srow = src + (size_t)(dy - y) * src_w + (cx0 - x);
        unsigned int *drow = w->pixels + (size_t)dy * w->width + cx0;
        int count = cx1 - cx0;
        for (int i = 0; i < count; i++) {
            unsigned int c = srow[i];
            unsigned char a = (unsigned char)(c >> 24);
            if (a == 0) continue;
            drow[i] = (a == 255) ? c : uk_blend(drow[i], c, a);
        }
    }
}

/* uk_draw_icon32() — alpha-blit a 32x32 ARGB icon (as loaded by
 * uk_load_icon32()) at (x, y). */
static inline void uk_draw_icon32(uk_window_t *w, int x, int y, const unsigned int *icon)
{
    uk_blit_argb(w, x, y, icon, UK_ICON_DIM, UK_ICON_DIM);
}

/* Real (arbitrary-size) PNG loading — uk_load_png()/uk_free_png(). See
 * png_decode.h for what PNG subset it covers and why (bit depth 8, no
 * palette, no interlacing). */
#include "png_decode.h"

/* uk_draw_png(win, x, y, pixels, img_w, img_h) — alpha-blit a uk_load_png()
 * result at (x, y) onto a uk_window_t, same blend rule as uk_draw_icon32():
 * alpha 0 skipped, alpha 255 stored directly, anything between blended
 * against what's already there. Lives here rather than in png_decode.h
 * since it needs uk_put_pixel()/uk_blend()/uk_window_t, which that file
 * deliberately doesn't depend on — see its comment by uk_free_png(). */
static inline void uk_draw_png(uk_window_t *w, int x, int y,
                               const unsigned int *pixels, int img_w, int img_h)
{
    uk_blit_argb(w, x, y, pixels, img_w, img_h);
}

/* ============================================================================
 * Pixel-art icons (32×32 each)
 * All drawn relative to a top-left (ix, iy) anchor.
 * ============================================================================ */

/* Generic app icon base: rounded square background */
static inline void uk_icon_base(uk_window_t *w, int ix, int iy, unsigned int bg)
{
    uk_fill_rounded_rect(w, ix, iy, 32, 32, 5, bg);
}

/* ── Text Editor icon: horizontal lines ─────────────────────────────────────── */
static inline void uk_icon_texteditor(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_BLUE);
    int row;
    /* Document lines */
    for (row = 0; row < 5; row++) {
        int lw = (row == 4) ? 10 : 18;
        uk_fill_rect(w, ix + 7, iy + 7 + row * 4, lw, 2, UK_BASE);
    }
    /* Cursor */
    uk_fill_rect(w, ix + 7, iy + 7, 1, 18, UK_YELLOW);
}

/* ── Calculator icon: grid of buttons ───────────────────────────────────────── */
static inline void uk_icon_calculator(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_GREEN);
    /* Display bar */
    uk_fill_rect(w, ix + 5, iy + 5, 22, 6, UK_BASE);
    /* 4 buttons in 2×2 grid */
    uk_fill_rect(w, ix + 5,  iy + 14, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 18, iy + 14, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 5,  iy + 22, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 18, iy + 22, 9, 5, UK_BASE);
}

/* ── Clock icon: circle with hands ─────────────────────────────────────────── */
static inline void uk_icon_clock(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_PEACH);
    int cx = ix + 16, cy = iy + 16;
    uk_fill_circle(w, cx, cy, 10, UK_BASE);
    uk_fill_circle(w, cx, cy,  8, UK_MANTLE);
    /* Hour hand (pointing up-right) */
    int hx, hy;
    for (hx = 0; hx <= 4; hx++) { hy = -hx; uk_put_pixel(w, cx+hx, cy+hy, UK_TEXT); }
    /* Minute hand (pointing down) */
    int my2;
    for (my2 = 0; my2 <= 6; my2++) uk_put_pixel(w, cx, cy + my2, UK_TEXT);
    /* Centre dot */
    uk_fill_circle(w, cx, cy, 1, UK_MAUVE);
}

/* ── System Monitor icon: bar chart ─────────────────────────────────────────── */
static inline void uk_icon_sysmon(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_TEAL);
    /* Bar chart bars */
    uk_fill_rect(w, ix + 5,  iy + 18, 5, 8,  UK_BASE);
    uk_fill_rect(w, ix + 12, iy + 12, 5, 14, UK_BASE);
    uk_fill_rect(w, ix + 19, iy + 8,  5, 18, UK_BASE);
    /* X axis */
    uk_fill_rect(w, ix + 4, iy + 26, 22, 1, UK_BASE);
}

/* ── File Manager icon: folder ──────────────────────────────────────────────── */
static inline void uk_icon_filemanager(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_YELLOW);
    /* Folder tab */
    uk_fill_rounded_rect(w, ix + 4, iy + 10, 24, 16, 2, UK_BASE);
    uk_fill_rect(w, ix + 4, iy + 8, 10, 4, UK_BASE);
    /* Files inside (3 lines) */
    uk_fill_rect(w, ix + 7, iy + 13, 14, 2, UK_CRUST);
    uk_fill_rect(w, ix + 7, iy + 17, 14, 2, UK_CRUST);
    uk_fill_rect(w, ix + 7, iy + 21, 10, 2, UK_CRUST);
}

/* ── Terminal icon: >_ prompt ───────────────────────────────────────────────── */
static inline void uk_icon_terminal(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_SURFACE0);
    /* > arrow */
    uk_put_pixel(w, ix + 7,  iy + 12, UK_GREEN);
    uk_put_pixel(w, ix + 9,  iy + 14, UK_GREEN);
    uk_put_pixel(w, ix + 11, iy + 16, UK_GREEN);
    uk_put_pixel(w, ix + 9,  iy + 18, UK_GREEN);
    uk_put_pixel(w, ix + 7,  iy + 20, UK_GREEN);
    uk_put_pixel(w, ix + 8,  iy + 13, UK_GREEN);
    uk_put_pixel(w, ix + 10, iy + 15, UK_GREEN);
    uk_put_pixel(w, ix + 10, iy + 17, UK_GREEN);
    uk_put_pixel(w, ix + 8,  iy + 19, UK_GREEN);
    /* _ underscore */
    uk_fill_rect(w, ix + 14, iy + 20, 10, 2, UK_TEXT);
}

/* ── Settings icon: gear ────────────────────────────────────────────────────── */
static inline void uk_icon_settings(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_OVERLAY1);
    int cx = ix + 16, cy = iy + 16;
    /* Outer ring (gear body) */
    uk_fill_circle(w, cx, cy, 9, UK_BASE);
    uk_fill_circle(w, cx, cy, 5, UK_SURFACE1);
    /* 4 gear teeth (N/S/E/W) */
    uk_fill_rect(w, cx - 1, cy - 13, 3, 4, UK_BASE);
    uk_fill_rect(w, cx - 1, cy +  9, 3, 4, UK_BASE);
    uk_fill_rect(w, cx - 13, cy - 1, 4, 3, UK_BASE);
    uk_fill_rect(w, cx +  9, cy - 1, 4, 3, UK_BASE);
    /* Centre hole */
    uk_fill_circle(w, cx, cy, 3, UK_SURFACE1);
}

/* ── About icon: info circle ────────────────────────────────────────────────── */
static inline void uk_icon_about(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_MAUVE);
    int cx = ix + 16, cy = iy + 16;
    uk_fill_circle(w, cx, cy, 11, UK_BASE);
    /* "i" letter */
    uk_fill_rect(w, cx - 1, cy -  6, 3, 3, UK_MAUVE);  /* dot */
    uk_fill_rect(w, cx - 1, cy -  1, 3, 9, UK_MAUVE);  /* stem */
}

/* ============================================================================
 * IPC helpers
 * ============================================================================ */

/*
 * uk_invalidate() — publish the frame just drawn and tell the compositor.
 *
 * This is the commit point: the private buffer is copied to the shared surface
 * as one linear pass, so azwm never composites a half-drawn window.
 */
static inline void uk_invalidate(uk_window_t *win)
{
    if (win->shared && win->pixels && win->shared != win->pixels) {
        uk_commit_frame(win->shared, win->pixels,
                        (size_t)win->width * win->height);
    }

    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = win->wid;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&inv);
}

/*
 * uk_invalidate_rect() — like uk_invalidate(), but for a caller that knows
 * only a small part of the window actually changed (a blinking cursor, one
 * hovered row, a single updated stat) and wants both ends of the publish to
 * cost proportionally little instead of a full window's worth of work:
 *
 *   - client-side: uk_invalidate() always copies the *entire* private
 *     buffer to the shared surface in one pass, no matter how little of it
 *     is actually different from what's already there. That's the right
 *     shape for an app that redrew everything, but it means even a single
 *     toggled pixel pays for a copy the size of the whole window.
 *   - server-side: azwm's AZ_WM_INVALIDATE handler used to always damage
 *     the window's full frame (content + border + titlebar), so the
 *     compositor recomposited the whole thing regardless of what the
 *     client actually said changed.
 *
 * (x, y, w, h) are in this window's own content-local coordinates — the
 * same coordinates every uk_draw_*()/uk_fill_*() call already uses — and
 * are clipped to the window here, so passing a rect that runs off an edge
 * (or a stale one from before a resize) can't publish or damage outside
 * the buffer. A rect that clips down to empty publishes nothing at all,
 * client-side copy included: there's genuinely nothing to tell azwm.
 *
 * The exact "copy the whole thing in one pass" fast path uk_invalidate()
 * always used stays intact for the case that still matches it — a rect
 * that, after clipping, *is* the whole window — so switching an existing
 * full-window uk_invalidate() call to uk_invalidate_rect(win, 0, 0, w, h)
 * costs nothing.
 */
static inline void uk_invalidate_rect(uk_window_t *win, int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win->width)  w = (int)win->width  - x;
    if (y + h > (int)win->height) h = (int)win->height - y;
    if (w <= 0 || h <= 0) return;

    if (win->shared && win->pixels && win->shared != win->pixels) {
        if (x == 0 && y == 0 && (unsigned int)w == win->width && (unsigned int)h == win->height) {
            uk_commit_frame(win->shared, win->pixels, (size_t)win->width * win->height);
        } else {
            for (int row = 0; row < h; row++) {
                size_t off = (size_t)(y + row) * win->width + (size_t)x;
                uk_commit_frame(win->shared + off, win->pixels + off, (size_t)w);
            }
        }
    }

    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = win->wid;
    inv.invalidate.x = x;
    inv.invalidate.y = y;
    inv.invalidate.w = (unsigned int)w;
    inv.invalidate.h = (unsigned int)h;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&inv);
}

/*
 * uk_set_opacity() — adjust window opacity level (0 = fully transparent, 255 = fully opaque).
 */
static inline void uk_set_opacity(uk_window_t *win, unsigned char opacity)
{
    if (!win || win->wid == 0) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_SET_OPACITY;
    msg.wid  = win->wid;
    msg.opacity.opacity = opacity;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/*
 * uk_set_title() — dynamically update the window title in titlebar and taskbar.
 */
static inline void uk_set_title(uk_window_t *win, const char *title)
{
    if (!win || win->wid == 0 || !title) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_SET_TITLE;
    msg.wid  = win->wid;
    int i;
    for (i = 0; title[i] && i < (int)sizeof(msg.set_title.title) - 1; i++) {
        msg.set_title.title[i] = title[i];
    }
    msg.set_title.title[i] = '\0';
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/*
 * uk_set_cursor() — dynamically set the cursor sprite when hovering over this window.
 */
static inline void uk_set_cursor(uk_window_t *win, unsigned int cursor_type)
{
    if (!win || win->wid == 0) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_SET_CURSOR;
    msg.wid  = win->wid;
    msg.set_cursor.cursor = cursor_type;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/*
 * uk_set_always_on_top() — pin this window above normal windows in the Z-order.
 */
static inline void uk_set_always_on_top(uk_window_t *win, bool pinned)
{
    if (!win || win->wid == 0) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_SET_PINNED;
    msg.wid  = win->wid;
    msg.pinned.pinned = pinned ? 1 : 0;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/*
 * uk_window_connect() — full IPC handshake to create a compositor window.
 *
 * Parameters:
 *   win         — output: populated uk_window_t
 *   title       — window title (shown in title bar and taskbar)
 *   x, y        — screen position
 *   w, h        — client area dimensions
 *   map_addr    — virtual address to map the pixel buffer at
 *   server_chan — always 1 (azwm IPC channel)
 *
 * Returns 0 on success, negative on failure.
 *
 * A thin wrapper over uk_window_connect_ex() below with flags = 0 — kept as
 * its own function, rather than folding flags into this one with a default
 * argument C doesn't have, so the ~30 existing call sites that only ever
 * wanted a plain window stay untouched.
 */
static inline int uk_window_connect(uk_window_t *win,
                                     const char *title,
                                     int x, int y,
                                     unsigned int w, unsigned int h,
                                     void *map_addr,
                                     int server_chan);

/*
 * uk_window_connect_ex() — uk_window_connect() plus an AZ_WIN_FLAG_* bitset
 * (protocol.h) the compositor applies at create time — e.g.
 * AZ_WIN_FLAG_BLUR_BACKDROP, which asks azwm to blur whatever is behind this
 * window before compositing it (see the lock screen, the one current user).
 */
static inline int uk_window_connect_ex(uk_window_t *win,
                                        const char *title,
                                        int x, int y,
                                        unsigned int w, unsigned int h,
                                        void *map_addr,
                                        int server_chan,
                                        unsigned int flags)
{
    win->server_chan = server_chan;
    win->width       = w;   /* provisional: overwritten below with what the
                              * server actually allocated, once it replies */
    win->height      = h;
    win->x           = x;
    win->y           = y;

    win->client_chan = az_channel_create();
    if (win->client_chan < 0) return -1;

    /* Send create request */
    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type       = AZ_WM_CREATE_WINDOW;
    req.client_chan = (unsigned int)win->client_chan;
    req.create.x   = x;
    req.create.y   = y;
    req.create.w   = w;
    req.create.h   = h;

    /* Copy title */
    int i;
    for (i = 0; title[i] && i < 63; i++)
        req.create.title[i] = title[i];
    req.create.title[i] = '\0';
    req.create.flags = flags;

    de_log("[ui_kit] Sending AZ_WM_CREATE_WINDOW...");
    if (az_channel_send(server_chan, (az_ipc_msg_t *)&req) < 0) {
        de_log("[ui_kit] az_channel_send failed.");
        return -2;
    }

    de_log("[ui_kit] Waiting for AZ_WM_WINDOW_CREATED...");
    /* Wait for confirmation */
    az_wm_msg_t resp;
    for (;;) {
        int r = az_channel_recv(win->client_chan, (az_ipc_msg_t *)&resp);
        if (r < 0) {
            de_log("[ui_kit] az_channel_recv failed.");
            return -2;
        }
        if (resp.type == AZ_WM_WINDOW_CREATED) {
            if (resp.created.assigned_wid == 0) {
                de_log("[ui_kit] Server refused window creation.");
                return -1;
            }
            break;
        }
    }
    de_log("[ui_kit] Received AZ_WM_WINDOW_CREATED.");

    win->wid = resp.created.assigned_wid;

    /* The server clamps w/h to the screen before sizing the shmem surface
     * (compositor_create_window()) and now echoes back what it actually
     * allocated — trust that, not the request, for both the frame-commit
     * pixel count below and the backbuffer size: a window whose request got
     * clamped would otherwise believe it has more surface than was ever
     * mapped, and its first uk_invalidate() would stream pixels straight
     * past the end of it. */
    win->width  = resp.created.width  ? resp.created.width  : w;
    win->height = resp.created.height ? resp.created.height : h;

    /* Map the surface the compositor reads. */
    if (az_shmem_map((int)resp.created.shmem_id, map_addr) < 0) return -3;
    win->shared    = (unsigned int *)map_addr;
    win->pixels    = win->shared;
    win->shmem_id  = resp.created.shmem_id;
    win->backbuf_shmem_id = 0;

    /*
     * Then a private buffer of the same size to draw into.  Windows are laid
     * out 16 MB apart, which is more than any client area needs and keeps a
     * second window in the same process clear of the first.  If the
     * allocation fails the window still works — it just draws straight to the
     * surface, and may flicker while doing so.
     */
    static unsigned long s_next_backbuf = UK_BACKBUF_BASE;

    size_t bytes = (size_t)win->width * win->height * sizeof(unsigned int);
    int    pages = (int)((bytes + 4095) / 4096);
    int    back_id = az_shmem_create(pages);
    if (back_id >= 0) {
        void *back = (void *)s_next_backbuf;
        if (az_shmem_map(back_id, back) == 0) {
            win->pixels = (unsigned int *)back;
            win->backbuf_shmem_id = (unsigned int)back_id;
            s_next_backbuf += UK_BACKBUF_STEP;
        } else {
            az_shmem_destroy(back_id);
        }
    }

    /* Scissor starts as the whole client area — wide open until an app
     * narrows it with uk_push_clip(). */
    win->clip_x0 = 0;
    win->clip_y0 = 0;
    win->clip_x1 = (int)win->width;
    win->clip_y1 = (int)win->height;
    win->clip_depth = 0;
    win->font = NULL;

    return 0;
}

static inline int uk_window_connect(uk_window_t *win,
                                     const char *title,
                                     int x, int y,
                                     unsigned int w, unsigned int h,
                                     void *map_addr,
                                     int server_chan)
{
    return uk_window_connect_ex(win, title, x, y, w, h, map_addr, server_chan, 0);
}

/*
 * uk_handle_resize(win, msg) — apply an AZ_WM_WINDOW_RESIZED notification.
 *
 * azwm sends this after a drag-resize, edge-snap, or maximize/restore
 * reallocates the window's content surface (see compositor_resize_window())
 * — the old shmem_id is no longer backed by anything. Call this from the
 * app's own message loop on AZ_WM_WINDOW_RESIZED, before drawing again:
 * remaps `shared` (and the private backbuffer, if one exists) to the new
 * surface in place, at the same virtual addresses connect() chose, and
 * updates width/height so the very next draw call already lays out at the
 * new size instead of one frame behind it.
 *
 * Returns true on success. False means the window has no valid surface at
 * all any more (a shmem allocation failed) — draw calls and uk_invalidate()
 * are unsafe until the app reconnects, so the caller should treat this as
 * fatal for the window rather than attempt to keep using it.
 */
static inline bool uk_handle_resize(uk_window_t *win, const az_wm_msg_t *msg)
{
    if (msg->type != AZ_WM_WINDOW_RESIZED || msg->wid != win->wid) return false;

    unsigned int new_w = msg->resized.width;
    unsigned int new_h = msg->resized.height;

    /* Old mapping is unmapped from *this process* only — az_shmem_unmap
     * never touches the shmem object itself, which azwm already tore down
     * on its own side (compositor_resize_window()), so this is safe even
     * though win->shmem_id no longer names anything live. */
    az_shmem_unmap((int)win->shmem_id, win->shared);
    if (az_shmem_map((int)msg->resized.shmem_id, win->shared) < 0) {
        win->shmem_id = 0;
        return false;
    }
    win->shmem_id = msg->resized.shmem_id;

    if (win->pixels != win->shared) {
        /* Private backbuffer exists — resize it too, in place at the same
         * VA, so uk_invalidate()'s pixel count and this buffer's actual
         * capacity stay the same relationship they've always had. */
        az_shmem_unmap((int)win->backbuf_shmem_id, win->pixels);
        az_shmem_destroy((int)win->backbuf_shmem_id);

        size_t bytes = (size_t)new_w * new_h * sizeof(unsigned int);
        int    pages = (int)((bytes + 4095) / 4096);
        int    back_id = az_shmem_create(pages);
        if (back_id >= 0 && az_shmem_map(back_id, win->pixels) == 0) {
            win->backbuf_shmem_id = (unsigned int)back_id;
        } else {
            /* Couldn't get a private buffer back — same degrade-gracefully
             * path connect() takes when the backbuffer allocation fails in
             * the first place: draw straight on the shared surface. */
            if (back_id >= 0) az_shmem_destroy(back_id);
            win->pixels = win->shared;
            win->backbuf_shmem_id = 0;
        }
    }

    win->width  = new_w;
    win->height = new_h;

    /* A resize invalidates any sub-region an app had scissored to before
     * the surface changed shape — reopen to the whole (new) client area,
     * same as a freshly connected window, rather than leave a clip rect
     * sized for the old geometry active (or nested, orphaning pushes the
     * app never got to pop). */
    win->clip_x0 = 0;
    win->clip_y0 = 0;
    win->clip_x1 = (int)win->width;
    win->clip_y1 = (int)win->height;
    win->clip_depth = 0;

    return true;
}

/*
 * uk_set_zorder() — set a permanent Z-order band for this window.
 */
static inline void uk_set_zorder(uk_window_t *win, unsigned char band)
{
    az_wm_msg_t zmsg;
    memset(&zmsg, 0, sizeof(zmsg));
    zmsg.type = AZ_WM_SET_ZORDER_HINT;
    az_wm_zorder_payload_t *zpl = AZ_WM_MSG_ZORDER(&zmsg);
    zpl->wid  = win->wid;
    zpl->band = band;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&zmsg);
}

/*
 * uk_subscribe_events() — subscribe to broadcast window events.
 */
static inline void uk_subscribe_events(uk_window_t *win)
{
    az_wm_msg_t sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = AZ_WM_SUBSCRIBE_EVENTS;
    az_wm_subscribe_payload_t *pl = AZ_WM_MSG_SUBSCRIBE(&sub);
    pl->subscriber_chan = (unsigned int)win->client_chan;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&sub);
}

/*
 * uk_launch_app_arg() — request azwm to spawn an ELF binary, optionally
 * passing `arg` through as its argv[1] (e.g. a specific file to open).
 * Pass NULL or "" for `arg` to launch with no extra argument.
 */
static inline void uk_launch_app_arg(uk_window_t *win, const char *path, const char *arg)
{
    az_wm_msg_t lmsg;
    memset(&lmsg, 0, sizeof(lmsg));
    lmsg.type = AZ_WM_LAUNCH_APP;
    az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
    int j;
    for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
        pl->path[j] = path[j];
    pl->path[j] = '\0';
    if (arg) {
        for (j = 0; j < AZ_WM_LAUNCH_ARG_MAX - 1 && arg[j]; j++)
            pl->arg[j] = arg[j];
        pl->arg[j] = '\0';
    }
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&lmsg);
}

/*
 * uk_launch_app() — request azwm to spawn an ELF binary, with no argument.
 */
static inline void uk_launch_app(uk_window_t *win, const char *path)
{
    uk_launch_app_arg(win, path, NULL);
}

/*
 * uk_move_window() — move window to absolute screen coordinates (x, y).
 */
static inline void uk_move_window(uk_window_t *win, int x, int y)
{
    if (!win) return;
    az_wm_msg_t mmsg;
    memset(&mmsg, 0, sizeof(mmsg));
    mmsg.type = AZ_WM_MOVE_WINDOW;
    mmsg.wid  = win->wid;
    mmsg.move.x = x;
    mmsg.move.y = y;
    win->x = x;
    win->y = y;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&mmsg);
}

/*
 * uk_window_destroy() — tell azwm to destroy the window.
 */
static inline void uk_window_destroy(uk_window_t *win)
{
    if (!win || win->wid == 0) return;
    az_wm_msg_t dmsg;
    memset(&dmsg, 0, sizeof(dmsg));
    dmsg.type = AZ_WM_DESTROY_WINDOW;
    dmsg.wid  = win->wid;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&dmsg);
    win->wid = 0;
}

/*
 * uk_clear() — fill the window drawing buffer with a solid ARGB colour.
 */
static inline void uk_clear(uk_window_t *w, unsigned int col)
{
    if (!w || !w->pixels) return;
    unsigned int total = w->width * w->height;
    uk_fill_span(w->pixels, col, (int)total);
}

/* ============================================================================
 * Separator line helpers
 * ============================================================================ */

static inline void uk_hline(uk_window_t *w, int x, int y, int len, unsigned int col)
{
    uk_fill_rect(w, x, y, len, 1, col);
}

static inline void uk_vline(uk_window_t *w, int x, int y, int len, unsigned int col)
{
    uk_fill_rect(w, x, y, 1, len, col);
}

static inline void uk_line(uk_window_t *w, int x0, int y0, int x1, int y1, unsigned int col)
{
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        uk_put_pixel(w, x0, y0, col);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* ============================================================================
 * Modern Extended Controls
 * ============================================================================ */

/* Slider control (horizontal) */
static inline void uk_draw_slider(uk_window_t *w, int x, int y, int width, int value_pct, unsigned int fill_col)
{
    int height = 6;
    uk_fill_rounded_rect(w, x, y, width, height, 3, UK_SURFACE0);
    int fill_w = (width * value_pct) / 100;
    if (fill_w > 0)
        uk_fill_rounded_rect(w, x, y, fill_w, height, 3, fill_col);
    int handle_x = x + fill_w - 6;
    if (handle_x < x) handle_x = x;
    if (handle_x > x + width - 12) handle_x = x + width - 12;
    uk_fill_circle(w, handle_x + 6, y + 3, 6, UK_TEXT);
}

/* Forward declaration: defined further down with the other line-drawing
 * primitives, but the checkbox's checkmark glyph needs it earlier. */
static inline void uk_draw_line_aa(uk_window_t *w, int x0, int y0, int x1, int y1, unsigned int col);

/* Checkbox control */
static inline void uk_draw_checkbox(uk_window_t *w, int x, int y, int checked, const char *label)
{
    /* Same offset solid-fill shadow every other raised control here uses —
     * a hard-edged rect peeking out behind the box reads as a soft shadow
     * at this pixel scale without needing real alpha compositing. */
    uk_fill_rounded_rect(w, x + 1, y + 1, 18, 18, 4, UK_CRUST);

    unsigned int bg = checked ? UK_MAUVE : UK_SURFACE0;
    uk_fill_rounded_rect(w, x, y, 18, 18, 4, bg);
    uk_fill_rounded_rect(w, x + 2, y + 2, 14, 14, 3, checked ? UK_MAUVE : UK_BASE);
    if (checked) {
        /* A drawn checkmark (two joined strokes) rather than the ASCII
         * letter "v" — reads as a check glyph at any zoom instead of a
         * stray lowercase letter. */
        uk_draw_line_aa(w, x + 4,  y + 9, x + 7,  y + 13, UK_TEXT);
        uk_draw_line_aa(w, x + 7,  y + 13, x + 14, y + 4,  UK_TEXT);
    }
    if (label && label[0]) {
        uk_draw_text(w, x + 24, y + 1, label, UK_TEXT);
    }
}

/* Status badge pill */
static inline void uk_draw_badge(uk_window_t *w, int x, int y, const char *text, unsigned int bg_col, unsigned int fg_col)
{
    int len = uk_strlen(text);
    int bw = len * 8 + 12;
    uk_fill_rounded_rect(w, x, y, bw, 20, 10, bg_col);
    uk_draw_text(w, x + 6, y + 2, text, fg_col);
}

/* Floating context menu */
static inline void uk_draw_context_menu(uk_window_t *w, int x, int y, const char *items[], int count, int hovered_idx)
{
    int max_w = 120;
    int item_h = 28;
    for (int i = 0; i < count; i++) {
        int l = uk_strlen(items[i]) * 8 + 24;
        if (l > max_w) max_w = l;
    }
    int total_h = count * item_h + 8;
    /* Floating above the window's own content, so it gets the same offset
     * shadow as a modal — without it the menu reads as pasted onto the
     * surface below rather than hovering over it. */
    uk_fill_rounded_rect(w, x + 3, y + 4, max_w, total_h, 8, 0x40000000);
    uk_fill_rounded_rect(w, x, y, max_w, total_h, 8, UK_MANTLE);
    uk_fill_rounded_rect(w, x + 1, y + 1, max_w - 2, total_h - 2, 7, UK_BASE);

    for (int i = 0; i < count; i++) {
        int iy = y + 4 + i * item_h;
        if (i == hovered_idx) {
            uk_fill_rounded_rect(w, x + 4, iy, max_w - 8, item_h - 2, 4, UK_SURFACE0);
        }
        uk_draw_text(w, x + 12, iy + 4, items[i], (i == hovered_idx) ? UK_TEXT : UK_SUBTEXT1);
    }
}

/* Rounded container card */
static inline void uk_draw_card(uk_window_t *w, int x, int y, int width, int height, unsigned int bg_col)
{
    uk_fill_rounded_rect(w, x, y, width, height, 8, bg_col);
    uk_hline(w, x + 4, y, width - 8, UK_SURFACE1);
    uk_hline(w, x + 4, y + height - 1, width - 8, UK_CRUST);
}

/* Glassmorphic card container with top specular highlight */
static inline void uk_draw_card_glass(uk_window_t *w, int x, int y, int width, int height, unsigned int bg_col, const char *header)
{
    uk_fill_rounded_rect(w, x, y, width, height, 8, bg_col);
    /* 1px Specular top highlight */
    for (int px = x + 4; px < x + width - 4; px++) {
        if (px >= 0 && (unsigned int)px < w->width && y >= 0 && (unsigned int)y < w->height) {
            unsigned int *p = &w->pixels[(unsigned int)y * w->width + (unsigned int)px];
            *p = uk_blend(*p, 0xFFFFFFFF, 0x40);
        }
    }
    uk_hline(w, x + 4, y + height - 1, width - 8, UK_CRUST);
    if (header && header[0]) {
        uk_draw_text(w, x + 12, y + 10, header, UK_TEXT);
        uk_hline(w, x + 8, y + 28, width - 16, UK_SURFACE1);
    }
}

/* Modern gradient progress bar with glowing fill */
static inline void uk_draw_progress_bar_modern(uk_window_t *w, int x, int y, int width, int height, int percent, unsigned int color)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int r = height / 2;
    uk_fill_rounded_rect(w, x, y, width, height, r, UK_SURFACE0);
    int fill_w = (width * percent) / 100;
    if (fill_w > 0) {
        uk_fill_rounded_rect(w, x, y, fill_w, height, r, color);
        /* Glossy shine */
        if (height > 4) {
            for (int px = x + 2; px < x + fill_w - 2; px++) {
                if (px >= 0 && (unsigned int)px < w->width && y >= 0 && (unsigned int)y < w->height) {
                    unsigned int *p = &w->pixels[(unsigned int)y * w->width + (unsigned int)px];
                    *p = uk_blend(*p, 0xFFFFFFFF, 0x50);
                }
            }
        }
    }
}

/* Modern pill toggle switch */
static inline void uk_draw_toggle_modern(uk_window_t *w, int x, int y, int active)
{
    int width = 36;
    int height = 20;
    unsigned int bg = active ? UK_MAUVE : UK_SURFACE1;
    uk_fill_rounded_rect(w, x + 1, y + 2, width, height, 10, UK_CRUST); /* track shadow */
    uk_fill_rounded_rect(w, x, y, width, height, 10, bg);
    int knob_x = active ? (x + width - 18) : (x + 2);
    uk_fill_circle(w, knob_x + 8, y + 11, 7, UK_CRUST);  /* knob shadow, offset 1px down */
    uk_fill_circle(w, knob_x + 8, y + 10, 7, UK_TEXT);
}

/* Anti-aliased line drawing (Bresenham with subpixel alpha) */
static inline void uk_draw_line_aa(uk_window_t *w, int x0, int y0, int x1, int y1, unsigned int col)
{
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int e2;
    int ed = (dx + dy == 0) ? 1 : (dx > dy ? dx : dy);

    while (1) {
        if (x0 >= 0 && (unsigned int)x0 < w->width && y0 >= 0 && (unsigned int)y0 < w->height) {
            w->pixels[(unsigned int)y0 * w->width + (unsigned int)x0] = col;
        }
        e2 = err;
        int x2 = x0;
        if (2 * e2 >= -dx) {
            if (x0 == x1 && y0 == y1) break;
            if (e2 + dy < 2 * ed && y0 + sy >= 0 && (unsigned int)(y0 + sy) < w->height && x0 >= 0 && (unsigned int)x0 < w->width) {
                int dist = e2 + dy;
                if (dist < 0) dist = 0;
                int a = 255 - (255 * dist) / (2 * ed);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                unsigned int *p = &w->pixels[(unsigned int)(y0 + sy) * w->width + (unsigned int)x0];
                *p = uk_blend(*p, col, (unsigned int)a);
            }
            err -= dy;
            x0 += sx;
        }
        if (2 * e2 <= dy) {
            if (x2 == x1 && y0 == y1) break;
            if (dx - e2 < 2 * ed && x2 + sx >= 0 && (unsigned int)(x2 + sx) < w->width && y0 >= 0 && (unsigned int)y0 < w->height) {
                int dist = dx - e2;
                if (dist < 0) dist = 0;
                int a = 255 - (255 * dist) / (2 * ed);
                if (a < 0) a = 0;
                if (a > 255) a = 255;
                unsigned int *p = &w->pixels[(unsigned int)y0 * w->width + (unsigned int)(x2 + sx)];
                *p = uk_blend(*p, col, (unsigned int)a);
            }
            err += dx;
            y0 += sy;
        }
    }
}

/* Rounded rectangle border outline */
static inline void uk_draw_rounded_rect_outline(uk_window_t *w, int x, int y, int rw, int rh, int radius, unsigned int border_col)
{
    if (rw <= 0 || rh <= 0) return;
    if (radius * 2 > rw) radius = rw / 2;
    if (radius * 2 > rh) radius = rh / 2;

    /* Top & bottom horizontal segments */
    uk_fill_rect(w, x + radius, y, rw - 2 * radius, 1, border_col);
    uk_fill_rect(w, x + radius, y + rh - 1, rw - 2 * radius, 1, border_col);

    /* Left & right vertical segments */
    uk_fill_rect(w, x, y + radius, 1, rh - 2 * radius, border_col);
    uk_fill_rect(w, x + rw - 1, y + radius, 1, rh - 2 * radius, border_col);

    /* Four corner arcs */
    for (int cy = -radius; cy <= 0; cy++) {
        for (int cx = -radius; cx <= 0; cx++) {
            int d = cx * cx + cy * cy;
            if (d <= radius * radius && d >= (radius - 1) * (radius - 1)) {
                uk_put_pixel(w, x + radius + cx, y + radius + cy, border_col);
                uk_put_pixel(w, x + rw - 1 - radius - cx, y + radius + cy, border_col);
                uk_put_pixel(w, x + radius + cx, y + rh - 1 - radius - cy, border_col);
                uk_put_pixel(w, x + rw - 1 - radius - cx, y + rh - 1 - radius - cy, border_col);
            }
        }
    }
}

/* Modern text input box with placeholder, focus glow, and blinking cursor */
static inline void uk_draw_textbox(uk_window_t *w, int x, int y, int width, int height,
                                   const char *text, const char *placeholder, int focused, int cursor_pos)
{
    unsigned int bg_col = UK_MANTLE;
    unsigned int border_col = focused ? UK_BLUE : UK_SURFACE1;

    uk_fill_rounded_rect(w, x, y, width, height, 6, bg_col);
    uk_draw_rounded_rect_outline(w, x, y, width, height, 6, border_col);

    if (focused) {
        /* Subtle focus glow line */
        uk_fill_rect(w, x + 6, y + height - 2, width - 12, 2, UK_BLUE);
    }

    int text_y = y + (height - 16) / 2;
    if (text && text[0]) {
        uk_draw_text_clip(w, x + 8, text_y, text, UK_TEXT, width - 16);
    } else if (placeholder && placeholder[0]) {
        uk_draw_text_clip(w, x + 8, text_y, placeholder, UK_OVERLAY0, width - 16);
    }

    if (focused && cursor_pos >= 0) {
        int cx = x + 8 + cursor_pos * 8;
        if (cx < x + width - 10) {
            uk_fill_rect(w, cx, text_y, 2, 16, UK_BLUE);
        }
    }
}

/* Floating dark tooltip balloon */
static inline void uk_draw_tooltip(uk_window_t *w, int x, int y, const char *text)
{
    if (!text || !text[0]) return;
    int len = uk_strlen(text);
    int tw = len * 8 + 16;
    int th = 24;

    if (x + tw > (int)w->width - 4) x = (int)w->width - tw - 4;
    if (y + th > (int)w->height - 4) y = (int)w->height - th - 4;
    if (x < 4) x = 4;
    if (y < 4) y = 4;

    uk_fill_rounded_rect(w, x, y, tw, th, 4, UK_CRUST);
    uk_draw_rounded_rect_outline(w, x, y, tw, th, 4, UK_SURFACE2);
    uk_draw_text(w, x + 8, y + 4, text, UK_TEXT);
}

/* Plain (x, y, w, h) rect, used by uk_draw_confirm_dialog() below to hand
 * back where it put each button so a caller's own hit-testing doesn't have
 * to re-derive the layout. */
typedef struct { int x, y, w, h; } uk_rect_t;

static inline bool uk_hit_rect(uk_rect_t r, int mx, int my)
{
    return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h;
}

/*
 * uk_confirm_dialog_layout() — where uk_draw_confirm_dialog() below puts its
 * Cancel/confirm buttons for a given (mx, my, modal_w, modal_h, confirm_label),
 * with no drawing. Pure geometry so a caller's mouse-click handling can ask
 * "was that click on Cancel or Confirm?" the frame *was* drawn at — often a
 * different call site, and usually a different point in time, than the
 * draw — without hand-deriving the same button rects a second time and
 * risking the two silently drifting apart.
 */
static inline void uk_confirm_dialog_layout(int mx, int my, int modal_w, int modal_h,
                                            const char *confirm_label,
                                            uk_rect_t *out_cancel, uk_rect_t *out_confirm)
{
    if (!confirm_label) confirm_label = "OK";
    int confirm_w = uk_strlen(confirm_label) * 8 + 20;
    if (confirm_w < 70) confirm_w = 70;
    int cancel_w = 75;
    int by = my + modal_h - 38;

    uk_rect_t confirm_rect = { mx + modal_w - 15 - confirm_w, by, confirm_w, 26 };
    uk_rect_t cancel_rect  = { confirm_rect.x - 10 - cancel_w, by, cancel_w, 26 };

    if (out_cancel)  *out_cancel  = cancel_rect;
    if (out_confirm) *out_confirm = confirm_rect;
}

/*
 * uk_draw_confirm_dialog() — centred modal card: title, one optional line of
 * body text, and a Cancel/confirm button pair. This is the "delete this
 * item?" / "discard unsaved changes?" shape almost every app eventually
 * needs and used to hand-roll separately (filemanager's delete confirmation
 * drew exactly this by hand before it switched to this helper) — one
 * drawing routine now, instead of the rounded-rect-plus-two-buttons layout
 * being re-derived per app and slowly drifting out of sync visually.
 *
 * (mx, my, modal_w, modal_h) is the dialog's rect exactly as the caller
 * wants it drawn — already offset for centring and any open/close slide
 * animation; this only draws the frame there, it doesn't compute placement.
 * accent_col tints both the border and the confirm button (UK_RED for a
 * destructive action, UK_BLUE/UK_MAUVE for a neutral one) and is normally
 * also passed as the title's colour, so the heading reads as "this is what
 * the border means". `message` may be NULL/empty to omit the body line.
 *
 * Returns the two buttons' rects via out_cancel/out_confirm (either may be
 * NULL; see uk_confirm_dialog_layout() above for getting the same rects
 * back later without redrawing) for the caller to hit-test with uk_hit_rect().
 */
static inline void uk_draw_confirm_dialog(uk_window_t *w, int mx, int my,
                                          int modal_w, int modal_h,
                                          const char *title, unsigned int accent_col,
                                          const char *message,
                                          const char *confirm_label,
                                          uk_rect_t *out_cancel, uk_rect_t *out_confirm)
{
    uk_fill_rounded_rect(w, mx, my, modal_w, modal_h, 10, UK_CRUST);
    uk_draw_rounded_rect_outline(w, mx, my, modal_w, modal_h, 10, accent_col);
    uk_draw_text(w, mx + 16, my + 14, title, accent_col);
    if (message && message[0])
        uk_draw_text(w, mx + 16, my + 40, message, UK_TEXT);

    uk_rect_t cancel_rect, confirm_rect;
    uk_confirm_dialog_layout(mx, my, modal_w, modal_h, confirm_label, &cancel_rect, &confirm_rect);

    uk_draw_button(w, cancel_rect.x, cancel_rect.y, cancel_rect.w, cancel_rect.h,
                  "Cancel", UK_BTN_NORMAL);
    uk_draw_button(w, confirm_rect.x, confirm_rect.y, confirm_rect.w, confirm_rect.h,
                  confirm_label ? confirm_label : "OK", UK_BTN_PRESSED);

    if (out_cancel)  *out_cancel  = cancel_rect;
    if (out_confirm) *out_confirm = confirm_rect;
}

/* Circular arc gauge indicator with fixed-point trigonometric circle rasterizer */
static inline void uk_draw_gauge(uk_window_t *w, int cx, int cy, int radius, int percent, unsigned int color)
{
    if (radius <= 4 || !w || !w->pixels) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int r_in = radius - 4;
    int r_out = radius;

    for (int y = -r_out; y <= r_out; y++) {
        for (int x = -r_out; x <= r_out; x++) {
            int d = x * x + y * y;
            if (d >= r_in * r_in && d <= r_out * r_out) {
                uk_put_pixel(w, cx + x, cy + y, UK_SURFACE0);
            }
        }
    }

    static const short sin_tab[64] = {
           0,  100,  199,  296,  391,  482,  568,  649,
         724,  791,  851,  903,  946,  979, 1002, 1017,
        1024, 1017, 1002,  979,  946,  903,  851,  791,
         724,  649,  568,  482,  391,  296,  199,  100,
           0, -100, -199, -296, -391, -482, -568, -649,
        -724, -791, -851, -903, -946, -979,-1002,-1017,
       -1024,-1017,-1002, -979, -946, -903, -851, -791,
        -724, -649, -568, -482, -391, -296, -199, -100
    };

    int rad_mid = (r_in + r_out) / 2;
    int fill_steps = (percent * 64) / 100;

    for (int i = 0; i <= fill_steps && i < 64; i++) {
        int sin_val = sin_tab[i];
        int cos_val = sin_tab[(i + 16) & 63];
        int px = cx + (rad_mid * sin_val) / 1024;
        int py = cy - (rad_mid * cos_val) / 1024;
        uk_fill_circle(w, px, py, 2, color);
    }
}


/* ============================================================================
 * Clipboard helpers (cross-process, backed by azwm compositor)
 * ============================================================================ */

/**
 * uk_clipboard_set() — push up to AZ_WM_CLIPBOARD_TEXT_MAX-1 bytes of @text
 * into the system-wide clipboard.  The compositor stores it for all processes.
 */
static inline void uk_clipboard_set(uk_window_t *win, const char *text)
{
    if (!text || !win) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_CLIPBOARD_SET;
    az_wm_clipboard_payload_t *pl = AZ_WM_MSG_CLIPBOARD(&msg);
    unsigned int len = 0;
    while (text[len] && len < AZ_WM_CLIPBOARD_TEXT_MAX - 1) len++;
    pl->len = len;
    unsigned int i;
    for (i = 0; i < len; i++) pl->text[i] = text[i];
    pl->text[len] = '\0';
    pl->reply_chan = 0;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/**
 * uk_clipboard_get() — read the compositor's clipboard into @buf (up to
 * @maxlen-1 bytes + NUL).  Returns bytes copied, or -1 on error.
 */
static inline int uk_clipboard_get(uk_window_t *win, char *buf, int maxlen)
{
    if (!win || !buf || maxlen <= 0) return -1;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_CLIPBOARD_GET;
    az_wm_clipboard_payload_t *pl = AZ_WM_MSG_CLIPBOARD(&msg);
    pl->len        = 0;
    pl->reply_chan = (unsigned int)win->client_chan;
    if (az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg) < 0) return -1;

    az_wm_msg_t resp;
    az_wm_msg_t deferred[8];
    int def_count = 0;
    int found = 0;

    for (int it = 0; it < 32; it++) {
        if (az_channel_recv(win->client_chan, (az_ipc_msg_t *)&resp) < 0) break;
        if (resp.type == AZ_WM_CLIPBOARD_DATA) {
            found = 1;
            break;
        }
        if (def_count < 8) {
            deferred[def_count++] = resp;
        }
    }

    /* Re-inject any interleaved window events back into client_chan */
    for (int k = 0; k < def_count; k++) {
        az_channel_send_nb(win->client_chan, (az_ipc_msg_t *)&deferred[k]);
    }

    if (!found) return -1;

    az_wm_clipboard_payload_t *rpl = AZ_WM_MSG_CLIPBOARD(&resp);
    int copy = (int)rpl->len;
    if (copy >= maxlen) copy = maxlen - 1;
    int j;
    for (j = 0; j < copy; j++) buf[j] = rpl->text[j];
    buf[copy] = '\0';
    return copy;
}

/* ============================================================================
 * Toast notification helpers
 * ============================================================================ */

/**
 * uk_draw_toast() — render a glassmorphic notification bubble in the
 * bottom-right corner of the window.  Call once per frame while visible.
 *
 * @title  Short heading (accent colour)
 * @body   Message body  (subtext colour)
 * @alpha  0 = invisible, 255 = fully opaque (callers decrement each frame)
 */
static inline void uk_draw_toast(uk_window_t *w,
                                  const char *title, const char *body,
                                  unsigned int alpha)
{
    if (!w || !w->pixels || alpha == 0) return;
    int toast_w = 280;
    int toast_h = 64;
    int margin  = 12;
    int tx = (int)w->width  - toast_w - margin;
    int ty = (int)w->height - toast_h - margin;
    if (tx < 0) tx = 0;
    if (ty < 0) ty = 0;
    unsigned int bg = 0xFF1E1E2E;
    uk_fill_rounded_rect(w, tx, ty, toast_w, toast_h, 10, bg);
    /* Specular top-edge highlight */
    int px;
    for (px = tx + 10; px < tx + toast_w - 10; px++) {
        if (px >= 0 && (unsigned int)px < w->width &&
            ty >= 0 && (unsigned int)ty < w->height) {
            unsigned int *p = &w->pixels[(unsigned int)ty * w->width + (unsigned int)px];
            *p = uk_blend(*p, 0xFFFFFFFF, 0x28);
        }
    }
    uk_fill_rect(w, tx, ty + 10, 3, toast_h - 20, UK_MAUVE);
    uk_fill_circle(w, tx + 18, ty + 20, 7, UK_MAUVE);
    uk_fill_circle(w, tx + 18, ty + 20, 4, bg);
    uk_fill_rect(w, tx + 16, ty + 26, 5, 3, UK_MAUVE);
    if (title && title[0])
        uk_draw_text_clip(w, tx + 34, ty + 10, title, UK_MAUVE, toast_w - 40);
    uk_fill_rect(w, tx + 34, ty + 28, toast_w - 42, 1, UK_SURFACE1);
    if (body && body[0])
        uk_draw_text_clip(w, tx + 34, ty + 34, body, UK_SUBTEXT1, toast_w - 42);
    uk_draw_text(w, tx + toast_w - 16, ty + 6, "x", UK_OVERLAY0);
    uk_draw_rounded_rect_outline(w, tx, ty, toast_w, toast_h, 10, UK_SURFACE1);
    if (alpha < 255) {
        unsigned int total = w->width * w->height;
        uk_apply_alpha(w->pixels, (size_t)total, alpha);
    }
}

/**
 * uk_notify() — broadcast AZ_WM_NOTIFY so notifyd renders a toast overlay.
 */
static inline void uk_notify(uk_window_t *win,
                              const char *title, const char *body)
{
    if (!win) return;
    az_wm_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AZ_WM_NOTIFY;
    az_wm_notify_payload_t *pl = AZ_WM_MSG_NOTIFY(&msg);
    int i;
    for (i = 0; title && title[i] && i < AZ_WM_NOTIFY_TITLE_MAX - 1; i++)
        pl->title[i] = title[i];
    pl->title[i] = '\0';
    for (i = 0; body && body[i] && i < AZ_WM_NOTIFY_BODY_MAX - 1; i++)
        pl->body[i] = body[i];
    pl->body[i] = '\0';
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&msg);
}

/* ============================================================================
 * Modern UI Component Suite
 * ============================================================================ */

/*
 * uk_draw_segmented_control() — modern segmented button bar / pill tabs.
 */
static inline void uk_draw_segmented_control(uk_window_t *w,
                                            int x, int y, int width, int height,
                                            const char **items, int count,
                                            int selected_idx, int hover_idx)
{
    if (!w || !w->pixels || width <= 0 || height <= 0 || count <= 0) return;
    int radius = height / 2;
    if (radius > 8) radius = 8;

    uk_fill_rounded_rect(w, x, y, width, height, radius, UK_SURFACE0);
    uk_draw_rounded_rect_outline(w, x, y, width, height, radius, UK_SURFACE1);

    int seg_w = (width - 4) / count;
    if (seg_w <= 0) return;

    for (int i = 0; i < count; i++) {
        int sx = x + 2 + i * seg_w;
        int sy = y + 2;
        int sw = (i == count - 1) ? (x + width - 2 - sx) : seg_w;
        int sh = height - 4;
        int sradius = (radius > 2) ? radius - 2 : 2;

        if (i == selected_idx) {
            uk_fill_rounded_rect(w, sx, sy + 1, sw, sh, sradius, 0x30000000);
            uk_fill_rounded_rect(w, sx, sy, sw, sh, sradius, UK_SURFACE2);
            if (sh > 4) {
                for (int hx = sx + 2; hx < sx + sw - 2; hx++) {
                    if (hx >= 0 && (unsigned int)hx < w->width &&
                        sy >= 0 && (unsigned int)sy < w->height) {
                        unsigned int *p = &w->pixels[(unsigned int)sy * w->width + (unsigned int)hx];
                        *p = uk_blend(*p, 0xFFFFFFFF, 0x40);
                    }
                }
            }
        } else if (i == hover_idx) {
            uk_fill_rounded_rect(w, sx, sy, sw, sh, sradius, UK_SURFACE1);
        }

        unsigned int text_col = (i == selected_idx) ? UK_TEXT : ((i == hover_idx) ? UK_LAVENDER : UK_SUBTEXT0);
        int len = uk_strlen(items[i]);
        int tx = sx + (sw - len * 8) / 2;
        int ty = sy + (sh - 16) / 2;
        if (i == selected_idx) {
            uk_draw_text_bold(w, tx, ty, items[i], text_col);
        } else {
            uk_draw_text(w, tx, ty, items[i], text_col);
        }
    }
}

/*
 * uk_draw_search_bar() — search input with magnifying glass and clear button.
 */
static inline void uk_draw_search_bar(uk_window_t *w,
                                      int x, int y, int width, int height,
                                      const char *placeholder, const char *text,
                                      bool focused, bool hover)
{
    if (!w || !w->pixels || width <= 0 || height <= 0) return;
    int radius = height / 2;
    if (radius > 8) radius = 8;

    unsigned int bg_col = focused ? UK_BASE : (hover ? UK_SURFACE0 : UK_MANTLE);
    unsigned int border_col = focused ? UK_MAUVE : (hover ? UK_SURFACE2 : UK_SURFACE1);

    uk_fill_rounded_rect(w, x, y, width, height, radius, bg_col);
    uk_draw_rounded_rect_outline(w, x, y, width, height, radius, border_col);

    int icon_cx = x + 16;
    int icon_cy = y + height / 2 - 1;
    unsigned int icon_col = focused ? UK_MAUVE : UK_OVERLAY1;
    uk_fill_circle(w, icon_cx, icon_cy, 5, icon_col);
    uk_fill_circle(w, icon_cx, icon_cy, 3, bg_col);
    uk_draw_line_aa(w, icon_cx + 3, icon_cy + 3, icon_cx + 7, icon_cy + 7, icon_col);

    int text_x = x + 28;
    int text_y = y + (height - 16) / 2;
    int max_w = width - 50;

    if (text && text[0]) {
        uk_draw_text_clip(w, text_x, text_y, text, UK_TEXT, max_w);
        if (focused) {
            int cur_x = text_x + uk_strlen(text) * 8;
            if (cur_x < x + width - 24) {
                uk_fill_rect(w, cur_x, text_y, 2, 16, UK_MAUVE);
            }
        }
        int clear_x = x + width - 18;
        int clear_y = y + height / 2;
        uk_fill_circle(w, clear_x, clear_y, 7, UK_SURFACE1);
        uk_draw_text(w, clear_x - 3, clear_y - 8, "x", UK_SUBTEXT0);
    } else {
        if (placeholder && placeholder[0]) {
            uk_draw_text_clip(w, text_x, text_y, placeholder, UK_OVERLAY0, max_w);
        }
        if (focused) {
            uk_fill_rect(w, text_x, text_y, 2, 16, UK_MAUVE);
        }
    }
}

/*
 * uk_draw_dropdown_select() — select picker button with chevron arrow.
 */
static inline void uk_draw_dropdown_select(uk_window_t *w,
                                          int x, int y, int width, int height,
                                          const char *selected_text,
                                          bool open, bool hover)
{
    if (!w || !w->pixels || width <= 0 || height <= 0) return;
    int radius = 6;
    unsigned int bg_col = open ? UK_SURFACE1 : (hover ? UK_SURFACE0 : UK_MANTLE);
    unsigned int border_col = open ? UK_BLUE : (hover ? UK_SURFACE2 : UK_SURFACE1);

    uk_fill_rounded_rect(w, x, y, width, height, radius, bg_col);
    uk_draw_rounded_rect_outline(w, x, y, width, height, radius, border_col);

    int ty = y + (height - 16) / 2;
    if (selected_text && selected_text[0]) {
        uk_draw_text_clip(w, x + 10, ty, selected_text, UK_TEXT, width - 30);
    }

    int ax = x + width - 16;
    int ay = y + height / 2;
    unsigned int arrow_col = open ? UK_BLUE : UK_SUBTEXT0;
    if (open) {
        uk_draw_line_aa(w, ax - 4, ay + 2, ax, ay - 2, arrow_col);
        uk_draw_line_aa(w, ax, ay - 2, ax + 4, ay + 2, arrow_col);
    } else {
        uk_draw_line_aa(w, ax - 4, ay - 2, ax, ay + 2, arrow_col);
        uk_draw_line_aa(w, ax, ay + 2, ax + 4, ay - 2, arrow_col);
    }
}

/*
 * uk_draw_metric_card() — modern dashboard card with bold statistic and accent badge.
 */
static inline void uk_draw_metric_card(uk_window_t *w,
                                      int x, int y, int width, int height,
                                      const char *title, const char *value,
                                      const char *subtext, unsigned int accent_col)
{
    if (!w || !w->pixels || width <= 0 || height <= 0) return;
    int radius = 8;

    uk_fill_rounded_rect(w, x + 1, y + 2, width, height, radius, 0x25000000);
    uk_fill_rounded_rect(w, x, y, width, height, radius, UK_BASE);
    uk_draw_rounded_rect_outline(w, x, y, width, height, radius, UK_SURFACE0);

    uk_fill_rounded_rect(w, x + 6, y + 2, width - 12, 3, 2, accent_col);

    if (title && title[0]) {
        uk_draw_text_clip(w, x + 12, y + 12, title, UK_SUBTEXT1, width - 24);
    }

    if (value && value[0]) {
        uk_draw_text_2x(w, x + 12, y + 30, value, UK_TEXT);
    }

    if (subtext && subtext[0]) {
        int badge_y = y + height - 24;
        uk_fill_rounded_rect(w, x + 12, badge_y, uk_strlen(subtext) * 8 + 12, 18, 4, UK_SURFACE0);
        uk_draw_text(w, x + 18, badge_y + 1, subtext, accent_col);
    }
}

/*
 * uk_draw_spinner() — circular progress spinner for loading animations.
 */
static inline void uk_draw_spinner(uk_window_t *w,
                                  int cx, int cy, int radius,
                                  int tick_step, unsigned int accent)
{
    if (!w || !w->pixels || radius <= 0) return;
    static const int offsets[8][2] = {
        { 0, -1 }, { 7, -7 }, { 1, 0 }, { 7, 7 },
        { 0, 1 }, { -7, 7 }, { -1, 0 }, { -7, -7 }
    };
    int r = radius;
    int r_diag = (radius * 707) / 1000;

    int active_idx = (tick_step / 2) % 8;
    for (int i = 0; i < 8; i++) {
        int dot_x = cx + ((i % 2 == 0) ? (offsets[i][0] * r) : ((offsets[i][0] > 0 ? 1 : -1) * r_diag));
        int dot_y = cy + ((i % 2 == 0) ? (offsets[i][1] * r) : ((offsets[i][1] > 0 ? 1 : -1) * r_diag));

        int dist = (8 + i - active_idx) % 8;
        unsigned int alpha = (dist == 0) ? 255 : (40 + (7 - dist) * 28);
        unsigned int col = uk_blend(UK_BASE, accent, alpha);
        uk_fill_circle(w, dot_x, dot_y, (dist == 0) ? 3 : 2, col);
    }
}

/*
 * uk_draw_avatar() — circular user avatar with bold initials.
 */
static inline void uk_draw_avatar(uk_window_t *w,
                                 int cx, int cy, int radius,
                                 const char *initials, unsigned int bg_col)
{
    if (!w || !w->pixels || radius <= 0) return;
    uk_fill_circle(w, cx, cy, radius, bg_col);
    if (initials && initials[0]) {
        int len = uk_strlen(initials);
        int tx = cx - (len * 8) / 2;
        int ty = cy - 8;
        uk_draw_text_bold(w, tx, ty, initials, UK_BASE);
    }
}

/*
 * uk_draw_divider_text() — horizontal rule line with centered pill label.
 */
static inline void uk_draw_divider_text(uk_window_t *w,
                                       int x, int y, int width,
                                       const char *label)
{
    if (!w || !w->pixels || width <= 0) return;
    int line_y = y + 8;
    if (label && label[0]) {
        int len = uk_strlen(label);
        int text_w = len * 8 + 16;
        int cx = x + width / 2;
        int left_w = (width - text_w) / 2;

        if (left_w > 0) {
            uk_fill_rect(w, x, line_y, left_w, 1, UK_SURFACE1);
            uk_fill_rect(w, cx + text_w / 2, line_y, left_w, 1, UK_SURFACE1);
        }
        uk_fill_rounded_rect(w, cx - text_w / 2, y, text_w, 16, 4, UK_SURFACE0);
        uk_draw_text(w, cx - text_w / 2 + 8, y, label, UK_OVERLAY1);
    } else {
        uk_fill_rect(w, x, line_y, width, 1, UK_SURFACE1);
    }
}

