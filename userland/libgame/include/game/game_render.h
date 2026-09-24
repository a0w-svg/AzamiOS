/* ============================================================================
 * AzamiOS Game Framework — Rendering (Sprites, Tilemap, Camera, Particles)
 * File: userland/libgame/include/game/game_render.h
 *
 * Header-only 2D game renderer built on top of ui_kit.h primitives:
 *  • Sprite blitting with source rect (atlas), flip, alpha
 *  • Frame-based sprite animation (loop / once / ping-pong)
 *  • Tilemap rendering from tile atlas with camera scrolling
 *  • 2D camera with position, zoom, viewport, screen shake
 *  • Particle system with pooled particles, color fade, gravity
 *
 * All drawing goes to the uk_window_t pixel buffer; the game loop calls
 * uk_invalidate() after rendering is complete.
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "game_math.h"

/* Forward-declare uk_window_t so this header doesn't force ui_kit.h order —
 * the actual uk_window_t is defined in ui_kit.h, which is always included
 * before this header through game.h or the game's own includes. */
struct uk_window_tag;

/* ── Sprite ───────────────────────────────────────────────────────────────── */

/* A sprite is a reference to a region within an ARGB32 pixel buffer. */
typedef struct {
    uint32_t *pixels;    /* source pixel data (ARGB32) */
    int       tex_w;     /* full texture width (stride) */
    int       tex_h;     /* full texture height */
    int       src_x;     /* source rectangle top-left X */
    int       src_y;     /* source rectangle top-left Y */
    int       src_w;     /* source rectangle width */
    int       src_h;     /* source rectangle height */
    bool      flip_h;    /* horizontal flip */
    bool      flip_v;    /* vertical flip */
    uint8_t   alpha;     /* global alpha 0..255 */
} sprite_t;

static inline sprite_t sprite_make(uint32_t *pixels, int tex_w, int tex_h)
{
    sprite_t s;
    memset(&s, 0, sizeof(s));
    s.pixels = pixels;
    s.tex_w  = tex_w;
    s.tex_h  = tex_h;
    s.src_x  = 0;
    s.src_y  = 0;
    s.src_w  = tex_w;
    s.src_h  = tex_h;
    s.alpha  = 255;
    return s;
}

static inline sprite_t sprite_sub(sprite_t *atlas, int sx, int sy, int sw, int sh)
{
    sprite_t s = *atlas;
    s.src_x = sx;
    s.src_y = sy;
    s.src_w = sw;
    s.src_h = sh;
    return s;
}

