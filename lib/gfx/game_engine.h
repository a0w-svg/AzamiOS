/**
 * lib/gfx/game_engine.h — AzamiOS 2D Arcade Game Engine Framework
 *
 * Kernel-independent pure C header for 2D sprites, bounding-box collisions,
 * physics movement, and animation state management.
 */
#ifndef GAME_ENGINE_H
#define GAME_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int      x, y;          /* Position (fixed or pixel) */
    int      w, h;          /* Dimensions */
    int      vx, vy;        /* Velocity components */
    uint32_t color;         /* ARGB / RGB color */
    bool     active;        /* Is sprite rendered/active */
} sprite_t;

/* ── Public Game API ────────────────────────────────────────────────── */
bool game_check_collision(const sprite_t *a, const sprite_t *b);
void game_update_physics(sprite_t *s, int min_x, int min_y, int max_x, int max_y, bool bounce);
void game_clamp_sprite(sprite_t *s, int min_x, int min_y, int max_x, int max_y);

#endif /* GAME_ENGINE_H */
