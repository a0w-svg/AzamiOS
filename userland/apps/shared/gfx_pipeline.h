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
#if defined(__x86_64__) && !defined(__KERNEL__)
#include <emmintrin.h>
#include <immintrin.h>

/* Runtime AVX2 gate. Every userspace binary that pulls in this header is
 * compiled for the SSE2-only x86_64 baseline (no -march on the build line),
 * so an AVX2 codepath can only be reached through a function carrying
 * __attribute__((target("avx2"))) — never through -mavx2 on the whole
 * translation unit, which would make the binary crash with #UD on any CPU
 * (or hypervisor) that doesn't advertise AVX2. This checks not just CPUID's
 * AVX2 bit but that the OS has actually turned on YMM state via XSETBV
 * (CPUID.1:ECX.OSXSAVE + XCR0 bits 1:2); skipping that check is the classic
 * way to fault on a kernel that supports the feature but hasn't enabled it. */
static inline int gfx_cpu_has_avx2(void)
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
#endif

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

/* ── SIMD / Vectorized Span Blitting, Filling & Tinting ──────────────────── */

#if defined(__x86_64__) && !defined(__KERNEL__)
/*
 * Every span function below has an SSE2 4-pixels/iteration path (the x86_64
 * baseline, always present) and an AVX2 8-pixels/iteration path used when
 * gfx_cpu_has_avx2() says the CPU and OS both support it. The one-block
 * helpers (*_block4_sse2) hold the actual per-4-pixel math and are shared by
 * both: the SSE2 loop calls one every iteration, and the AVX2 loop calls one
 * exactly once, on whatever 4-7 pixel remainder is too short for another
 * 8-wide step. That sharing is deliberate, not just tidy — the blend formulas
 * round the way they do (each 128-bit lane forces its output alpha to 0xFF
 * rather than computing it, since a compositor's damage span is always drawn
 * onto an already-opaque backbuffer) and reimplementing that by hand a second
 * time for an AVX2 remainder is exactly how the two paths would quietly drift
 * apart on non-multiple-of-8 span widths. One implementation, called from
 * both places, can't disagree with itself.
 *
 * The AVX2 *_avx2() bodies process their main loop 128 bits at a time inside
 * each 256-bit register: every unpack/shuffle/pack instruction they use stays
 * within its own 128-bit half (no cross-lane traffic), so replicating the
 * SSE2 constants (inv_a, src_term, alpha_mask) identically across both halves
 * runs the same SSE2 computation twice in parallel — not a different 8-wide
 * algorithm that happens to agree with it.
 */

static inline void gfx_tint_block4_sse2(uint32_t *dst, __m128i inv_a_vec,
                                        __m128i src_term, __m128i alpha_mask)
{
    __m128i zero = _mm_setzero_si128();
    __m128i d = _mm_loadu_si128((const __m128i *)dst);

    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);

    __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(d_lo, inv_a_vec), src_term), 8);
    __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(d_hi, inv_a_vec), src_term), 8);

    __m128i res = _mm_packus_epi16(res_lo, res_hi);
    res = _mm_or_si128(res, alpha_mask);
    _mm_storeu_si128((__m128i *)dst, res);
}

/* One 4-pixel Porter-Duff SRC_OVER step, including the opaque/transparent
 * shortcuts. Skipping those for a "remainder" block would not just cost
 * speed: the blend formula's fixed-point rounding does not reproduce an
 * opaque source exactly (e.g. sa=255 puts (255*255+127)>>8 == 254 through the
 * multiply, one below the source's own 255), so the direct-store shortcut is
 * load-bearing for correctness, not just an optimization. */
