/* ============================================================================
 * AzamiOS Game Framework — Math Utilities
 * File: userland/libgame/include/game/game_math.h
 *
 * Header-only 2D game math library:
 *  • vec2_t with full arithmetic operations
 *  • AABB / rect overlap and containment tests
 *  • Fixed-point 16.16 math for deterministic physics
 *  • Fast integer square root
 *  • Sin/Cos lookup table (256 entries, Q15 format)
 *  • XorShift32 RNG
 *  • Utility: clamp, lerp, smoothstep, remap, min, max, abs
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ── Utility Macros ───────────────────────────────────────────────────────── */

#ifndef GAME_MIN
#define GAME_MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef GAME_MAX
#define GAME_MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif
#ifndef GAME_ABS
#define GAME_ABS(x) (((x) < 0) ? -(x) : (x))
#endif
#ifndef GAME_SWAP
#define GAME_SWAP(T, a, b) do { T _tmp = (a); (a) = (b); (b) = _tmp; } while(0)
#endif

/* ── Scalar Helpers ───────────────────────────────────────────────────────── */

static inline float game_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline int game_clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline float game_lerpf(float a, float b, float t)
{
    return a + (b - a) * t;
}

static inline int game_lerpi(int a, int b, int t256)
{
    return a + ((b - a) * t256) / 256;
}

