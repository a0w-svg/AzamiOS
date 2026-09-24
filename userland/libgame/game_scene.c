/* ============================================================================
 * AzamiOS Game Framework — Scene Manager Implementation
 * File: userland/libgame/game_scene.c
 * ============================================================================ */

#include "include/game/game_scene.h"
#include <string.h>
#include <stddef.h>

void scene_init(scene_mgr_t *sm)
{
    if (!sm) return;
    memset(sm, 0, sizeof(*sm));
}

static int scene_find(const scene_mgr_t *sm, const char *name)
{
    if (!sm || !name) return -1;
    for (int i = 0; i < sm->registry_count; i++) {
        if (sm->registry[i].active &&
            strncmp(sm->registry[i].name, name, SCENE_NAME_LEN - 1) == 0) {
            return i;
        }
    }
    return -1;
}

void scene_register(scene_mgr_t *sm, const char *name, const scene_t *scene)
{
    if (!sm || !name || !scene) return;

    int idx = scene_find(sm, name);
    if (idx >= 0) {
        sm->registry[idx].scene = *scene;
        return;
    }

    if (sm->registry_count < SCENE_MAX_SCENES) {
        idx = sm->registry_count++;
        strncpy(sm->registry[idx].name, name, SCENE_NAME_LEN - 1);
        sm->registry[idx].name[SCENE_NAME_LEN - 1] = '\0';
        sm->registry[idx].scene = *scene;
        sm->registry[idx].active = true;
    }
}

static void scene_exec_exit(scene_mgr_t *sm, struct game_ctx *g)
{
    if (sm->stack_depth > 0) {
        int cur_idx = sm->stack[sm->stack_depth - 1];
        if (sm->registry[cur_idx].scene.on_exit) {
            sm->registry[cur_idx].scene.on_exit(g);
        }
    }
}

static void scene_exec_enter(scene_mgr_t *sm, struct game_ctx *g)
{
    if (sm->stack_depth > 0) {
        int cur_idx = sm->stack[sm->stack_depth - 1];
        if (sm->registry[cur_idx].scene.on_enter) {
            sm->registry[cur_idx].scene.on_enter(g);
        }
    }
}

void scene_switch(scene_mgr_t *sm, const char *name)
{
    if (!sm || !name) return;
    int idx = scene_find(sm, name);
    if (idx < 0) return;

    sm->has_pending = true;
    sm->pending_scene = idx;
    sm->pending_is_push = false;
    sm->pending_trans = SCENE_TRANS_NONE;
    sm->trans_active = false;
}

void scene_switch_with(scene_mgr_t *sm, const char *name,
                       scene_transition_t trans, float duration)
{
    if (!sm || !name) return;
    int idx = scene_find(sm, name);
    if (idx < 0) return;

    if (trans == SCENE_TRANS_NONE || duration <= 0.0f) {
        scene_switch(sm, name);
        return;
    }

    sm->has_pending = true;
    sm->pending_scene = idx;
    sm->pending_is_push = false;
    sm->pending_trans = trans;
    sm->trans_duration = duration;
    sm->trans_timer = 0.0f;
    sm->trans_active = true;
    sm->trans_halfway = false;
    sm->trans_color = (trans == SCENE_TRANS_FADE_WHITE) ? 0xFFFFFFFF : 0xFF000000;
}

void scene_push(scene_mgr_t *sm, const char *name)
{
    if (!sm || !name) return;
    int idx = scene_find(sm, name);
    if (idx < 0) return;

    sm->has_pending = true;
    sm->pending_scene = idx;
    sm->pending_is_push = true;
    sm->pending_trans = SCENE_TRANS_NONE;
    sm->trans_active = false;
}

void scene_pop(scene_mgr_t *sm)
{
    if (!sm || sm->stack_depth <= 1) return;
    /* Pop directly or mark */
    sm->stack_depth--;
}

void scene_update(scene_mgr_t *sm, struct game_ctx *g, float dt)
{
    if (!sm) return;

    /* Handle transition */
    if (sm->trans_active) {
        sm->trans_timer += dt;
        float half = sm->trans_duration * 0.5f;

        if (!sm->trans_halfway && sm->trans_timer >= half) {
            /* Halfway point: swap the scene */
            if (sm->pending_is_push) {
                if (sm->stack_depth > 0 && sm->registry[sm->stack[sm->stack_depth - 1]].scene.on_pause) {
                    sm->registry[sm->stack[sm->stack_depth - 1]].scene.on_pause(g);
                }
                if (sm->stack_depth < SCENE_STACK_DEPTH) {
                    sm->stack[sm->stack_depth++] = sm->pending_scene;
                    scene_exec_enter(sm, g);
                }
            } else {
                scene_exec_exit(sm, g);
                sm->stack_depth = 1;
                sm->stack[0] = sm->pending_scene;
                scene_exec_enter(sm, g);
            }
            sm->trans_halfway = true;
            sm->has_pending = false;
        }

        if (sm->trans_timer >= sm->trans_duration) {
            sm->trans_active = false;
            sm->trans_timer = 0.0f;
        }
    } else if (sm->has_pending) {
        /* Instant switch/push */
        sm->has_pending = false;
        if (sm->pending_is_push) {
            if (sm->stack_depth > 0 && sm->registry[sm->stack[sm->stack_depth - 1]].scene.on_pause) {
                sm->registry[sm->stack[sm->stack_depth - 1]].scene.on_pause(g);
            }
            if (sm->stack_depth < SCENE_STACK_DEPTH) {
                sm->stack[sm->stack_depth++] = sm->pending_scene;
                scene_exec_enter(sm, g);
            }
        } else {
            scene_exec_exit(sm, g);
            sm->stack_depth = 1;
            sm->stack[0] = sm->pending_scene;
            scene_exec_enter(sm, g);
        }
    }

    /* Update current active scene */
    if (sm->stack_depth > 0) {
        int cur_idx = sm->stack[sm->stack_depth - 1];
        if (sm->registry[cur_idx].scene.on_update) {
            sm->registry[cur_idx].scene.on_update(g, dt);
        }
    }
}

void scene_render(scene_mgr_t *sm, struct game_ctx *g)
{
    if (!sm) return;
    if (sm->stack_depth > 0) {
        int cur_idx = sm->stack[sm->stack_depth - 1];
        if (sm->registry[cur_idx].scene.on_render) {
            sm->registry[cur_idx].scene.on_render(g);
        }
    }
}

const char *scene_current_name(const scene_mgr_t *sm)
{
    if (!sm || sm->stack_depth <= 0) return NULL;
    int cur_idx = sm->stack[sm->stack_depth - 1];
    return sm->registry[cur_idx].name;
}

int scene_transition_alpha(const scene_mgr_t *sm)
{
    if (!sm || !sm->trans_active || sm->trans_duration <= 0.0f) return 0;

    float t = sm->trans_timer / sm->trans_duration;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float alpha_f;
    if (t <= 0.5f) {
        alpha_f = t * 2.0f;          /* 0.0 -> 1.0 */
    } else {
        alpha_f = (1.0f - t) * 2.0f;  /* 1.0 -> 0.0 */
    }

    int a = (int)(alpha_f * 255.0f);
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return a;
}