/* Blit sprite to the window's pixel buffer at (dx, dy), with clipping. */
static inline void gfx_draw_sprite_raw(uint32_t *dst, int dst_w, int dst_h,
                                        const sprite_t *spr, int dx, int dy)
{
    if (!spr->pixels || !dst || spr->src_w <= 0 || spr->src_h <= 0) return;

    int sw = spr->src_w, sh = spr->src_h;

    for (int row = 0; row < sh; row++) {
        int dst_y = dy + row;
        if (dst_y < 0 || dst_y >= dst_h) continue;

        int src_row = spr->flip_v ? (sh - 1 - row) : row;

        for (int col = 0; col < sw; col++) {
            int dst_x = dx + col;
            if (dst_x < 0 || dst_x >= dst_w) continue;

            int src_col = spr->flip_h ? (sw - 1 - col) : col;
            int sx = spr->src_x + src_col;
            int sy = spr->src_y + src_row;

            if (sx < 0 || sx >= spr->tex_w || sy < 0 || sy >= spr->tex_h) continue;

            uint32_t src_px = spr->pixels[sy * spr->tex_w + sx];
            uint32_t sa = (src_px >> 24) & 0xFF;

            if (spr->alpha < 255)
                sa = (sa * spr->alpha) / 255;

            if (sa == 0) continue;

            if (sa >= 255) {
                dst[dst_y * dst_w + dst_x] = src_px | 0xFF000000u;
            } else {
                /* Alpha blend */
                uint32_t d = dst[dst_y * dst_w + dst_x];
                uint32_t inv_a = 255 - sa;
                uint32_t rb = (((src_px & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_a) >> 8) & 0x00FF00FFu;
                uint32_t g  = (((src_px & 0x0000FF00u) * sa + (d & 0x0000FF00u) * inv_a) >> 8) & 0x0000FF00u;
                dst[dst_y * dst_w + dst_x] = 0xFF000000u | rb | g;
            }
        }
    }
}

/* ── Sprite Animation ─────────────────────────────────────────────────────── */

typedef enum {
    ANIM_LOOP,
    ANIM_ONCE,
    ANIM_PINGPONG
} anim_mode_t;

typedef struct {
    sprite_t  base;         /* base sprite (atlas) */
    int       frame_w;      /* width of a single frame */
    int       frame_h;      /* height of a single frame */
    int       frame_count;  /* total frames */
    int       cols;         /* frames per row in the atlas */
    float     fps;          /* animation speed */
    anim_mode_t mode;

    /* Runtime state */
    float     timer;
    int       current_frame;
    int       direction;    /* +1 or -1 for ping-pong */
    bool      finished;
} sprite_anim_t;

static inline sprite_anim_t anim_create(sprite_t atlas, int frame_w, int frame_h,
                                          int frame_count, float fps, anim_mode_t mode)
{
    sprite_anim_t a;
    memset(&a, 0, sizeof(a));
    a.base = atlas;
    a.frame_w = frame_w;
    a.frame_h = frame_h;
    a.frame_count = (frame_count > 0) ? frame_count : 1;
    a.cols = (atlas.tex_w > 0 && frame_w > 0) ? (atlas.tex_w / frame_w) : 1;
    if (a.cols <= 0) a.cols = 1;
    a.fps = (fps > 0) ? fps : 10.0f;
    a.mode = mode;
    a.direction = 1;
    return a;
}

static inline void anim_update(sprite_anim_t *a, float dt)
{
    if (a->finished || a->frame_count <= 1) return;

    a->timer += dt;
    float frame_dur = 1.0f / a->fps;

    while (a->timer >= frame_dur) {
        a->timer -= frame_dur;
        a->current_frame += a->direction;

        switch (a->mode) {
        case ANIM_LOOP:
            if (a->current_frame >= a->frame_count)
                a->current_frame = 0;
            break;
        case ANIM_ONCE:
            if (a->current_frame >= a->frame_count) {
                a->current_frame = a->frame_count - 1;
                a->finished = true;
            }
            break;
        case ANIM_PINGPONG:
            if (a->current_frame >= a->frame_count) {
                a->current_frame = a->frame_count - 2;
                a->direction = -1;
            } else if (a->current_frame < 0) {
                a->current_frame = 1;
                a->direction = 1;
            }
            break;
        }
    }
}

static inline void anim_reset(sprite_anim_t *a)
{
    a->current_frame = 0;
    a->timer = 0;
    a->direction = 1;
    a->finished = false;
}

static inline sprite_t anim_get_frame(const sprite_anim_t *a)
{
    sprite_t s = a->base;
    int frame = a->current_frame;
    if (frame < 0) frame = 0;
    if (frame >= a->frame_count) frame = a->frame_count - 1;

    int col = frame % a->cols;
    int row = frame / a->cols;
    s.src_x = col * a->frame_w;
    s.src_y = row * a->frame_h;
    s.src_w = a->frame_w;
    s.src_h = a->frame_h;
    return s;
}

/* ── Camera ───────────────────────────────────────────────────────────────── */

typedef struct {
    float x, y;              /* camera position (top-left of viewport) */
    float target_x, target_y;/* smooth follow target */
    float follow_speed;      /* 0 = instant, 1 = smooth (per-frame lerp factor) */
    int   viewport_w;        /* viewport pixel width */
    int   viewport_h;        /* viewport pixel height */

    /* Screen shake */
    float shake_intensity;
    float shake_duration;
    float shake_timer;
    int   shake_offset_x;
    int   shake_offset_y;

    game_rng_t shake_rng;
} camera_t;

static inline camera_t camera_create(int vw, int vh)
{
    camera_t cam;
    memset(&cam, 0, sizeof(cam));
    cam.viewport_w = vw;
    cam.viewport_h = vh;
    cam.follow_speed = 0.1f;
    game_rng_seed(&cam.shake_rng, 0xCAFE1234u);
    return cam;
}

static inline void camera_init(camera_t *cam, int vw, int vh)
{
    if (cam) *cam = camera_create(vw, vh);
}

static inline void camera_set_position(camera_t *cam, float x, float y)
{
    cam->x = x;
    cam->y = y;
    cam->target_x = x;
    cam->target_y = y;
}

static inline void camera_follow(camera_t *cam, float target_x, float target_y)
{
    cam->target_x = target_x - cam->viewport_w * 0.5f;
    cam->target_y = target_y - cam->viewport_h * 0.5f;
}

static inline void camera_shake(camera_t *cam, float intensity, float duration)
{
    cam->shake_intensity = intensity;
    cam->shake_duration = duration;
    cam->shake_timer = duration;
}

static inline void camera_update(camera_t *cam, float dt)
{
    /* Smooth follow */
    if (cam->follow_speed > 0.0f) {
        float t = game_clampf(cam->follow_speed * dt * 60.0f, 0.0f, 1.0f);
        cam->x = game_lerpf(cam->x, cam->target_x, t);
        cam->y = game_lerpf(cam->y, cam->target_y, t);
    } else {
        cam->x = cam->target_x;
        cam->y = cam->target_y;
    }

    /* Screen shake */
    if (cam->shake_timer > 0) {
        cam->shake_timer -= dt;
        float t = cam->shake_timer / cam->shake_duration;
        float intensity = cam->shake_intensity * t;
        cam->shake_offset_x = game_rng_range_ii(&cam->shake_rng,
                                                 (int)(-intensity), (int)intensity);
        cam->shake_offset_y = game_rng_range_ii(&cam->shake_rng,
                                                 (int)(-intensity), (int)intensity);
    } else {
        cam->shake_offset_x = 0;
        cam->shake_offset_y = 0;
    }
}

/* Convert world coords to screen coords */
static inline int camera_world_to_screen_x(const camera_t *cam, float wx)
{
    return (int)(wx - cam->x) + cam->shake_offset_x;
}

static inline int camera_world_to_screen_y(const camera_t *cam, float wy)
{
    return (int)(wy - cam->y) + cam->shake_offset_y;
}

/* ── Tilemap ──────────────────────────────────────────────────────────────── */

#ifndef TILEMAP_MAX_LAYERS
#define TILEMAP_MAX_LAYERS 4
#endif

typedef struct {
    int      *tiles;         /* 2D array of tile indices (row-major), 0 = empty */
    int       map_w;         /* map width in tiles */
    int       map_h;         /* map height in tiles */
    int       tile_w;        /* tile pixel width */
    int       tile_h;        /* tile pixel height */

    /* Tile atlas */
    uint32_t *atlas_pixels;  /* atlas sprite sheet */
    int       atlas_w;       /* atlas pixel width */
    int       atlas_h;       /* atlas pixel height */
    int       atlas_cols;    /* tiles per row in atlas */
} tilemap_layer_t;

typedef struct {
    tilemap_layer_t layers[TILEMAP_MAX_LAYERS];
    int             layer_count;
} tilemap_t;

static inline void tilemap_init(tilemap_t *tm)
{
    memset(tm, 0, sizeof(*tm));
}

static inline int tilemap_add_layer(tilemap_t *tm, int *tiles, int map_w, int map_h,
                                      int tile_w, int tile_h,
                                      uint32_t *atlas, int atlas_w, int atlas_h)
{
    if (tm->layer_count >= TILEMAP_MAX_LAYERS) return -1;
    int idx = tm->layer_count++;
    tilemap_layer_t *l = &tm->layers[idx];
    l->tiles = tiles;
    l->map_w = map_w;
    l->map_h = map_h;
    l->tile_w = tile_w;
    l->tile_h = tile_h;
    l->atlas_pixels = atlas;
    l->atlas_w = atlas_w;
    l->atlas_h = atlas_h;
    l->atlas_cols = (atlas_w > 0 && tile_w > 0) ? (atlas_w / tile_w) : 1;
    return idx;
}

static inline void gfx_draw_tilemap_layer(uint32_t *dst, int dst_w, int dst_h,
                                            const tilemap_layer_t *l,
                                            const camera_t *cam)
{
    if (!l->tiles || !l->atlas_pixels || !dst) return;

    int tw = l->tile_w, th = l->tile_h;
    if (tw <= 0 || th <= 0) return;

    /* Compute visible tile range */
    int cam_x = (int)cam->x - cam->shake_offset_x;
    int cam_y = (int)cam->y - cam->shake_offset_y;

    int start_col = cam_x / tw;
    int start_row = cam_y / th;
    int end_col = (cam_x + cam->viewport_w) / tw + 1;
    int end_row = (cam_y + cam->viewport_h) / th + 1;

    if (start_col < 0) start_col = 0;
    if (start_row < 0) start_row = 0;
    if (end_col > l->map_w) end_col = l->map_w;
    if (end_row > l->map_h) end_row = l->map_h;

    for (int row = start_row; row < end_row; row++) {
        for (int col = start_col; col < end_col; col++) {
            int tile_id = l->tiles[row * l->map_w + col];
            if (tile_id <= 0) continue;  /* 0 = empty tile */
            tile_id--;  /* 1-indexed to 0-indexed */

            int atlas_col = tile_id % l->atlas_cols;
            int atlas_row = tile_id / l->atlas_cols;

            int sx = atlas_col * tw;
            int sy = atlas_row * th;

            int screen_x = col * tw - cam_x + cam->shake_offset_x;
            int screen_y = row * th - cam_y + cam->shake_offset_y;

            /* Blit tile */
            for (int py = 0; py < th; py++) {
                int dy = screen_y + py;
                if (dy < 0 || dy >= dst_h) continue;
                int src_sy = sy + py;
                if (src_sy >= l->atlas_h) continue;

                for (int px = 0; px < tw; px++) {
                    int dx = screen_x + px;
                    if (dx < 0 || dx >= dst_w) continue;
                    int src_sx = sx + px;
                    if (src_sx >= l->atlas_w) continue;

                    uint32_t spx = l->atlas_pixels[src_sy * l->atlas_w + src_sx];
                    uint32_t sa = (spx >> 24) & 0xFF;
                    if (sa == 0) continue;

                    if (sa >= 255) {
                        dst[dy * dst_w + dx] = spx | 0xFF000000u;
                    } else {
                        uint32_t d = dst[dy * dst_w + dx];
                        uint32_t inv_a = 255 - sa;
                        uint32_t rb = (((spx & 0x00FF00FFu) * sa + (d & 0x00FF00FFu) * inv_a) >> 8) & 0x00FF00FFu;
                        uint32_t g  = (((spx & 0x0000FF00u) * sa + (d & 0x0000FF00u) * inv_a) >> 8) & 0x0000FF00u;
                        dst[dy * dst_w + dx] = 0xFF000000u | rb | g;
                    }
                }
            }
        }
    }
}

/* ── Particle System ──────────────────────────────────────────────────────── */

#ifndef PARTICLE_POOL_SIZE
#define PARTICLE_POOL_SIZE 256
#endif

typedef struct {
    float    x, y;
    float    vx, vy;
    float    life;       /* remaining lifetime in seconds */
    float    max_life;   /* initial lifetime */
    uint32_t color;      /* ARGB color */
    uint32_t color_end;  /* fade-to color */
    float    size;       /* particle size (radius) */
    float    size_end;
    bool     active;
} particle_t;

typedef struct {
    particle_t pool[PARTICLE_POOL_SIZE];
    int        count;         /* active count (for stats) */
    float      gravity_x;
    float      gravity_y;
    float      damping;       /* velocity damping per second (0 = none, 1 = full stop) */
} particle_sys_t;

static inline void particles_init(particle_sys_t *ps)
{
    memset(ps, 0, sizeof(*ps));
    ps->gravity_y = 200.0f;  /* default downward gravity */
    ps->damping = 0.02f;
}

static inline void particles_emit(particle_sys_t *ps, float x, float y,
                                    float vx, float vy,
                                    float life, float size, float size_end,
                                    uint32_t color, uint32_t color_end)
{
    /* Find an inactive slot */
    for (int i = 0; i < PARTICLE_POOL_SIZE; i++) {
        if (!ps->pool[i].active) {
            particle_t *p = &ps->pool[i];
            p->x = x;
            p->y = y;
            p->vx = vx;
            p->vy = vy;
            p->life = life;
            p->max_life = life;
            p->size = size;
            p->size_end = size_end;
            p->color = color;
            p->color_end = color_end;
            p->active = true;
            ps->count++;
            return;
        }
    }
}

/* Emit a burst of particles with randomized velocities */
static inline void particles_emit_burst(particle_sys_t *ps, game_rng_t *rng,
                                          float x, float y, int count,
                                          float speed, float life,
                                          float size, uint32_t color)
{
    for (int i = 0; i < count; i++) {
        int angle = game_rng_range(rng, 256);
        float s = speed * (0.5f + game_rng_float(rng) * 0.5f);
        float vx = (float)game_sin_lut(angle + 64) / 32767.0f * s;
        float vy = (float)game_sin_lut(angle) / 32767.0f * s;
        float l = life * (0.7f + game_rng_float(rng) * 0.6f);
        particles_emit(ps, x, y, vx, vy, l, size, 0.0f, color, color & 0x00FFFFFFu);
    }
}

static inline uint32_t particle_lerp_color(uint32_t a, uint32_t b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int it = (int)(t * 255.0f);
    int inv = 255 - it;
    uint32_t rb = (((a & 0x00FF00FFu) * (uint32_t)inv + (b & 0x00FF00FFu) * (uint32_t)it) >> 8) & 0x00FF00FFu;
    uint32_t ag = (((a & 0xFF00FF00u) >> 8) * (uint32_t)inv + ((b & 0xFF00FF00u) >> 8) * (uint32_t)it) & 0xFF00FF00u;
    return rb | ag;
}

static inline void particles_update(particle_sys_t *ps, float dt)
{
    int active = 0;
    for (int i = 0; i < PARTICLE_POOL_SIZE; i++) {
        particle_t *p = &ps->pool[i];
        if (!p->active) continue;

        p->life -= dt;
        if (p->life <= 0) {
            p->active = false;
            continue;
        }

        p->vx += ps->gravity_x * dt;
        p->vy += ps->gravity_y * dt;

        if (ps->damping > 0) {
            float d = 1.0f - ps->damping * dt;
            if (d < 0) d = 0;
            p->vx *= d;
            p->vy *= d;
        }

        p->x += p->vx * dt;
        p->y += p->vy * dt;
        active++;
    }
    ps->count = active;
}

static inline void particles_draw(particle_sys_t *ps, uint32_t *dst, int dst_w, int dst_h,
                                    const camera_t *cam)
{
    for (int i = 0; i < PARTICLE_POOL_SIZE; i++) {
        particle_t *p = &ps->pool[i];
        if (!p->active) continue;

        float t = 1.0f - (p->life / p->max_life); /* 0 at birth, 1 at death */
        float size = game_lerpf(p->size, p->size_end, t);
        uint32_t color = particle_lerp_color(p->color, p->color_end, t);

        /* Alpha fade out in the last 30% of life */
        uint32_t alpha = (color >> 24) & 0xFF;
        if (t > 0.7f) {
            alpha = (uint32_t)((float)alpha * (1.0f - (t - 0.7f) / 0.3f));
        }

        int sx, sy;
        if (cam) {
            sx = camera_world_to_screen_x(cam, p->x);
            sy = camera_world_to_screen_y(cam, p->y);
        } else {
            sx = (int)p->x;
            sy = (int)p->y;
        }

        int r = (int)(size + 0.5f);
        if (r < 1) r = 1;

        /* Draw as filled circle using scanlines */
        int r2 = r * r;
        for (int dy = -r; dy <= r; dy++) {
            int py = sy + dy;
            if (py < 0 || py >= dst_h) continue;
            int rem = r2 - dy * dy;
            if (rem < 0) continue;
            int max_x = 0;
            while ((max_x + 1) * (max_x + 1) <= rem) max_x++;

            for (int dx = -max_x; dx <= max_x; dx++) {
                int px = sx + dx;
                if (px < 0 || px >= dst_w) continue;

                if (alpha >= 255) {
                    dst[py * dst_w + px] = color | 0xFF000000u;
                } else if (alpha > 0) {
                    uint32_t d = dst[py * dst_w + px];
                    uint32_t inv_a = 255 - alpha;
                    uint32_t rb = (((color & 0x00FF00FFu) * alpha + (d & 0x00FF00FFu) * inv_a) >> 8) & 0x00FF00FFu;
                    uint32_t g  = (((color & 0x0000FF00u) * alpha + (d & 0x0000FF00u) * inv_a) >> 8) & 0x0000FF00u;
                    dst[py * dst_w + px] = 0xFF000000u | rb | g;
                }
            }
        }
    }
}

/* Clear all particles */
static inline void particles_clear(particle_sys_t *ps)
{
    for (int i = 0; i < PARTICLE_POOL_SIZE; i++)
        ps->pool[i].active = false;
    ps->count = 0;
}