static inline float game_smoothstep(float edge0, float edge1, float x)
{
    float t = game_clampf((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline float game_remap(float v, float in_lo, float in_hi, float out_lo, float out_hi)
{
    float t = (v - in_lo) / (in_hi - in_lo);
    return out_lo + t * (out_hi - out_lo);
}

/* ── Vec2 (float) ─────────────────────────────────────────────────────────── */

typedef struct {
    float x, y;
} vec2_t;

#define VEC2_ZERO   ((vec2_t){ 0.0f, 0.0f })
#define VEC2_ONE    ((vec2_t){ 1.0f, 1.0f })
#define VEC2_UP     ((vec2_t){ 0.0f, -1.0f })
#define VEC2_DOWN   ((vec2_t){ 0.0f,  1.0f })
#define VEC2_LEFT   ((vec2_t){-1.0f,  0.0f })
#define VEC2_RIGHT  ((vec2_t){ 1.0f,  0.0f })

static inline vec2_t vec2(float x, float y)
{
    return (vec2_t){ x, y };
}

static inline vec2_t vec2_add(vec2_t a, vec2_t b)
{
    return (vec2_t){ a.x + b.x, a.y + b.y };
}

static inline vec2_t vec2_sub(vec2_t a, vec2_t b)
{
    return (vec2_t){ a.x - b.x, a.y - b.y };
}

static inline vec2_t vec2_scale(vec2_t v, float s)
{
    return (vec2_t){ v.x * s, v.y * s };
}

static inline vec2_t vec2_neg(vec2_t v)
{
    return (vec2_t){ -v.x, -v.y };
}

static inline float vec2_dot(vec2_t a, vec2_t b)
{
    return a.x * b.x + a.y * b.y;
}

static inline float vec2_cross(vec2_t a, vec2_t b)
{
    return a.x * b.y - a.y * b.x;
}

static inline float vec2_length_sq(vec2_t v)
{
    return v.x * v.x + v.y * v.y;
}

/* Fast inverse square root (Quake III style) */
static inline float game_inv_sqrt(float x)
{
    union { float f; uint32_t i; } conv;
    conv.f = x;
    conv.i = 0x5F3759DFu - (conv.i >> 1);
    conv.f *= (1.5f - (x * 0.5f * conv.f * conv.f));
    return conv.f;
}

static inline float vec2_length(vec2_t v)
{
    float sq = vec2_length_sq(v);
    if (sq < 0.00001f) return 0.0f;
    return sq * game_inv_sqrt(sq);  /* sq / sqrt(sq) = sqrt(sq) */
}

static inline vec2_t vec2_normalize(vec2_t v)
{
    float sq = vec2_length_sq(v);
    if (sq < 0.00001f) return VEC2_ZERO;
    float inv = game_inv_sqrt(sq);
    return (vec2_t){ v.x * inv, v.y * inv };
}

static inline float vec2_distance(vec2_t a, vec2_t b)
{
    return vec2_length(vec2_sub(b, a));
}

static inline float vec2_distance_sq(vec2_t a, vec2_t b)
{
    return vec2_length_sq(vec2_sub(b, a));
}

static inline vec2_t vec2_lerp(vec2_t a, vec2_t b, float t)
{
    return (vec2_t){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

static inline vec2_t vec2_reflect(vec2_t v, vec2_t normal)
{
    float d = 2.0f * vec2_dot(v, normal);
    return (vec2_t){ v.x - d * normal.x, v.y - d * normal.y };
}

static inline vec2_t vec2_perp(vec2_t v)
{
    return (vec2_t){ -v.y, v.x };
}

/* ── Vec2 (integer) ───────────────────────────────────────────────────────── */

typedef struct {
    int x, y;
} ivec2_t;

static inline ivec2_t ivec2(int x, int y)
{
    return (ivec2_t){ x, y };
}

static inline ivec2_t ivec2_add(ivec2_t a, ivec2_t b)
{
    return (ivec2_t){ a.x + b.x, a.y + b.y };
}

static inline ivec2_t ivec2_sub(ivec2_t a, ivec2_t b)
{
    return (ivec2_t){ a.x - b.x, a.y - b.y };
}

/* ── AABB ─────────────────────────────────────────────────────────────────── */

typedef struct {
    float x, y;     /* top-left corner */
    float w, h;     /* width, height */
} aabb_t;

static inline aabb_t aabb_make(float x, float y, float w, float h)
{
    return (aabb_t){ x, y, w, h };
}

static inline aabb_t aabb_from_center(float cx, float cy, float w, float h)
{
    return (aabb_t){ cx - w * 0.5f, cy - h * 0.5f, w, h };
}

static inline vec2_t aabb_center(aabb_t a)
{
    return (vec2_t){ a.x + a.w * 0.5f, a.y + a.h * 0.5f };
}

static inline bool aabb_overlaps(aabb_t a, aabb_t b)
{
    return a.x < b.x + b.w && a.x + a.w > b.x &&
           a.y < b.y + b.h && a.y + a.h > b.y;
}

static inline bool aabb_contains_point(aabb_t a, float px, float py)
{
    return px >= a.x && px < a.x + a.w && py >= a.y && py < a.y + a.h;
}

static inline bool aabb_contains(aabb_t outer, aabb_t inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w &&
           inner.y + inner.h <= outer.y + outer.h;
}

/* Minimum translation vector to separate two overlapping AABBs.
 * Returns VEC2_ZERO if they don't overlap. */
static inline vec2_t aabb_mtv(aabb_t a, aabb_t b)
{
    float dx1 = (b.x + b.w) - a.x;
    float dx2 = (a.x + a.w) - b.x;
    float dy1 = (b.y + b.h) - a.y;
    float dy2 = (a.y + a.h) - b.y;

    if (dx1 <= 0 || dx2 <= 0 || dy1 <= 0 || dy2 <= 0)
        return VEC2_ZERO;

    float min_x = (dx1 < dx2) ? -dx1 : dx2;
    float min_y = (dy1 < dy2) ? -dy1 : dy2;

    if (GAME_ABS(min_x) < GAME_ABS(min_y))
        return vec2(min_x, 0);
    else
        return vec2(0, min_y);
}

/* ── Circle ───────────────────────────────────────────────────────────────── */

typedef struct {
    float cx, cy, r;
} circle_t;

static inline bool circle_overlaps(circle_t a, circle_t b)
{
    float dx = b.cx - a.cx;
    float dy = b.cy - a.cy;
    float rsum = a.r + b.r;
    return (dx * dx + dy * dy) < (rsum * rsum);
}

static inline bool circle_contains_point(circle_t c, float px, float py)
{
    float dx = px - c.cx;
    float dy = py - c.cy;
    return (dx * dx + dy * dy) <= (c.r * c.r);
}

static inline bool aabb_circle_overlap(aabb_t a, circle_t c)
{
    float nearest_x = game_clampf(c.cx, a.x, a.x + a.w);
    float nearest_y = game_clampf(c.cy, a.y, a.y + a.h);
    float dx = c.cx - nearest_x;
    float dy = c.cy - nearest_y;
    return (dx * dx + dy * dy) < (c.r * c.r);
}

/* ── Fixed-Point 16.16 ────────────────────────────────────────────────────── */

typedef int32_t fixed16_t;

#define FP_SHIFT  16
#define FP_ONE    (1 << FP_SHIFT)
#define FP_HALF   (FP_ONE >> 1)

static inline fixed16_t fp_from_int(int v) { return v << FP_SHIFT; }
static inline int       fp_to_int(fixed16_t v) { return v >> FP_SHIFT; }
static inline fixed16_t fp_from_float(float v) { return (fixed16_t)(v * FP_ONE); }
static inline float     fp_to_float(fixed16_t v) { return (float)v / FP_ONE; }

static inline fixed16_t fp_mul(fixed16_t a, fixed16_t b)
{
    return (fixed16_t)(((int64_t)a * b) >> FP_SHIFT);
}

static inline fixed16_t fp_div(fixed16_t a, fixed16_t b)
{
    if (b == 0) return 0;
    return (fixed16_t)(((int64_t)a << FP_SHIFT) / b);
}

/* ── Fast Integer Square Root ─────────────────────────────────────────────── */

static inline unsigned int game_isqrt(unsigned int n)
{
    if (n == 0) return 0;
    unsigned int x = n;
    unsigned int y = (x + 1) >> 1;
    while (y < x) {
        x = y;
        y = (x + n / x) >> 1;
    }
    return x;
}

/* ── Sin / Cos Lookup Table (256 entries, Q15) ────────────────────────────── */
/* sin_lut[i] = sin(i * 2π / 256) * 32767
 * Index 0..63 = 0°..90°, full period = 256 entries */

#define GAME_SIN_LUT_SIZE 256
#define GAME_SIN_Q15_ONE  32767

/* Precomputed first quadrant; remaining quadrants by symmetry */
static const int16_t game_sin_q1[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

/* Returns sin(angle) in Q15, where angle is 0..255 (= 0..360°) */
static inline int game_sin_lut(int angle)
{
    angle &= 255;
    if (angle < 64)       return  game_sin_q1[angle];
    else if (angle < 128) return  game_sin_q1[128 - angle];
    else if (angle < 192) return -game_sin_q1[angle - 128];
    else                  return -game_sin_q1[256 - angle];
}

/* Returns cos(angle) in Q15, where angle is 0..255 (= 0..360°) */
static inline int game_cos_lut(int angle)
{
    return game_sin_lut(angle + 64);
}

/* ── XorShift32 Random Number Generator ───────────────────────────────────── */

typedef struct {
    uint32_t state;
} game_rng_t;

static inline void game_rng_seed(game_rng_t *rng, uint32_t seed)
{
    rng->state = seed ? seed : 0x12345678u;
}

/* Seed from hardware entropy (/dev/hwrng or /dev/urandom) */
static inline void game_rng_seed_hw(game_rng_t *rng)
{
    uint32_t seed = 0x54A8E001u;
    int fd = -1;
    /* Try hardware RNG first, then urandom — same pattern as existing games */
    extern int sys_open(const char *, int, int);
    extern long sys_read(int, void *, unsigned long);
    extern int sys_close(int);
    fd = sys_open("/dev/hwrng", 0, 0);
    if (fd >= 0) {
        sys_read(fd, &seed, sizeof(seed));
        sys_close(fd);
    } else {
        fd = sys_open("/dev/urandom", 0, 0);
        if (fd >= 0) {
            sys_read(fd, &seed, sizeof(seed));
            sys_close(fd);
        }
    }
    game_rng_seed(rng, seed);
}

static inline uint32_t game_rng_next(game_rng_t *rng)
{
    uint32_t x = rng->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng->state = x;
    return x;
}

/* Random integer in [0, max) */
static inline int game_rng_range(game_rng_t *rng, int max)
{
    if (max <= 0) return 0;
    return (int)(game_rng_next(rng) % (uint32_t)max);
}

/* Random integer in [lo, hi] inclusive */
static inline int game_rng_range_ii(game_rng_t *rng, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + game_rng_range(rng, hi - lo + 1);
}

/* Random float in [0.0, 1.0) */
static inline float game_rng_float(game_rng_t *rng)
{
    return (float)(game_rng_next(rng) & 0x00FFFFFFu) / (float)0x01000000u;
}

/* Random float in [lo, hi) */
static inline float game_rng_float_range(game_rng_t *rng, float lo, float hi)
{
    return lo + game_rng_float(rng) * (hi - lo);
}
