/**
 * lib/gfx/game_engine.c — AzamiOS 2D Arcade Game Engine Implementation
 *
 * Pure C implementation of 2D bounding box collision math and physics updates.
 */
#include "game_engine.h"

bool game_check_collision(const sprite_t *a, const sprite_t *b) {
    if (!a || !b || !a->active || !b->active) return false;
    return (a->x < b->x + b->w &&
            a->x + a->w > b->x &&
            a->y < b->y + b->h &&
            a->y + a->h > b->y);
}

void game_clamp_sprite(sprite_t *s, int min_x, int min_y, int max_x, int max_y) {
    if (!s) return;
    if (s->x < min_x) s->x = min_x;
    if (s->y < min_y) s->y = min_y;
    if (s->x + s->w > max_x) s->x = max_x - s->w;
    if (s->y + s->h > max_y) s->y = max_y - s->h;
}

void game_update_physics(sprite_t *s, int min_x, int min_y, int max_x, int max_y, bool bounce) {
    if (!s || !s->active) return;
    s->x += s->vx;
    s->y += s->vy;

    if (bounce) {
        if (s->x <= min_x) {
            s->x = min_x;
            s->vx = -s->vx;
        } else if (s->x + s->w >= max_x) {
            s->x = max_x - s->w;
            s->vx = -s->vx;
        }
        if (s->y <= min_y) {
            s->y = min_y;
            s->vy = -s->vy;
        } else if (s->y + s->h >= max_y) {
            s->y = max_y - s->h;
            s->vy = -s->vy;
        }
    } else {
        game_clamp_sprite(s, min_x, min_y, max_x, max_y);
    }
}
