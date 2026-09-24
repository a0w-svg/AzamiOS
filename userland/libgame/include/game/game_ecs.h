/* ============================================================================
 * AzamiOS Game Framework — Entity-Component-System
 * File: userland/libgame/include/game/game_ecs.h
 *
 * Archetype-free ECS with dense component arrays:
 *  • Entities: uint16_t IDs with generation counter for dangling reference safety
 *  • Components: up to 32 registered types, stored in dense arrays per type
 *  • Systems: function pointers iterated each frame over matching entity masks
 *  • Built-in component types: transform, sprite, rigidbody, collider, animator, tag
 *
 * Tuning defines (set before including):
 *   ECS_MAX_ENTITIES    — max alive entities  (default 256)
 *   ECS_MAX_COMPONENTS  — max component types (default 32)
 *   ECS_MAX_SYSTEMS     — max registered systems (default 16)
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "game_math.h"
#include "game_render.h"

/* ── Tuning Defaults ──────────────────────────────────────────────────────── */

#ifndef ECS_MAX_ENTITIES
#define ECS_MAX_ENTITIES   256
#endif
#ifndef ECS_MAX_COMPONENTS
#define ECS_MAX_COMPONENTS 32
#endif
#ifndef ECS_MAX_SYSTEMS
#define ECS_MAX_SYSTEMS    16
#endif

/* ── Entity Handle ────────────────────────────────────────────────────────── */

/* Packed handle: low 16 bits = index, high 16 bits = generation.
 * A destroyed entity increments generation, so stale handles are detectable. */
typedef uint32_t ecs_entity_t;

#define ECS_ENTITY_NULL  0xFFFFFFFFu

static inline uint16_t ecs_entity_index(ecs_entity_t e) { return (uint16_t)(e & 0xFFFF); }
static inline uint16_t ecs_entity_gen(ecs_entity_t e)   { return (uint16_t)(e >> 16); }
static inline ecs_entity_t ecs_entity_make(uint16_t idx, uint16_t gen) {
    return (uint32_t)idx | ((uint32_t)gen << 16);
}

/* ── Component ID and Mask ────────────────────────────────────────────────── */

typedef uint8_t  ecs_comp_id_t;
typedef uint32_t ecs_comp_mask_t;

#define ECS_COMP_BIT(id)  (1u << (id))

/* ── Built-in Component Types ─────────────────────────────────────────────── */

/* Component ID 0: Transform (position, rotation, scale) */
#define COMP_TRANSFORM 0
typedef struct {
    vec2_t  position;
    float   rotation;   /* degrees */
    vec2_t  scale;
} comp_transform_t;

/* Component ID 1: Sprite rendering */
#define COMP_SPRITE    1
typedef struct {
    sprite_t      sprite;
    sprite_anim_t anim;
    bool          use_anim;
    int           z_order;   /* drawing order (higher = on top) */
    bool          visible;
} comp_sprite_t;

/* Component ID 2: Rigid body physics */
#define COMP_RIGIDBODY 2
typedef struct {
    vec2_t velocity;
    vec2_t acceleration;
    float  mass;          /* 0 = static/immovable */
    float  restitution;   /* bounce factor 0..1 */
    float  friction;      /* velocity damping 0..1 */
    float  drag;          /* air resistance */
    bool   is_kinematic;  /* moves but not affected by forces */
} comp_rigidbody_t;

/* Component ID 3: Collider shape */
#define COMP_COLLIDER  3
typedef enum {
    COLLIDER_AABB,
    COLLIDER_CIRCLE
} collider_type_t;

typedef struct {
    collider_type_t type;
    union {
        struct { float w, h; float off_x, off_y; } box;
        struct { float r; float off_x, off_y; }    circle;
    };
    uint32_t layer;       /* collision layer bitmask */
    uint32_t mask;        /* what layers to collide with */
    bool     is_trigger;  /* triggers generate events but no separation */
} comp_collider_t;

