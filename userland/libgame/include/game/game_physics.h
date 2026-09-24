/* ============================================================================
 * AzamiOS Game Framework — 2D Physics Engine
 * File: userland/libgame/include/game/game_physics.h
 *
 * 2D rigid-body physics with:
 *  • Velocity/acceleration integration (semi-implicit Euler)
 *  • AABB and circle collider shapes
 *  • Broadphase: spatial hash grid
 *  • Narrowphase: AABB-AABB, circle-circle, AABB-circle overlap tests
 *  • Collision response with separation and bounce (restitution)
 *  • Gravity vector
 *  • Collision callbacks: on_enter, on_stay, on_exit
 *  • Static vs dynamic bodies (mass == 0 → static)
 *
 * Tuning defines:
 *   PHYS_HASH_SIZE       — spatial hash buckets (default 128)
 *   PHYS_HASH_CELL_SIZE  — cell size in pixels  (default 64)
 *   PHYS_MAX_PAIRS       — max collision pairs per frame (default 256)
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "game_math.h"
#include "game_ecs.h"

#ifndef PHYS_HASH_SIZE
#define PHYS_HASH_SIZE      128
#endif
#ifndef PHYS_HASH_CELL_SIZE
#define PHYS_HASH_CELL_SIZE 64
#endif
#ifndef PHYS_MAX_PAIRS
#define PHYS_MAX_PAIRS      256
#endif

/* ── Collision Pair ───────────────────────────────────────────────────────── */

typedef struct {
    ecs_entity_t a;
    ecs_entity_t b;
    vec2_t       normal;   /* collision normal (a → b) */
    float        depth;    /* penetration depth */
} phys_collision_t;

/* Collision callback type */
typedef void (*phys_collision_fn)(const phys_collision_t *collision, void *userdata);

/* ── Spatial Hash Bucket ──────────────────────────────────────────────────── */

#define PHYS_BUCKET_CAP 16

typedef struct {
    ecs_entity_t entities[PHYS_BUCKET_CAP];
    int count;
} phys_bucket_t;

/* ── Physics World ────────────────────────────────────────────────────────── */

typedef struct {
    float gravity_x, gravity_y;

    /* Spatial hash for broadphase */
    phys_bucket_t hash[PHYS_HASH_SIZE];

    /* Collision pairs this frame */
    phys_collision_t pairs[PHYS_MAX_PAIRS];
    int pair_count;

    /* Previous frame pairs (for enter/exit detection) */
    phys_collision_t prev_pairs[PHYS_MAX_PAIRS];
    int prev_pair_count;

    /* Callbacks */
    phys_collision_fn on_collision;
    void             *on_collision_data;
    phys_collision_fn on_trigger;
    void             *on_trigger_data;
} phys_world_t;

/* ── API (implemented in game_physics.c) ──────────────────────────────────── */

void phys_init(phys_world_t *pw, float gravity_x, float gravity_y);
void phys_set_collision_callback(phys_world_t *pw, phys_collision_fn fn, void *userdata);
void phys_set_trigger_callback(phys_world_t *pw, phys_collision_fn fn, void *userdata);
void phys_step(phys_world_t *pw, ecs_world_t *ew, float dt);
