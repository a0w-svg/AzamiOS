/* ============================================================================
 * AzamiOS Game Framework — ECS Implementation
 * File: userland/libgame/game_ecs.c
 * ============================================================================ */

#include "include/game/game_ecs.h"
#include <stdlib.h>
#include <string.h>

/* ── World Initialization ─────────────────────────────────────────────────── */

void ecs_init(ecs_world_t *w)
{
    memset(w, 0, sizeof(*w));

    /* Build free list (all slots available, lowest first) */
    for (int i = 0; i < ECS_MAX_ENTITIES; i++) {
        w->free_list[i] = (uint16_t)(ECS_MAX_ENTITIES - 1 - i);
    }
    w->free_count = ECS_MAX_ENTITIES;

    /* Register built-in component types */
    ecs_register_component(w, (int)sizeof(comp_transform_t));  /* COMP_TRANSFORM = 0 */
    ecs_register_component(w, (int)sizeof(comp_sprite_t));     /* COMP_SPRITE    = 1 */
    ecs_register_component(w, (int)sizeof(comp_rigidbody_t));  /* COMP_RIGIDBODY = 2 */
    ecs_register_component(w, (int)sizeof(comp_collider_t));   /* COMP_COLLIDER  = 3 */
    ecs_register_component(w, (int)sizeof(comp_animator_t));   /* COMP_ANIMATOR  = 4 */
    ecs_register_component(w, (int)sizeof(comp_tag_t));        /* COMP_TAG       = 5 */
}

void ecs_destroy_world(ecs_world_t *w)
{
    for (int i = 0; i < w->comp_count; i++) {
        if (w->comp_data[i]) {
            free(w->comp_data[i]);
            w->comp_data[i] = NULL;
        }
    }
}

/* ── Component Registration ───────────────────────────────────────────────── */

ecs_comp_id_t ecs_register_component(ecs_world_t *w, int struct_size)
{
    if (w->comp_count >= ECS_MAX_COMPONENTS) return (ecs_comp_id_t)255;

    ecs_comp_id_t id = (ecs_comp_id_t)w->comp_count;
    w->comp_size[id] = struct_size;

    /* Allocate dense array for all possible entities */
    w->comp_data[id] = calloc((size_t)ECS_MAX_ENTITIES, (size_t)struct_size);
    w->comp_count++;
    return id;
}

/* ── Entity Lifecycle ─────────────────────────────────────────────────────── */

ecs_entity_t ecs_spawn(ecs_world_t *w)
{
    if (w->free_count == 0) return ECS_ENTITY_NULL;

    uint16_t idx = w->free_list[--w->free_count];
    w->alive[idx] = true;
    w->masks[idx] = 0;
    w->entity_count++;

    return ecs_entity_make(idx, w->generation[idx]);
}

void ecs_kill(ecs_world_t *w, ecs_entity_t e)
{
    uint16_t idx = ecs_entity_index(e);
    if (idx >= ECS_MAX_ENTITIES) return;
    if (!w->alive[idx]) return;
    if (w->generation[idx] != ecs_entity_gen(e)) return;

    /* Clear all component data for this entity */
    for (int c = 0; c < w->comp_count; c++) {
        if (w->masks[idx] & ECS_COMP_BIT(c)) {
            char *base = (char *)w->comp_data[c];
            memset(base + idx * w->comp_size[c], 0, (size_t)w->comp_size[c]);
        }
    }

    w->alive[idx] = false;
    w->masks[idx] = 0;
    w->generation[idx]++;
    w->entity_count--;

    /* Return to free list */
    if (w->free_count < ECS_MAX_ENTITIES) {
        w->free_list[w->free_count++] = idx;
    }
}

bool ecs_alive(const ecs_world_t *w, ecs_entity_t e)
{
    if (e == ECS_ENTITY_NULL) return false;
    uint16_t idx = ecs_entity_index(e);
    if (idx >= ECS_MAX_ENTITIES) return false;
    return w->alive[idx] && w->generation[idx] == ecs_entity_gen(e);
}

/* ── Component Access ─────────────────────────────────────────────────────── */