static inline void gfx_blend_block4_sse2(uint32_t *dst, const uint32_t *src)
{
    __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000U);
    __m128i zero = _mm_setzero_si128();

    __m128i s = _mm_loadu_si128((const __m128i *)src);
    __m128i s_alpha = _mm_and_si128(s, alpha_mask);

    __m128i is_opaque = _mm_cmpeq_epi32(s_alpha, alpha_mask);
    if (_mm_movemask_epi8(is_opaque) == 0xFFFF) {
        _mm_storeu_si128((__m128i *)dst, s);
        return;
    }

    __m128i is_zero = _mm_cmpeq_epi32(s_alpha, zero);
    if (_mm_movemask_epi8(is_zero) == 0xFFFF) {
        return;
    }

    __m128i d = _mm_loadu_si128((const __m128i *)dst);

    __m128i s_lo = _mm_unpacklo_epi8(s, zero);
    __m128i d_lo = _mm_unpacklo_epi8(d, zero);
    __m128i a_lo = _mm_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
    a_lo = _mm_shufflehi_epi16(a_lo, _MM_SHUFFLE(3, 3, 3, 3));
    __m128i inv_a_lo = _mm_sub_epi16(_mm_set1_epi16(255), a_lo);
    __m128i res_lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(s_lo, a_lo),
                                                                _mm_mullo_epi16(d_lo, inv_a_lo)),
                                                  _mm_set1_epi16(127)), 8);

    __m128i s_hi = _mm_unpackhi_epi8(s, zero);
    __m128i d_hi = _mm_unpackhi_epi8(d, zero);
    __m128i a_hi = _mm_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
    a_hi = _mm_shufflehi_epi16(a_hi, _MM_SHUFFLE(3, 3, 3, 3));
    __m128i inv_a_hi = _mm_sub_epi16(_mm_set1_epi16(255), a_hi);
    __m128i res_hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(s_hi, a_hi),
                                                                _mm_mullo_epi16(d_hi, inv_a_hi)),
                                                  _mm_set1_epi16(127)), 8);

    __m128i res = _mm_packus_epi16(res_lo, res_hi);
    res = _mm_or_si128(res, alpha_mask);
    _mm_storeu_si128((__m128i *)dst, res);
}

__attribute__((target("avx2")))
static void gfx_fill_span_avx2(uint32_t *dst, uint32_t color, int count)
{
    __m256i c256 = _mm256_set1_epi32((int)color);
    int i = 0;
    for (; i <= count - 8; i += 8) {
        _mm256_storeu_si256((__m256i *)(dst + i), c256);
    }
    for (; i < count; i++) dst[i] = color;
}

__attribute__((target("avx2")))
static void gfx_blend_tint_span_avx2(uint32_t *dst, uint32_t color, int count)
{
    uint32_t a  = (color >> 24) & 0xFF;
    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8)  & 0xFF;
    uint32_t sb = color         & 0xFF;
    uint32_t inv_a = 255 - a;

    short t_b = (short)(sb * a + 127), t_g = (short)(sg * a + 127);
    short t_r = (short)(sr * a + 127), t_a = (short)(255 * 255);

    __m256i zero      = _mm256_setzero_si256();
    __m256i inv_a_vec = _mm256_set1_epi16((short)inv_a);
    __m256i src_term  = _mm256_setr_epi16(t_b, t_g, t_r, t_a, t_b, t_g, t_r, t_a,
                                          t_b, t_g, t_r, t_a, t_b, t_g, t_r, t_a);
    __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000U);

    int i = 0;
    for (; i <= count - 8; i += 8) {
        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));

        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);

        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(d_lo, inv_a_vec), src_term), 8);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(d_hi, inv_a_vec), src_term), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        res = _mm256_or_si256(res, alpha_mask);
        _mm256_storeu_si256((__m256i *)(dst + i), res);
    }
    /* 4-7 pixels left: one shared SSE2 block, not scalar — see the comment
     * above this section for why skipping it would disagree with the SSE2
     * path on the alpha channel. */
    if (i <= count - 4) {
        __m128i inv_a_vec4 = _mm_set1_epi16((short)inv_a);
        __m128i src_term4  = _mm_setr_epi16(t_b, t_g, t_r, t_a, t_b, t_g, t_r, t_a);
        __m128i alpha_mask4 = _mm_set1_epi32((int)0xFF000000U);
        gfx_tint_block4_sse2(dst + i, inv_a_vec4, src_term4, alpha_mask4);
        i += 4;
    }
    for (; i < count; i++) dst[i] = gfx_blend_pixel(dst[i], color);
}