/* Component ID 4: Animator state */
#define COMP_ANIMATOR  4
typedef struct {
    int    current_anim;   /* index into a game-managed animation table */
    float  speed_scale;
} comp_animator_t;

/* Component ID 5: Tag (generic 32-bit tag for game logic) */
#define COMP_TAG       5
typedef struct {
    uint32_t tag;
    int      int_data;
    void    *user_data;
} comp_tag_t;

#define ECS_BUILTIN_COUNT 6

/* ── System Function Signature ────────────────────────────────────────────── */

struct ecs_world;
typedef void (*ecs_system_fn)(struct ecs_world *w, ecs_entity_t entity, float dt);

/* ── ECS World ────────────────────────────────────────────────────────────── */

typedef struct ecs_world {
    /* Entity management */
    uint16_t       generation[ECS_MAX_ENTITIES];
    bool           alive[ECS_MAX_ENTITIES];
    ecs_comp_mask_t masks[ECS_MAX_ENTITIES];
    uint16_t       entity_count;

    /* Free list for recycling entity slots */
    uint16_t       free_list[ECS_MAX_ENTITIES];
    int            free_count;

    /* Component storage: one byte array per component type,
     * indexed by entity index. The actual struct size is stored in comp_size[]. */
    void          *comp_data[ECS_MAX_COMPONENTS];
    int            comp_size[ECS_MAX_COMPONENTS];
    int            comp_count;

    /* Systems */
    struct {
        ecs_comp_mask_t required;
        ecs_system_fn   fn;
        int             priority;  /* lower = runs first */
    } systems[ECS_MAX_SYSTEMS];
    int system_count;
} ecs_world_t;

/* ── API declarations (implemented in game_ecs.c) ─────────────────────────── */

void         ecs_init(ecs_world_t *w);
void         ecs_destroy_world(ecs_world_t *w);

/* Register a custom component type. Returns comp_id, or -1 on error. */
ecs_comp_id_t ecs_register_component(ecs_world_t *w, int struct_size);

ecs_entity_t ecs_spawn(ecs_world_t *w);
void         ecs_kill(ecs_world_t *w, ecs_entity_t e);
bool         ecs_alive(const ecs_world_t *w, ecs_entity_t e);

void        *ecs_add(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp);
void        *ecs_get(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp);
const void  *ecs_get_const(const ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp);
bool         ecs_has(const ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp);
void         ecs_remove(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp);

void         ecs_register_system(ecs_world_t *w, ecs_comp_mask_t required, ecs_system_fn fn, int priority);
void         ecs_tick(ecs_world_t *w, float dt);

/* Iteration helper: call fn for every alive entity matching mask */
void         ecs_each(ecs_world_t *w, ecs_comp_mask_t required,
                      void (*fn)(ecs_world_t *, ecs_entity_t, void *),
                      void *userdata);

/* Count alive entities matching mask */
int          ecs_count(const ecs_world_t *w, ecs_comp_mask_t required);

/* Convenience: get built-in component as typed pointer */
static inline comp_transform_t *ecs_transform(ecs_world_t *w, ecs_entity_t e)
{
    return (comp_transform_t *)ecs_get(w, e, COMP_TRANSFORM);
}

static inline comp_sprite_t *ecs_sprite(ecs_world_t *w, ecs_entity_t e)
{
    return (comp_sprite_t *)ecs_get(w, e, COMP_SPRITE);
}

static inline comp_rigidbody_t *ecs_rigidbody(ecs_world_t *w, ecs_entity_t e)
{
    return (comp_rigidbody_t *)ecs_get(w, e, COMP_RIGIDBODY);
}

static inline comp_collider_t *ecs_collider(ecs_world_t *w, ecs_entity_t e)
{
    return (comp_collider_t *)ecs_get(w, e, COMP_COLLIDER);
}

static inline comp_tag_t *ecs_tag(ecs_world_t *w, ecs_entity_t e)
{
    return (comp_tag_t *)ecs_get(w, e, COMP_TAG);
}
