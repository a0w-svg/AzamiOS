/* ============================================================================
 * AzamiOS Game Framework Demo — Breakout / Brick Breaker
 * File: userland/apps/breakout/main.c
 *
 * Demonstrates the full libgame framework:
 *  • game_run() bootstrap & autonomous timer loop
 *  • Scene manager (Title, Playing, GameOver, Victory) with fade transitions
 *  • Input system (Keyboard + Mouse paddle control)
 *  • 2D Camera with screen shake on impacts
 *  • Particle system for brick explosions
 *  • Sound effects via procedural waveform synthesis (game_audio)
 *  • Asset manager for procedural brick styling
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <game/game.h>

#define WIN_W 640
#define WIN_H 520

#define BRICK_ROWS 5
#define BRICK_COLS 10
#define BRICK_W    56
#define BRICK_H    18
#define BRICK_GAP  6

#define PADDLE_W   90
#define PADDLE_H   14
#define PADDLE_SPEED 420.0f

#define BALL_RADIUS 6
#define BALL_SPEED  320.0f

/* ── Brick State ──────────────────────────────────────────────────────────── */

typedef struct {
    float    x, y;
    bool     alive;
    uint32_t color;
    int      points;
} brick_t;

/* ── Game State ───────────────────────────────────────────────────────────── */

typedef struct {
    /* Paddle */
    float paddle_x;
    float paddle_y;

    /* Ball */
    float ball_x;
    float ball_y;
    float ball_vx;
    float ball_vy;
    bool  ball_attached;

    /* Bricks */
    brick_t bricks[BRICK_ROWS * BRICK_COLS];
    int     bricks_left;

    /* Stats */
    int  score;
    int  lives;
    int  high_score;

    /* FX */
    particle_sys_t particles;
    game_rng_t     rng;

    /* SFX IDs */
    int sfx_paddle;
    int sfx_brick;
    int sfx_wall;
    int sfx_lose;
    int sfx_win;
} breakout_t;

static breakout_t g_state;

/* Palette (Catppuccin Mocha inspired) */
static const uint32_t ROW_COLORS[BRICK_ROWS] = {
    0xFFF38BA8u, /* Red */
    0xFFFAB387u, /* Peach */
    0xFFF9E2AFu, /* Yellow */
    0xFFA6E3A1u, /* Green */
    0xFF89B4FAu  /* Blue */
};

/* ── Game Logic Helpers ───────────────────────────────────────────────────── */

static void reset_level(breakout_t *b)
{
    b->paddle_x = (WIN_W - PADDLE_W) * 0.5f;
    b->paddle_y = WIN_H - 45.0f;

    b->ball_attached = true;
    b->ball_x = b->paddle_x + PADDLE_W * 0.5f;
    b->ball_y = b->paddle_y - BALL_RADIUS - 1.0f;
    b->ball_vx = BALL_SPEED * 0.707f;
    b->ball_vy = -BALL_SPEED * 0.707f;

    b->bricks_left = BRICK_ROWS * BRICK_COLS;
    float total_w = BRICK_COLS * BRICK_W + (BRICK_COLS - 1) * BRICK_GAP;
    float start_x = (WIN_W - total_w) * 0.5f;
    float start_y = 65.0f;

    int idx = 0;
    for (int r = 0; r < BRICK_ROWS; r++) {
        for (int c = 0; c < BRICK_COLS; c++) {
            brick_t *br = &b->bricks[idx++];
            br->x = start_x + c * (BRICK_W + BRICK_GAP);
            br->y = start_y + r * (BRICK_H + BRICK_GAP);
            br->alive = true;
            br->color = ROW_COLORS[r];
            br->points = (BRICK_ROWS - r) * 10;
        }
    }
}

static void new_game(breakout_t *b)
{
    b->score = 0;
    b->lives = 3;
    particles_clear(&b->particles);
    reset_level(b);
}