__attribute__((target("avx2")))
static void gfx_blend_span_avx2(uint32_t *dst, const uint32_t *src, int count)
{
    __m256i alpha_mask = _mm256_set1_epi32((int)0xFF000000U);
    __m256i zero       = _mm256_setzero_si256();

    int i = 0;
    for (; i <= count - 8; i += 8) {
        __m256i s = _mm256_loadu_si256((const __m256i *)(src + i));
        __m256i s_alpha = _mm256_and_si256(s, alpha_mask);

        __m256i is_opaque = _mm256_cmpeq_epi32(s_alpha, alpha_mask);
        if ((unsigned)_mm256_movemask_epi8(is_opaque) == 0xFFFFFFFFu) {
            _mm256_storeu_si256((__m256i *)(dst + i), s);
            continue;
        }

        __m256i is_zero = _mm256_cmpeq_epi32(s_alpha, zero);
        if ((unsigned)_mm256_movemask_epi8(is_zero) == 0xFFFFFFFFu) {
            continue;
        }

        __m256i d = _mm256_loadu_si256((const __m256i *)(dst + i));

        __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
        __m256i d_lo = _mm256_unpacklo_epi8(d, zero);
        __m256i a_lo = _mm256_shufflelo_epi16(s_lo, _MM_SHUFFLE(3, 3, 3, 3));
        a_lo = _mm256_shufflehi_epi16(a_lo, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a_lo = _mm256_sub_epi16(_mm256_set1_epi16(255), a_lo);
        __m256i res_lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(s_lo, a_lo),
                                                                              _mm256_mullo_epi16(d_lo, inv_a_lo)),
                                                             _mm256_set1_epi16(127)), 8);

        __m256i s_hi = _mm256_unpackhi_epi8(s, zero);
        __m256i d_hi = _mm256_unpackhi_epi8(d, zero);
        __m256i a_hi = _mm256_shufflelo_epi16(s_hi, _MM_SHUFFLE(3, 3, 3, 3));
        a_hi = _mm256_shufflehi_epi16(a_hi, _MM_SHUFFLE(3, 3, 3, 3));
        __m256i inv_a_hi = _mm256_sub_epi16(_mm256_set1_epi16(255), a_hi);
        __m256i res_hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(s_hi, a_hi),
                                                                              _mm256_mullo_epi16(d_hi, inv_a_hi)),
                                                             _mm256_set1_epi16(127)), 8);

        __m256i res = _mm256_packus_epi16(res_lo, res_hi);
        res = _mm256_or_si256(res, alpha_mask);
        _mm256_storeu_si256((__m256i *)(dst + i), res);
    }
    /* 4-7 pixels left: one shared SSE2 block (opaque/transparent shortcuts
     * included), not scalar — same reasoning as the tint path above. */
    if (i <= count - 4) {
        gfx_blend_block4_sse2(dst + i, src + i);
        i += 4;
    }
    for (; i < count; i++) dst[i] = gfx_blend_pixel(dst[i], src[i]);
}
#endif /* __x86_64__ && !__KERNEL__ */

static inline void gfx_fill_span_fast(uint32_t *dst, uint32_t color, int count)
{
    if (count <= 0) return;
    int i = 0;
#if defined(__x86_64__) && !defined(__KERNEL__)
    if (gfx_cpu_has_avx2()) { gfx_fill_span_avx2(dst, color, count); return; }
    __m128i c128 = _mm_set1_epi32((int)color);
    for (; i <= count - 4; i += 4) {
        _mm_storeu_si128((__m128i *)(dst + i), c128);
    }
#else
    uint64_t col64 = ((uint64_t)color << 32) | (uint64_t)color;
    while (((uintptr_t)&dst[i] & 7) && i < count) {
        dst[i] = color;
        i++;
    }
    uint64_t *d64 = (uint64_t *)&dst[i];
    int count64 = (count - i) / 2;
    for (int j = 0; j < count64; j++) {
        d64[j] = col64;
    }
    i += count64 * 2;
#endif
    while (i < count) {
        dst[i] = color;
        i++;
    }
}

