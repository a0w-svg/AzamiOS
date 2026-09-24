/* ============================================================================
 * AzamiOS Game Framework — Core Game Loop & Bootstrap
 * File: userland/libgame/include/game/game_core.h
 *
 * Provides:
 *  • Window bootstrap with azwm/ui_kit connection and centered placement
 *  • Autonomous timer tick setup via az_set_timer()
 *  • IPC message pump (resize, key, mouse, timer, close events)
 *  • Subsystem coordination: Input, ECS, Physics, Audio, Assets, Scenes, Render
 *  • Fixed timestep / delta-time tracking
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#if __has_include(<ui_kit.h>)
#include <ui_kit.h>
#elif __has_include("ui_kit.h")
#include "ui_kit.h"
#elif __has_include("../apps/shared/ui_kit.h")
#include "../apps/shared/ui_kit.h"
#elif __has_include("../../apps/shared/ui_kit.h")
#include "../../apps/shared/ui_kit.h"
#endif

#include "game_math.h"
#include "game_input.h"
#include "game_ecs.h"
#include "game_physics.h"
#include "game_render.h"
#include "game_audio.h"
#include "game_asset.h"
#include "game_scene.h"

#ifndef GAME_DEFAULT_FPS
#define GAME_DEFAULT_FPS 60
#endif

#ifndef GAME_DEFAULT_MAP_ADDR
#define GAME_DEFAULT_MAP_ADDR ((void *)0x77000000)
#endif

#ifndef GAME_DEFAULT_SERVER_CHAN
#define GAME_DEFAULT_SERVER_CHAN 1
#endif

/* Forward declaration */
struct game_ctx;
typedef struct game_ctx game_t;

/* ── Game Configuration ───────────────────────────────────────────────────── */

typedef struct {
    const char *title;
    int         width;
    int         height;
    int         target_fps;     /* e.g. 60. 0 = default (60) */
    void       *map_addr;       /* shmem address, NULL = default 0x77000000 */
    int         server_chan;    /* compositor channel, 0 = default 1 */
    bool        enable_audio;   /* initialize audio subsystem (/dev/dsp) */
    float       gravity_x;      /* physics world gravity X (default 0) */
    float       gravity_y;      /* physics world gravity Y (default 980.0f) */
    void       *user_data;      /* arbitrary user state pointer */
} game_config_t;

/* ── Game Callbacks ───────────────────────────────────────────────────────── */

typedef struct {
    void (*on_init)(game_t *g);
    void (*on_update)(game_t *g, float dt);
    void (*on_render)(game_t *g);
    void (*on_key)(game_t *g, int key, bool pressed, int mods);
    void (*on_mouse)(game_t *g, int x, int y, int btn, bool pressed);
    void (*on_resize)(game_t *g, int width, int height);
    void (*on_destroy)(game_t *g);
} game_callbacks_t;

/* ── Game Context ─────────────────────────────────────────────────────────── */

struct game_ctx {
    uk_window_t       win;
    input_state_t     input;
    ecs_world_t       ecs;
    phys_world_t      phys;
    audio_ctx_t       audio;
    asset_mgr_t       assets;
    scene_mgr_t       scenes;
    camera_t          camera;

    /* Timing */
    float             dt;
    float             total_time;
    uint32_t          frame_count;
    int               target_fps;
    int               timer_interval_ms;

    /* Loop state */
    bool              running;
    bool              paused;
    void             *user_data;

    /* Callbacks */
    game_callbacks_t  callbacks;
};

/* ── Lifecycle & Runtime API (implemented in game_core.c) ─────────────────── */

/* Initialize a game context, create window, start timer and setup subsystems. */
int  game_init(game_t *g, const game_config_t *cfg, const game_callbacks_t *cb);

/* Convenience: bootstrap and run entire game loop to completion. */
int  game_run(game_config_t *cfg, game_callbacks_t *cb);

/* Execute a single frame step (input snapshot, update, physics, render, flush). */
void game_step(game_t *g);

/* Terminate the game loop cleanly. */
void game_quit(game_t *g);

/* Change target framerate at runtime. */
void game_set_fps(game_t *g, int fps);

/* Clear the window backbuffer to a solid ARGB color. */
void game_clear(game_t *g, uint32_t color);

/* Draw a full-window alpha blend overlay (used for screen fades). */
void game_draw_overlay(game_t *g, uint32_t color, uint8_t alpha);

/* ── Convenience Rendering Helpers ────────────────────────────────────────── */

static inline void game_draw_sprite(game_t *g, const sprite_t *spr, int x, int y)
{
    if (g && g->win.pixels) {
        gfx_draw_sprite_raw((uint32_t *)g->win.pixels, (int)g->win.width, (int)g->win.height, spr, x, y);
    }
}

static inline void game_draw_particles(game_t *g, particle_sys_t *ps)
{
    if (g && g->win.pixels) {
        particles_draw(ps, (uint32_t *)g->win.pixels, (int)g->win.width, (int)g->win.height, &g->camera);
    }
}