/* ── Scene: Title / Menu ──────────────────────────────────────────────────── */

static void title_on_render(game_t *g)
{
    game_clear(g, 0xFF181825u); /* Mantle */

    /* Title banner */
    uk_draw_text_2x(&g->win, (WIN_W - 200) / 2, 130, "BREAKOUT", 0xFFCBA6F7u);
    uk_draw_text(&g->win, (WIN_W - 260) / 2, 185, "AzamiOS Game Framework Demo", 0xFF9399B2u);

    /* Decorative bricks */
    for (int i = 0; i < 5; i++) {
        int bx = (WIN_W - 5 * 50) / 2 + i * 50;
        uk_fill_rounded_rect(&g->win, bx, 230, 42, 14, 3, ROW_COLORS[i]);
    }

    /* Start prompt */
    int blink = (int)(g->total_time * 2.5f) % 2;
    if (blink == 0) {
        uk_draw_text_bold(&g->win, (WIN_W - 220) / 2, 310, "Press SPACE or Click to Play", 0xFFCDD6F4u);
    }

    /* Controls */
    uk_draw_text_small(&g->win, (WIN_W - 240) / 2, 400, "Controls: Arrow Keys / A-D / Mouse to move", 0xFF6C7086u);
    uk_draw_text_small(&g->win, (WIN_W - 220) / 2, 420, "SPACE to launch ball  |  P to pause", 0xFF6C7086u);
}

static void title_on_update(game_t *g, float dt)
{
    (void)dt;
    if (input_key_pressed(&g->input, ' ') ||
        input_key_pressed(&g->input, 0x0D /* Enter */) ||
        input_mouse_pressed(&g->input, 0)) {
        new_game(&g_state);
        scene_switch_with(&g->scenes, "play", SCENE_TRANS_FADE_BLACK, 0.35f);
    }
}

/* ── Scene: Playing ───────────────────────────────────────────────────────── */

