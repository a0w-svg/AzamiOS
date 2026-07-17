/**
 * pong.c — AzamiOS Arcade Pong Game Service for Window Manager
 *
 * Demonstrates the 2D Game Framework (sprite rendering, bounding box collision,
 * physics update) integrated directly into the secure window manager.
 */

#include "../wm.h"
#include "../../../../lib/gfx/game_engine.h"

static sprite_t g_ball;
static sprite_t g_player;
static sprite_t g_ai;
static int g_player_score = 0;
static int g_ai_score = 0;
static bool g_game_started = false;

static void reset_ball(window_t *w) {
    if (!w) return;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;
    g_ball.w = 8;
    g_ball.h = 8;
    g_ball.x = bw / 2 - 4;
    g_ball.y = bh / 2 - 4;
    g_ball.vx = (g_player_score + g_ai_score) % 2 == 0 ? 3 : -3;
    g_ball.vy = 2;
    g_ball.color = 0x00FBBF24; /* Amber ball */
    g_ball.active = true;
}

static void pong_init_game(window_t *w) {
    if (!w) return;
    int bh = w->h - TITLEBAR_H - 1;
    g_player.w = 10;
    g_player.h = 40;
    g_player.x = 16;
    g_player.y = bh / 2 - 20;
    g_player.vy = 0;
    g_player.color = 0x0038BDF8; /* Cyan player paddle */
    g_player.active = true;

    g_ai.w = 10;
    g_ai.h = 40;
    g_ai.x = w->w - 28;
    g_ai.y = bh / 2 - 20;
    g_ai.vy = 0;
    g_ai.color = 0x00EF4444; /* Red AI paddle */
    g_ai.active = true;

    reset_ball(w);
    g_game_started = true;
}

static void pong_on_open(window_t *w) {
    pong_init_game(w);
}

static void pong_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)blink;
    if (!w) return;
    int bx = w->x + 1;
    int by = w->y + TITLEBAR_H;
    int bw = w->w - 2;
    int bh = w->h - TITLEBAR_H - 1;
    if (bw <= 0 || bh <= 0) return;

    if (!g_game_started) pong_init_game(w);

    /* 1. Clear background */
    draw_rect(bx, by, bw, bh, 0x000F172A);

    /* 2. Draw center net */
    for (int y = 0; y < bh; y += 16) {
        draw_rect(bx + bw / 2 - 1, by + y, 2, 8, 0x00334155);
    }

    /* 3. Physics & AI Updates (every other frame for smooth gameplay) */
    if (frame_cnt % 2 == 0) {
        /* Move ball */
        g_ball.x += g_ball.vx;
        g_ball.y += g_ball.vy;

        /* Top/Bottom bounce */
        if (g_ball.y <= 0) {
            g_ball.y = 0;
            g_ball.vy = -g_ball.vy;
        } else if (g_ball.y + g_ball.h >= bh) {
            g_ball.y = bh - g_ball.h;
            g_ball.vy = -g_ball.vy;
        }

        /* AI tracking */
        int ai_center = g_ai.y + g_ai.h / 2;
        if (ai_center < g_ball.y && g_ai.y + g_ai.h < bh - 4) g_ai.y += 3;
        else if (ai_center > g_ball.y + g_ball.h && g_ai.y > 4) g_ai.y -= 3;

        /* Check paddle collisions using Game Engine API */
        if (game_check_collision(&g_ball, &g_player)) {
            g_ball.x = g_player.x + g_player.w;
            g_ball.vx = -g_ball.vx;
            if (g_ball.vx < 8) g_ball.vx++;
        } else if (game_check_collision(&g_ball, &g_ai)) {
            g_ball.x = g_ai.x - g_ball.w;
            g_ball.vx = -g_ball.vx;
            if (g_ball.vx > -8) g_ball.vx--;
        }

        /* Scoring */
        if (g_ball.x < 0) {
            g_ai_score++;
            reset_ball(w);
        } else if (g_ball.x + g_ball.w > bw) {
            g_player_score++;
            reset_ball(w);
        }
    }

    /* Keep paddles in bounds */
    game_clamp_sprite(&g_player, 0, 0, bw, bh);
    game_clamp_sprite(&g_ai, 0, 0, bw, bh);
    g_ai.x = bw - 18;

    /* 4. Draw sprites */
    draw_rect(bx + g_player.x, by + g_player.y, g_player.w, g_player.h, g_player.color);
    draw_rect(bx + g_ai.x, by + g_ai.y, g_ai.w, g_ai.h, g_ai.color);
    draw_rect(bx + g_ball.x, by + g_ball.y, g_ball.w, g_ball.h, g_ball.color);

    /* 5. Draw scoreboard */
    char score_str[32];
    sprintf(score_str, "Player: %d   AI: %d", g_player_score, g_ai_score);
    draw_text(bx + bw / 2 - 50, by + 12, score_str, COL_TEXT_WHITE, 0x000F172A);
    draw_text(bx + 10, by + bh - 20, "Controls: W / S keys to move", COL_TEXT_GRAY, 0x000F172A);
}

static void pong_on_key(window_t *w, int char_code, rtc_time_t *t, uint32_t frame_cnt) {
    (void)t; (void)frame_cnt;
    if (!w) return;
    int bh = w->h - TITLEBAR_H - 1;

    if (char_code == 'w' || char_code == 'W' || char_code == '8') {
        g_player.y -= 14;
    } else if (char_code == 's' || char_code == 'S' || char_code == '2') {
        g_player.y += 14;
    }
    game_clamp_sprite(&g_player, 0, 0, w->w - 2, bh);
}

void pong_service_init(void) {
    static const wm_service_t pong_srv = {
        WIN_PONG,
        "Arcade Pong Game",
        WM_SRV_FLAG_ANIMATED,
        NULL,
        pong_on_open,
        NULL,
        pong_render,
        pong_on_key
    };
    wm_register_service(&pong_srv);
}
