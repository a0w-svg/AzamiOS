/* ============================================================================
 * AzamiOS Game Framework — Input Abstraction
 * File: userland/libgame/include/game/game_input.h
 *
 * Header-only input system:
 *  • Key state tracking with edge detection (pressed/released this frame)
 *  • Mouse position, button state, scroll wheel
 *  • Named action bindings (e.g., "jump" → Space/W)
 *  • Up to 256 key slots (matching AzamiOS keycode range)
 *  • Up to 32 action bindings
 * ============================================================================ */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Key State Bits ───────────────────────────────────────────────────────── */
#define INPUT_MAX_KEYS    256
#define INPUT_MAX_ACTIONS 32
#define INPUT_MAX_ACTION_KEYS 4   /* keys per action binding */
#define INPUT_ACTION_NAME_LEN 16

typedef struct {
    /* Current and previous frame key state (bitfields) */
    uint8_t  keys_curr[INPUT_MAX_KEYS / 8];
    uint8_t  keys_prev[INPUT_MAX_KEYS / 8];

    /* Mouse state */
    int      mouse_x, mouse_y;
    int      mouse_dx, mouse_dy;
    int      mouse_wheel;
    uint8_t  mouse_buttons_curr;
    uint8_t  mouse_buttons_prev;

    /* Key modifier flags (shift, ctrl, alt) */
    uint16_t modifiers;

    /* Named action bindings */
    struct {
        char    name[INPUT_ACTION_NAME_LEN];
        int     keys[INPUT_MAX_ACTION_KEYS];
        int     num_keys;
    } actions[INPUT_MAX_ACTIONS];
    int action_count;
} input_state_t;

/* ── Initialization ───────────────────────────────────────────────────────── */

static inline void input_init(input_state_t *in)
{
    memset(in, 0, sizeof(*in));
}

/* Call at the start of each frame to snapshot previous state */
static inline void input_begin_frame(input_state_t *in)
{
    memcpy(in->keys_prev, in->keys_curr, sizeof(in->keys_prev));
    in->mouse_buttons_prev = in->mouse_buttons_curr;
    in->mouse_dx = 0;
    in->mouse_dy = 0;
    in->mouse_wheel = 0;
}

/* ── Key Events (called by the game loop on AZ_WM_KEY_EVENT) ──────────────── */

static inline void input_set_key(input_state_t *in, int key, bool pressed)
{
    if (key < 0 || key >= INPUT_MAX_KEYS) return;
    int byte = key / 8;
    int bit  = key % 8;
    if (pressed)
        in->keys_curr[byte] |= (uint8_t)(1u << bit);
    else
        in->keys_curr[byte] &= (uint8_t)~(1u << bit);
}

static inline void input_set_modifiers(input_state_t *in, uint16_t mods)
{
    in->modifiers = mods;
}

/* ── Mouse Events (called by the game loop on AZ_WM_MOUSE_EVENT) ─────────── */

static inline void input_set_mouse(input_state_t *in, int x, int y, int dx, int dy,
                                    uint8_t buttons, int wheel)
{
    in->mouse_x = x;
    in->mouse_y = y;
    in->mouse_dx += dx;
    in->mouse_dy += dy;
    in->mouse_buttons_curr = buttons;
    in->mouse_wheel += wheel;
}

/* ── Key Queries ──────────────────────────────────────────────────────────── */

/* Is key currently held down? */
static inline bool input_key_down(const input_state_t *in, int key)
{
    if (key < 0 || key >= INPUT_MAX_KEYS) return false;
    return (in->keys_curr[key / 8] >> (key % 8)) & 1;
}

/* Was key just pressed this frame (edge-triggered)? */
static inline bool input_key_pressed(const input_state_t *in, int key)
{
    if (key < 0 || key >= INPUT_MAX_KEYS) return false;
    bool curr = (in->keys_curr[key / 8] >> (key % 8)) & 1;
    bool prev = (in->keys_prev[key / 8] >> (key % 8)) & 1;
    return curr && !prev;
}

/* Was key just released this frame? */
static inline bool input_key_released(const input_state_t *in, int key)
{
    if (key < 0 || key >= INPUT_MAX_KEYS) return false;
    bool curr = (in->keys_curr[key / 8] >> (key % 8)) & 1;
    bool prev = (in->keys_prev[key / 8] >> (key % 8)) & 1;
    return !curr && prev;
}

/* ── Mouse Queries ────────────────────────────────────────────────────────── */

/* Mouse button index: 0=left, 1=right, 2=middle */
static inline bool input_mouse_down(const input_state_t *in, int button)
{
    return (in->mouse_buttons_curr >> button) & 1;
}

static inline bool input_mouse_pressed(const input_state_t *in, int button)
{
    bool curr = (in->mouse_buttons_curr >> button) & 1;
    bool prev = (in->mouse_buttons_prev >> button) & 1;
    return curr && !prev;
}

static inline bool input_mouse_released(const input_state_t *in, int button)
{
    bool curr = (in->mouse_buttons_curr >> button) & 1;
    bool prev = (in->mouse_buttons_prev >> button) & 1;
    return !curr && prev;
}

/* ── Action Bindings ──────────────────────────────────────────────────────── */

static inline void input_bind_action(input_state_t *in, const char *action, int key)
{
    /* Find or create action slot */
    int slot = -1;
    for (int i = 0; i < in->action_count; i++) {
        if (strncmp(in->actions[i].name, action, INPUT_ACTION_NAME_LEN - 1) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (in->action_count >= INPUT_MAX_ACTIONS) return;
        slot = in->action_count++;
        strncpy(in->actions[slot].name, action, INPUT_ACTION_NAME_LEN - 1);
        in->actions[slot].name[INPUT_ACTION_NAME_LEN - 1] = '\0';
        in->actions[slot].num_keys = 0;
    }

    /* Add key to slot if not full and not duplicate */
    if (in->actions[slot].num_keys >= INPUT_MAX_ACTION_KEYS) return;
    for (int i = 0; i < in->actions[slot].num_keys; i++) {
        if (in->actions[slot].keys[i] == key) return;
    }
    in->actions[slot].keys[in->actions[slot].num_keys++] = key;
}

/* Is any key bound to this action currently held down? */
static inline bool input_action_down(const input_state_t *in, const char *action)
{
    for (int i = 0; i < in->action_count; i++) {
        if (strncmp(in->actions[i].name, action, INPUT_ACTION_NAME_LEN - 1) == 0) {
            for (int k = 0; k < in->actions[i].num_keys; k++) {
                if (input_key_down(in, in->actions[i].keys[k]))
                    return true;
            }
            return false;
        }
    }
    return false;
}

/* Was any key bound to this action just pressed this frame? */
static inline bool input_action_pressed(const input_state_t *in, const char *action)
{
    for (int i = 0; i < in->action_count; i++) {
        if (strncmp(in->actions[i].name, action, INPUT_ACTION_NAME_LEN - 1) == 0) {
            for (int k = 0; k < in->actions[i].num_keys; k++) {
                if (input_key_pressed(in, in->actions[i].keys[k]))
                    return true;
            }
            return false;
        }
    }
    return false;
}

/* Was any key bound to this action just released this frame? */
static inline bool input_action_released(const input_state_t *in, const char *action)
{
    for (int i = 0; i < in->action_count; i++) {
        if (strncmp(in->actions[i].name, action, INPUT_ACTION_NAME_LEN - 1) == 0) {
            for (int k = 0; k < in->actions[i].num_keys; k++) {
                if (input_key_released(in, in->actions[i].keys[k]))
                    return true;
            }
            return false;
        }
    }
    return false;
}