static void play_on_update(game_t *g, float dt)
{
    breakout_t *b = &g_state;

    /* Pause toggle */
    if (input_key_pressed(&g->input, 'p') || input_key_pressed(&g->input, 'P')) {
        g->paused = !g->paused;
    }
    if (g->paused) return;

    /* Paddle movement */
    float move = 0.0f;
    if (input_key_down(&g->input, 'a') || input_key_down(&g->input, 'A') ||
        input_key_down(&g->input, 0x4B /* Left */)) {
        move -= 1.0f;
    }
    if (input_key_down(&g->input, 'd') || input_key_down(&g->input, 'D') ||
        input_key_down(&g->input, 0x4D /* Right */)) {
        move += 1.0f;
    }

    b->paddle_x += move * PADDLE_SPEED * dt;

    /* Mouse paddle control */
    if (g->input.mouse_x > 0) {
        float target_x = (float)g->input.mouse_x - PADDLE_W * 0.5f;
        b->paddle_x = game_lerpf(b->paddle_x, target_x, game_clampf(18.0f * dt, 0.0f, 1.0f));
    }

    /* Clamp paddle to screen */
    if (b->paddle_x < 10.0f) b->paddle_x = 10.0f;
    if (b->paddle_x > WIN_W - PADDLE_W - 10.0f) b->paddle_x = WIN_W - PADDLE_W - 10.0f;

    /* Ball attached to paddle */
    if (b->ball_attached) {
        b->ball_x = b->paddle_x + PADDLE_W * 0.5f;
        b->ball_y = b->paddle_y - BALL_RADIUS - 1.0f;

        if (input_key_pressed(&g->input, ' ') || input_mouse_pressed(&g->input, 0)) {
            b->ball_attached = false;
            b->ball_vx = (game_rng_float(&b->rng) - 0.5f) * 200.0f;
            b->ball_vy = -BALL_SPEED;
            if (g->audio.initialized && b->sfx_paddle >= 0) {
                audio_play_sfx(&g->audio, b->sfx_paddle, 80, false);
            }
        }
    } else {
        /* Ball motion */
        b->ball_x += b->ball_vx * dt;
        b->ball_y += b->ball_vy * dt;

        /* Wall collisions */
        if (b->ball_x - BALL_RADIUS <= 10.0f) {
            b->ball_x = 10.0f + BALL_RADIUS;
            b->ball_vx = -b->ball_vx;
            if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_wall, 60, false);
        } else if (b->ball_x + BALL_RADIUS >= WIN_W - 10.0f) {
            b->ball_x = WIN_W - 10.0f - BALL_RADIUS;
            b->ball_vx = -b->ball_vx;
            if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_wall, 60, false);
        }

        if (b->ball_y - BALL_RADIUS <= 35.0f) {
            b->ball_y = 35.0f + BALL_RADIUS;
            b->ball_vy = -b->ball_vy;
            if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_wall, 60, false);
        }

        /* Bottom death */
        if (b->ball_y - BALL_RADIUS > WIN_H) {
            b->lives--;
            camera_shake(&g->camera, 8.0f, 0.35f);
            if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_lose, 90, false);

            if (b->lives <= 0) {
                if (b->score > b->high_score) b->high_score = b->score;
                scene_switch_with(&g->scenes, "gameover", SCENE_TRANS_FADE_BLACK, 0.5f);
                return;
            } else {
                b->ball_attached = true;
            }
        }

        /* Paddle collision */
        if (b->ball_vy > 0.0f &&
            b->ball_y + BALL_RADIUS >= b->paddle_y &&
            b->ball_y - BALL_RADIUS <= b->paddle_y + PADDLE_H &&
            b->ball_x >= b->paddle_x - BALL_RADIUS &&
            b->ball_x <= b->paddle_x + PADDLE_W + BALL_RADIUS) {

            b->ball_y = b->paddle_y - BALL_RADIUS - 1.0f;
            /* Angle based on impact position (-1..+1) */
            float hit_offset = (b->ball_x - (b->paddle_x + PADDLE_W * 0.5f)) / (PADDLE_W * 0.5f);
            hit_offset = game_clampf(hit_offset, -0.9f, 0.9f);

            float speed = sqrtf(b->ball_vx * b->ball_vx + b->ball_vy * b->ball_vy);
            if (speed < BALL_SPEED) speed = BALL_SPEED;

            b->ball_vx = hit_offset * speed;
            b->ball_vy = -sqrtf(speed * speed - b->ball_vx * b->ball_vx);

            if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_paddle, 85, false);
        }

        /* Brick collision */
        for (int i = 0; i < BRICK_ROWS * BRICK_COLS; i++) {
            brick_t *br = &b->bricks[i];
            if (!br->alive) continue;

            /* AABB vs Circle */
            float closest_x = game_clampf(b->ball_x, br->x, br->x + BRICK_W);
            float closest_y = game_clampf(b->ball_y, br->y, br->y + BRICK_H);
            float dist_x = b->ball_x - closest_x;
            float dist_y = b->ball_y - closest_y;
            float dist2 = dist_x * dist_x + dist_y * dist_y;

            if (dist2 <= (float)(BALL_RADIUS * BALL_RADIUS)) {
                br->alive = false;
                b->bricks_left--;
                b->score += br->points;

                /* Bounce direction */
                if (fabsf(dist_x) > fabsf(dist_y)) {
                    b->ball_vx = -b->ball_vx;
                } else {
                    b->ball_vy = -b->ball_vy;
                }

                /* FX */
                camera_shake(&g->camera, 4.0f, 0.15f);
                particles_emit_burst(&b->particles, &b->rng,
                                     closest_x, closest_y, 16,
                                     120.0f, 0.45f, 3.5f,
                                     br->color);

                if (g->audio.initialized) {
                    audio_play_sfx(&g->audio, b->sfx_brick, 95, false);
                }

                /* Victory check */
                if (b->bricks_left <= 0) {
                    if (b->score > b->high_score) b->high_score = b->score;
                    if (g->audio.initialized) audio_play_sfx(&g->audio, b->sfx_win, 100, false);
                    scene_switch_with(&g->scenes, "victory", SCENE_TRANS_FADE_WHITE, 0.5f);
                    return;
                }
                break;
            }
        }
    }

    /* Update particles */
    particles_update(&b->particles, dt);
}