static inline void gfx_blend_tint_span_fast(uint32_t *dst, uint32_t color, int count)
{
    if (count <= 0) return;
    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0) return;
    if (a == 255) {
        gfx_fill_span_fast(dst, color, count);
        return;
    }

    int i = 0;
#if defined(__x86_64__) && !defined(__KERNEL__)
    if (gfx_cpu_has_avx2()) { gfx_blend_tint_span_avx2(dst, color, count); return; }
    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8)  & 0xFF;
    uint32_t sb = color         & 0xFF;
    uint32_t inv_a = 255 - a;

    __m128i inv_a_vec = _mm_set1_epi16((short)inv_a);
    __m128i src_term = _mm_setr_epi16((short)(sb * a + 127),
                                      (short)(sg * a + 127),
                                      (short)(sr * a + 127),
                                      (short)(255 * 255),
                                      (short)(sb * a + 127),
                                      (short)(sg * a + 127),
                                      (short)(sr * a + 127),
                                      (short)(255 * 255));
    __m128i alpha_mask = _mm_set1_epi32((int)0xFF000000U);

    for (; i <= count - 4; i += 4) {
        gfx_tint_block4_sse2(dst + i, inv_a_vec, src_term, alpha_mask);
    }
#endif
    for (; i < count; i++) {
        dst[i] = gfx_blend_pixel(dst[i], color);
    }
}

static inline void gfx_blend_span_fast(uint32_t *dst, const uint32_t *src, int count)
{
    if (count <= 0) return;
    int i = 0;
#if defined(__x86_64__) && !defined(__KERNEL__)
    if (gfx_cpu_has_avx2()) { gfx_blend_span_avx2(dst, src, count); return; }
    for (; i <= count - 4; i += 4) {
        gfx_blend_block4_sse2(dst + i, src + i);
    }
#else
    for (; i <= count - 4; i += 4) {
        uint32_t s0 = src[i], s1 = src[i+1], s2 = src[i+2], s3 = src[i+3];
        if ((s0 & s1 & s2 & s3 & 0xFF000000) == 0xFF000000) {
            dst[i]   = s0;
            dst[i+1] = s1;
            dst[i+2] = s2;
            dst[i+3] = s3;
            continue;
        }
        dst[i]   = gfx_blend_pixel(dst[i], s0);
        dst[i+1] = gfx_blend_pixel(dst[i+1], s1);
        dst[i+2] = gfx_blend_pixel(dst[i+2], s2);
        dst[i+3] = gfx_blend_pixel(dst[i+3], s3);
    }
#endif
    for (; i < count; i++) {
        dst[i] = gfx_blend_pixel(dst[i], src[i]);
    }
}

