/* ============================================================================
 * AzamiOS Game Framework — Scene / State Machine
 * File: userland/libgame/include/game/game_scene.h
 *
 * Scene management with:
 *  • Named scene registration with lifecycle callbacks
 *  • Scene stack (push/pop for pause menus, overlays)
 *  • Transition effects (fade-to-black, instant)
 *
 * Tuning defines:
 *   SCENE_MAX_SCENES    — max registered scenes (default 16)
 *   SCENE_STACK_DEPTH   — max scene stack depth (default 8)
 *   SCENE_NAME_LEN      — max scene name length (default 24)
 * ============================================================================ */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifndef SCENE_MAX_SCENES
#define SCENE_MAX_SCENES  16
#endif
#ifndef SCENE_STACK_DEPTH
#define SCENE_STACK_DEPTH 8
#endif
#ifndef SCENE_NAME_LEN
#define SCENE_NAME_LEN    24
#endif

/* Forward-declare game context */
struct game_ctx;

/* ── Scene Callbacks ──────────────────────────────────────────────────────── */

typedef struct {
    void (*on_enter)(struct game_ctx *g);
    void (*on_exit)(struct game_ctx *g);
    void (*on_update)(struct game_ctx *g, float dt);
    void (*on_render)(struct game_ctx *g);
    void (*on_pause)(struct game_ctx *g);   /* called when another scene pushes on top */
    void (*on_resume)(struct game_ctx *g);  /* called when popped-back-to */
} scene_t;

/* ── Transition Types ─────────────────────────────────────────────────────── */

typedef enum {
    SCENE_TRANS_NONE,
    SCENE_TRANS_FADE_BLACK,
    SCENE_TRANS_FADE_WHITE
} scene_transition_t;

/* ── Scene Manager ────────────────────────────────────────────────────────── */

typedef struct {
    /* Registry */
    struct {
        char    name[SCENE_NAME_LEN];
        scene_t scene;
        bool    active;
    } registry[SCENE_MAX_SCENES];
    int registry_count;

    /* Scene stack */
    int stack[SCENE_STACK_DEPTH];   /* indices into registry */
    int stack_depth;

    /* Pending transition */
    bool               has_pending;
    int                pending_scene;  /* registry index to switch to */
    bool               pending_is_push;
    scene_transition_t pending_trans;
    float              trans_duration;
    float              trans_timer;
    bool               trans_active;
    bool               trans_halfway;  /* true after fade-in completes, scene switched */
    uint32_t           trans_color;
} scene_mgr_t;

/* ── API (implemented in game_scene.c) ────────────────────────────────────── */

void scene_init(scene_mgr_t *sm);

/* Register a named scene. */
void scene_register(scene_mgr_t *sm, const char *name, const scene_t *scene);

/* Switch to a named scene (replaces current). */
void scene_switch(scene_mgr_t *sm, const char *name);

/* Switch with a transition effect. */
void scene_switch_with(scene_mgr_t *sm, const char *name,
                       scene_transition_t trans, float duration);

/* Push a scene onto the stack (e.g., pause menu). */
void scene_push(scene_mgr_t *sm, const char *name);

/* Pop the top scene off the stack. */
void scene_pop(scene_mgr_t *sm);

/* Update the current scene (handles transitions). */
void scene_update(scene_mgr_t *sm, struct game_ctx *g, float dt);

/* Render the current scene. */
void scene_render(scene_mgr_t *sm, struct game_ctx *g);

/* Get the name of the current active scene, or NULL. */
const char *scene_current_name(const scene_mgr_t *sm);

/* Get the transition overlay alpha (0..255) for rendering. */
int scene_transition_alpha(const scene_mgr_t *sm);