static void play_on_render(game_t *g)
{
    breakout_t *b = &g_state;
    game_clear(g, 0xFF1E1E2Eu); /* Base background */

    /* Top HUD banner */
    uk_fill_rect(&g->win, 0, 0, WIN_W, 32, 0xFF181825u);

    char score_str[32];
    snprintf(score_str, sizeof(score_str), "SCORE: %d", b->score);
    uk_draw_text_bold(&g->win, 16, 8, score_str, 0xFFF9E2AFu);

    char lives_str[32];
    snprintf(lives_str, sizeof(lives_str), "LIVES: %d", b->lives);
    uk_draw_text_bold(&g->win, WIN_W - 100, 8, lives_str, 0xFFF38BA8u);

    char hi_str[32];
    snprintf(hi_str, sizeof(hi_str), "HIGH: %d", b->high_score);
    uk_draw_text(&g->win, (WIN_W - 100) / 2, 8, hi_str, 0xFF9399B2u);

    /* Draw Bricks */
    for (int i = 0; i < BRICK_ROWS * BRICK_COLS; i++) {
        brick_t *br = &b->bricks[i];
        if (!br->alive) continue;
        uk_fill_rounded_rect(&g->win, (int)br->x, (int)br->y,
                             BRICK_W, BRICK_H, 3, br->color);
        /* Subtle highlight border */
        uk_fill_rect(&g->win, (int)br->x + 2, (int)br->y + 1,
                     BRICK_W - 4, 2, 0x40FFFFFFu);
    }

    /* Draw Paddle */
    uk_fill_rounded_rect(&g->win, (int)b->paddle_x, (int)b->paddle_y,
                         PADDLE_W, PADDLE_H, 4, 0xFF89B4FAu);
    uk_fill_rect(&g->win, (int)b->paddle_x + 4, (int)b->paddle_y + 2,
                 PADDLE_W - 8, 2, 0x80FFFFFFu);

    /* Draw Ball */
    uk_fill_circle(&g->win, (int)b->ball_x, (int)b->ball_y,
                   BALL_RADIUS, 0xFFFFFFFFu);

    /* Draw Particles */
    game_draw_particles(g, &b->particles);

    /* Pause Overlay */
    if (g->paused) {
        game_draw_overlay(g, 0xFF000000u, 140);
        uk_draw_text_2x(&g->win, (WIN_W - 120) / 2, WIN_H / 2 - 20, "PAUSED", 0xFFCDD6F4u);
        uk_draw_text(&g->win, (WIN_W - 160) / 2, WIN_H / 2 + 25, "Press P to Resume", 0xFFA6ADC8u);
    }
}

/* ── Scene: GameOver ──────────────────────────────────────────────────────── */

static void gameover_on_render(game_t *g)
{
    game_clear(g, 0xFF11111Bu); /* Crust */

    uk_draw_text_2x(&g->win, (WIN_W - 180) / 2, 140, "GAME OVER", 0xFFF38BA8u);

    char score_buf[48];
    snprintf(score_buf, sizeof(score_buf), "Final Score: %d", g_state.score);
    uk_draw_text_bold(&g->win, (WIN_W - 140) / 2, 210, score_buf, 0xFFCDD6F4u);

    int blink = (int)(g->total_time * 2.5f) % 2;
    if (blink == 0) {
        uk_draw_text(&g->win, (WIN_W - 220) / 2, 280, "Press SPACE to Try Again", 0xFFA6ADC8u);
    }
}