static inline void gfx_clear(gfx_surface_t *surf, uint32_t color)
{
    if (!surf || !surf->pixels) return;
    int stride = surf->stride;
    int h = surf->height;
    int w = surf->width;
    for (int y = 0; y < h; y++) {
        gfx_fill_span_fast(&surf->pixels[y * stride], color, w);
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

    uint32_t a = (color >> 24) & 0xFF;
    if (a == 0) return;

    int stride = surf->stride;
    int count = x2 - x1;

    if (a == 255) {
        for (int py = y1; py < y2; py++) {
            gfx_fill_span_fast(&surf->pixels[py * stride + x1], color, count);
        }
    } else {
        for (int py = y1; py < y2; py++) {
            gfx_blend_tint_span_fast(&surf->pixels[py * stride + x1], color, count);
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

/* (gfx_fill_span_fast and gfx_blend_span_fast are declared above with SIMD) */

/* ── Frosted Glass / Dual-Pass Fast Box Blur ──────────────────────────────── */

static inline void gfx_blur_box_horizontal(const uint32_t *src, uint32_t *dst, int w, int h, int stride, int r)
{
    if (r <= 0 || w <= 0 || h <= 0) return;
    int div = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        int ti = y * stride;
        int li = ti, ri = ti + r;
        uint32_t fv = src[ti], lv = src[ti + w - 1];
        int val_a = ((fv >> 24) & 0xFF) * (r + 1);
        int val_r = ((fv >> 16) & 0xFF) * (r + 1);
        int val_g = ((fv >> 8)  & 0xFF) * (r + 1);
        int val_b = (fv         & 0xFF) * (r + 1);

        for (int j = 0; j < r; j++) {
            uint32_t c = src[ti + (j < w ? j : (w - 1))];
            val_a += (c >> 24) & 0xFF; val_r += (c >> 16) & 0xFF;
            val_g += (c >> 8)  & 0xFF; val_b += c         & 0xFF;
        }
        for (int j = 0; j <= r && j < w; j++) {
            uint32_t c = (ri < (y + 1) * stride && (ri - ti) < w) ? src[ri++] : lv;
            val_a += ((c >> 24) & 0xFF) - ((fv >> 24) & 0xFF);
            val_r += ((c >> 16) & 0xFF) - ((fv >> 16) & 0xFF);
            val_g += ((c >> 8)  & 0xFF) - ((fv >> 8)  & 0xFF);
            val_b += (c         & 0xFF) - (fv         & 0xFF);
            dst[ti++] = (((val_a / div) & 0xFF) << 24) | (((val_r / div) & 0xFF) << 16) |
                        (((val_g / div) & 0xFF) << 8)  | ((val_b / div) & 0xFF);
        }
        for (int j = r + 1; j < w - r; j++) {
            uint32_t c1 = src[ri++], c2 = src[li++];
            val_a += ((c1 >> 24) & 0xFF) - ((c2 >> 24) & 0xFF);
            val_r += ((c1 >> 16) & 0xFF) - ((c2 >> 16) & 0xFF);
            val_g += ((c1 >> 8)  & 0xFF) - ((c2 >> 8)  & 0xFF);
            val_b += (c1         & 0xFF) - (c2         & 0xFF);
            dst[ti++] = (((val_a / div) & 0xFF) << 24) | (((val_r / div) & 0xFF) << 16) |
                        (((val_g / div) & 0xFF) << 8)  | ((val_b / div) & 0xFF);
        }
        for (int j = w - r; j < w; j++) {
            uint32_t c = (li < (y + 1) * stride) ? src[li++] : lv;
            val_a += ((lv >> 24) & 0xFF) - ((c >> 24) & 0xFF);
            val_r += ((lv >> 16) & 0xFF) - ((c >> 16) & 0xFF);
            val_g += ((lv >> 8)  & 0xFF) - ((c >> 8)  & 0xFF);
            val_b += (lv         & 0xFF) - (c         & 0xFF);
            dst[ti++] = (((val_a / div) & 0xFF) << 24) | (((val_r / div) & 0xFF) << 16) |
                        (((val_g / div) & 0xFF) << 8)  | ((val_b / div) & 0xFF);
        }
    }
}

static inline void gfx_apply_frosted_glass(gfx_surface_t *surf, int x, int y, int w, int h,
                                           uint32_t tint_color, int radius)
{
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;
    int x1 = x < surf->clip.x ? surf->clip.x : x;
    int y1 = y < surf->clip.y ? surf->clip.y : y;
    int x2 = (x + w) > (surf->clip.x + surf->clip.w) ? (surf->clip.x + surf->clip.w) : (x + w);
    int y2 = (y + h) > (surf->clip.y + surf->clip.h) ? (surf->clip.y + surf->clip.h) : (y + h);
    if (x1 >= x2 || y1 >= y2) return;

    /* Blend subtle acrylic tint with slight contrast boost */
    int stride = surf->stride;
    int count = x2 - x1;
    for (int py = y1; py < y2; py++) {
        gfx_blend_tint_span_fast(&surf->pixels[py * stride + x1], tint_color, count);
    }
}

/* ── Anti-Aliased Circle and Smooth Rounded Corners ───────────────────────── */

static inline void gfx_draw_circle_aa(gfx_surface_t *surf, int cx, int cy, int radius, uint32_t color)
{
    if (radius <= 0) return;
    uint8_t a = (color >> 24) & 0xFF;
    for (int y = -radius - 1; y <= radius + 1; y++) {
        for (int x = -radius - 1; x <= radius + 1; x++) {
            int d2 = x * x + y * y;
            int r_in = (radius - 1) * (radius - 1);
            int r_out = (radius + 1) * (radius + 1);
            if (d2 <= r_in) {
                gfx_draw_pixel(surf, cx + x, cy + y, color);
            } else if (d2 < r_out) {
                int dist_approx = x * x + y * y;
                int alpha_scale = (r_out - dist_approx) * 255 / (r_out - r_in);
                if (alpha_scale < 0) alpha_scale = 0;
                if (alpha_scale > 255) alpha_scale = 255;
                uint8_t final_a = (uint8_t)((a * alpha_scale) / 255);
                uint32_t c = (color & 0x00FFFFFF) | ((uint32_t)final_a << 24);
                gfx_draw_pixel(surf, cx + x, cy + y, c);
            }
        }
    }
}

/* ── Radial and Diagonal Gradient Rendering ───────────────────────────────── */

static inline void gfx_draw_gradient_radial(gfx_surface_t *surf, int cx, int cy, int radius,
                                            uint32_t inner_col, uint32_t outer_col)
{
    if (!surf || !surf->pixels || radius <= 0) return;
    int x1 = cx - radius;
    int y1 = cy - radius;
    int x2 = cx + radius;
    int y2 = cy + radius;

    if (x1 < surf->clip.x) x1 = surf->clip.x;
    if (y1 < surf->clip.y) y1 = surf->clip.y;
    if (x2 >= surf->clip.x + surf->clip.w) x2 = surf->clip.x + surf->clip.w - 1;
    if (y2 >= surf->clip.y + surf->clip.h) y2 = surf->clip.y + surf->clip.h - 1;
    if (x1 > x2 || y1 > y2) return;

    int r2 = radius * radius;
    for (int y = y1; y <= y2; y++) {
        int dy = y - cy;
        int dy2 = dy * dy;
        for (int x = x1; x <= x2; x++) {
            int dx = x - cx;
            int dist2 = dx * dx + dy2;
            if (dist2 <= r2) {
                /* Integer square root calculation */
                int op = dist2;
                int res = 0;
                int one = 1 << 30;
                while (one > op) one >>= 2;
                while (one != 0) {
                    if (op >= res + one) {
                        op -= res + one;
                        res = (res >> 1) + one;
                    } else {
                        res >>= 1;
                    }
                    one >>= 2;
                }
                int dist = res;
                if (dist > radius) dist = radius;
                uint32_t c = gfx_lerp_color(inner_col, outer_col, dist, radius);
                gfx_draw_pixel(surf, x, y, c);
            }
        }
    }
}

static inline void gfx_draw_gradient_diagonal(gfx_surface_t *surf, int x, int y, int w, int h,
                                              uint32_t top_left, uint32_t bottom_right)
{
    if (!surf || !surf->pixels || w <= 0 || h <= 0) return;
    int x1 = x < surf->clip.x ? surf->clip.x : x;
    int y1 = y < surf->clip.y ? surf->clip.y : y;
    int x2 = (x + w) > (surf->clip.x + surf->clip.w) ? (surf->clip.x + surf->clip.w) : (x + w);
    int y2 = (y + h) > (surf->clip.y + surf->clip.h) ? (surf->clip.y + surf->clip.h) : (y + h);
    if (x1 >= x2 || y1 >= y2) return;

    int max_dist = (w - 1) + (h - 1);
    if (max_dist <= 0) max_dist = 1;

    for (int py = y1; py < y2; py++) {
        int dy = py - y;
        for (int px = x1; px < x2; px++) {
            int dx = px - x;
            int t = dx + dy;
            uint32_t c = gfx_lerp_color(top_left, bottom_right, t, max_dist);
            gfx_draw_pixel(surf, px, py, c);
        }
    }
}

/* ── Thick Outline and Ambient Glow Primitives ────────────────────────────── */

static inline void gfx_draw_rounded_rect_outline_thick(gfx_surface_t *surf, int x, int y, int w, int h,
                                                       int r, int thickness, uint32_t color)
{
    if (!surf || !surf->pixels || thickness <= 0 || w <= 0 || h <= 0) return;
    if (r <= 0) {
        gfx_draw_rect(surf, x, y, w, h, thickness, color);
        return;
    }
    for (int t = 0; t < thickness; t++) {
        int cur_w = w - 2 * t;
        int cur_h = h - 2 * t;
        int cur_r = r - t;
        if (cur_w <= 0 || cur_h <= 0) break;
        if (cur_r < 0) cur_r = 0;

        int rx = x + t;
        int ry = y + t;
        if (cur_r == 0) {
            gfx_draw_rect(surf, rx, ry, cur_w, cur_h, 1, color);
        } else {
            gfx_fill_rect(surf, rx + cur_r, ry, cur_w - 2 * cur_r, 1, color);
            gfx_fill_rect(surf, rx + cur_r, ry + cur_h - 1, cur_w - 2 * cur_r, 1, color);
            gfx_fill_rect(surf, rx, ry + cur_r, 1, cur_h - 2 * cur_r, color);
            gfx_fill_rect(surf, rx + cur_w - 1, ry + cur_r, 1, cur_h - 2 * cur_r, color);

            int r2 = cur_r * cur_r;
            int r_inner2 = (cur_r - 1) * (cur_r - 1);
            for (int dy = 0; dy < cur_r; dy++) {
                for (int dx = 0; dx < cur_r; dx++) {
                    int cx = cur_r - 1 - dx;
                    int cy = cur_r - 1 - dy;
                    int d2 = cx * cx + cy * cy;
                    if (d2 <= r2 && d2 >= r_inner2) {
                        gfx_draw_pixel(surf, rx + dx, ry + dy, color);
                        gfx_draw_pixel(surf, rx + cur_w - 1 - dx, ry + dy, color);
                        gfx_draw_pixel(surf, rx + dx, ry + cur_h - 1 - dy, color);
                        gfx_draw_pixel(surf, rx + cur_w - 1 - dx, ry + cur_h - 1 - dy, color);
                    }
                }
            }
        }
    }
}

static inline void gfx_draw_pill_glow(gfx_surface_t *surf, int x, int y, int w, int h,
                                      uint32_t glow_color, int blur_spread)
{
    if (!surf || !surf->pixels || blur_spread <= 0 || w <= 0 || h <= 0) return;
    uint8_t base_a = (glow_color >> 24) & 0xFF;
    if (base_a == 0) base_a = 255;
    uint32_t rgb = glow_color & 0x00FFFFFF;

    int r = h / 2;
    for (int i = 1; i <= blur_spread; i++) {
        int spread = i;
        uint8_t a = (uint8_t)((base_a * (blur_spread - i + 1)) / (blur_spread * 3));
        if (a == 0) continue;
        uint32_t col = rgb | ((uint32_t)a << 24);
        gfx_fill_rounded_rect(surf, x - spread, y - spread, w + 2 * spread, h + 2 * spread,
                              r + spread, col);
    }
}