void *ecs_add(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp)
{
    if (!ecs_alive(w, e)) return NULL;
    if (comp >= w->comp_count) return NULL;

    uint16_t idx = ecs_entity_index(e);
    w->masks[idx] |= ECS_COMP_BIT(comp);

    char *base = (char *)w->comp_data[comp];
    void *ptr = base + idx * w->comp_size[comp];
    memset(ptr, 0, (size_t)w->comp_size[comp]);
    return ptr;
}

void *ecs_get(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp)
{
    if (!ecs_alive(w, e)) return NULL;
    if (comp >= w->comp_count) return NULL;

    uint16_t idx = ecs_entity_index(e);
    if (!(w->masks[idx] & ECS_COMP_BIT(comp))) return NULL;

    char *base = (char *)w->comp_data[comp];
    return base + idx * w->comp_size[comp];
}

const void *ecs_get_const(const ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp)
{
    if (e == ECS_ENTITY_NULL) return NULL;
    uint16_t idx = ecs_entity_index(e);
    if (idx >= ECS_MAX_ENTITIES) return NULL;
    if (!w->alive[idx] || w->generation[idx] != ecs_entity_gen(e)) return NULL;
    if (comp >= (ecs_comp_id_t)w->comp_count) return NULL;
    if (!(w->masks[idx] & ECS_COMP_BIT(comp))) return NULL;

    const char *base = (const char *)w->comp_data[comp];
    return base + idx * w->comp_size[comp];
}

bool ecs_has(const ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp)
{
    if (e == ECS_ENTITY_NULL) return false;
    uint16_t idx = ecs_entity_index(e);
    if (idx >= ECS_MAX_ENTITIES) return false;
    if (!w->alive[idx] || w->generation[idx] != ecs_entity_gen(e)) return false;
    if (comp >= (ecs_comp_id_t)w->comp_count) return false;
    return (w->masks[idx] & ECS_COMP_BIT(comp)) != 0;
}

void ecs_remove(ecs_world_t *w, ecs_entity_t e, ecs_comp_id_t comp)
{
    if (!ecs_alive(w, e)) return;
    if (comp >= w->comp_count) return;

    uint16_t idx = ecs_entity_index(e);
    if (w->masks[idx] & ECS_COMP_BIT(comp)) {
        char *base = (char *)w->comp_data[comp];
        memset(base + idx * w->comp_size[comp], 0, (size_t)w->comp_size[comp]);
        w->masks[idx] &= ~ECS_COMP_BIT(comp);
    }
}

/* ── System Registration & Execution ──────────────────────────────────────── */

void ecs_register_system(ecs_world_t *w, ecs_comp_mask_t required, ecs_system_fn fn, int priority)
{
    if (w->system_count >= ECS_MAX_SYSTEMS) return;

    int slot = w->system_count;

    /* Insert sorted by priority (lower = earlier) */
    while (slot > 0 && w->systems[slot - 1].priority > priority) {
        w->systems[slot] = w->systems[slot - 1];
        slot--;
    }

    w->systems[slot].required = required;
    w->systems[slot].fn = fn;
    w->systems[slot].priority = priority;
    w->system_count++;
}

void ecs_tick(ecs_world_t *w, float dt)
{
    for (int s = 0; s < w->system_count; s++) {
        ecs_comp_mask_t req = w->systems[s].required;
        ecs_system_fn fn = w->systems[s].fn;

        for (uint16_t i = 0; i < ECS_MAX_ENTITIES; i++) {
            if (!w->alive[i]) continue;
            if ((w->masks[i] & req) != req) continue;

            ecs_entity_t e = ecs_entity_make(i, w->generation[i]);
            fn(w, e, dt);
        }
    }
}

/* ── Iteration Helper ─────────────────────────────────────────────────────── */

void ecs_each(ecs_world_t *w, ecs_comp_mask_t required,
              void (*fn)(ecs_world_t *, ecs_entity_t, void *),
              void *userdata)
{
    for (uint16_t i = 0; i < ECS_MAX_ENTITIES; i++) {
        if (!w->alive[i]) continue;
        if ((w->masks[i] & required) != required) continue;

        ecs_entity_t e = ecs_entity_make(i, w->generation[i]);
        fn(w, e, userdata);
    }
}

int ecs_count(const ecs_world_t *w, ecs_comp_mask_t required)
{
    int n = 0;
    for (uint16_t i = 0; i < ECS_MAX_ENTITIES; i++) {
        if (w->alive[i] && (w->masks[i] & required) == required) n++;
    }
    return n;
}