static void gameover_on_update(game_t *g, float dt)
{
    (void)dt;
    if (input_key_pressed(&g->input, ' ') || input_mouse_pressed(&g->input, 0)) {
        new_game(&g_state);
        scene_switch_with(&g->scenes, "play", SCENE_TRANS_FADE_BLACK, 0.35f);
    }
}

/* ── Scene: Victory ───────────────────────────────────────────────────────── */

static void victory_on_render(game_t *g)
{
    game_clear(g, 0xFF181825u);

    uk_draw_text_2x(&g->win, (WIN_W - 240) / 2, 130, "VICTORY!", 0xFFA6E3A1u);
    uk_draw_text(&g->win, (WIN_W - 260) / 2, 185, "You cleared all the bricks!", 0xFFCDD6F4u);

    char score_buf[48];
    snprintf(score_buf, sizeof(score_buf), "Final Score: %d", g_state.score);
    uk_draw_text_bold(&g->win, (WIN_W - 140) / 2, 230, score_buf, 0xFFF9E2AFu);

    int blink = (int)(g->total_time * 2.5f) % 2;
    if (blink == 0) {
        uk_draw_text(&g->win, (WIN_W - 220) / 2, 300, "Press SPACE to Play Again", 0xFFA6ADC8u);
    }
}

static void victory_on_update(game_t *g, float dt)
{
    (void)dt;
    if (input_key_pressed(&g->input, ' ') || input_mouse_pressed(&g->input, 0)) {
        new_game(&g_state);
        scene_switch_with(&g->scenes, "play", SCENE_TRANS_FADE_BLACK, 0.35f);
    }
}

/* ── Init Callback ────────────────────────────────────────────────────────── */

static void on_init(game_t *g)
{
    breakout_t *b = &g_state;
    memset(b, 0, sizeof(*b));

    game_rng_seed(&b->rng, 0x12345678u);
    particles_init(&b->particles);
    b->particles.gravity_y = 200.0f;
    b->particles.damping = 0.5f;

    /* Register procedural SFX */
    if (g->audio.initialized) {
        b->sfx_paddle = audio_generate_tone(&g->audio, 440, 45, WAVE_SQUARE, 80);
        b->sfx_brick  = audio_generate_tone(&g->audio, 880, 50, WAVE_TRIANGLE, 90);
        b->sfx_wall   = audio_generate_tone(&g->audio, 220, 30, WAVE_SQUARE, 60);
        b->sfx_lose   = audio_generate_tone(&g->audio, 150, 250, WAVE_SAWTOOTH, 95);
        b->sfx_win    = audio_generate_tone(&g->audio, 1100, 300, WAVE_SINE, 100);
    }

    /* Register Scenes */
    scene_t title_scene = {
        .on_render = title_on_render,
        .on_update = title_on_update
    };
    scene_register(&g->scenes, "title", &title_scene);

    scene_t play_scene = {
        .on_render = play_on_render,
        .on_update = play_on_update
    };
    scene_register(&g->scenes, "play", &play_scene);

    scene_t gameover_scene = {
        .on_render = gameover_on_render,
        .on_update = gameover_on_update
    };
    scene_register(&g->scenes, "gameover", &gameover_scene);

    scene_t victory_scene = {
        .on_render = victory_on_render,
        .on_update = victory_on_update
    };
    scene_register(&g->scenes, "victory", &victory_scene);

    /* Start at title scene */
    scene_switch(&g->scenes, "title");
}

/* ── Main Entry ───────────────────────────────────────────────────────────── */

int main(void)
{
    game_config_t cfg = {
        .title = "Breakout",
        .width = WIN_W,
        .height = WIN_H,
        .target_fps = 60,
        .enable_audio = true
    };

    game_callbacks_t cb = {
        .on_init = on_init
    };

    return game_run(&cfg, &cb);
}
