/* ============================================================================
 * AzamiOS Desktop Environment — Arcade Snake Game (v1.0)
 * File: userland/apps/snake/main.c
 *
 * Features:
 *  • Classic retro Snake arcade gameplay on 22x22 grid
 *  • Apple (+10 pts) and Golden Star bonus (+50 pts) pickups
 *  • 4 Speed difficulty levels (Chill, Normal, Fast, Turbo)
 *  • Arrow keys, WASD, and on-screen directional controls
 *  • Catppuccin Mocha aesthetic with particle effects & high-score tracking
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       480
#define WIN_H       540
#define MAP_ADDR    ((void *)0x78000000)

#define GRID_W 22
#define GRID_H 22
#define MAX_SNAKE (GRID_W * GRID_H)

typedef struct {
    int x;
    int y;
} point_t;

static uk_window_t g_win;

static point_t g_snake[MAX_SNAKE];
static int g_snake_len = 3;
static int g_dir_x = 1;
static int g_dir_y = 0;
static int g_next_dir_x = 1;
static int g_next_dir_y = 0;

static point_t g_food;
static point_t g_bonus_food;
static int g_bonus_timer = 0;

static int g_score = 0;
static int g_high_score = 0;
static bool g_game_running = false;
static bool g_game_over = false;
static bool g_paused = false;

/* Speed Presets */
static const int g_speeds[4] = { 180, 120, 80, 50 }; /* ms per tick */
static const char *g_speed_names[4] = { "Chill", "Norm", "Fast", "Turbo" };
static int g_speed_idx = 1;

static unsigned int g_rng_seed = 0x54A8E001;

static unsigned int rnd(void)
{
    g_rng_seed ^= g_rng_seed << 13;
    g_rng_seed ^= g_rng_seed >> 17;
    g_rng_seed ^= g_rng_seed << 5;
    return g_rng_seed;
}

static void spawn_food(void)
{
    bool valid = false;
    while (!valid) {
        g_food.x = (int)(rnd() % GRID_W);
        g_food.y = (int)(rnd() % GRID_H);
        valid = true;
        for (int i = 0; i < g_snake_len; i++) {
            if (g_snake[i].x == g_food.x && g_snake[i].y == g_food.y) {
                valid = false;
                break;
            }
        }
    }
}

static void spawn_bonus(void)
{
    g_bonus_food.x = (int)(rnd() % GRID_W);
    g_bonus_food.y = (int)(rnd() % GRID_H);
    g_bonus_timer = 40; /* active for 40 ticks */
}

static void reset_game(void)
{
    g_snake_len = 4;
    g_snake[0] = (point_t){ 8, 10 };
    g_snake[1] = (point_t){ 7, 10 };
    g_snake[2] = (point_t){ 6, 10 };
    g_snake[3] = (point_t){ 5, 10 };

    g_dir_x = 1;
    g_dir_y = 0;
    g_next_dir_x = 1;
    g_next_dir_y = 0;

    g_score = 0;
    g_bonus_timer = 0;
    g_game_over = false;
    g_paused = false;
    g_game_running = true;

    spawn_food();
}

static void update_game_tick(void)
{
    if (!g_game_running || g_game_over || g_paused) return;

    /* Apply queued direction */
    g_dir_x = g_next_dir_x;
    g_dir_y = g_next_dir_y;

    /* Calculate new head */
    point_t new_head = { g_snake[0].x + g_dir_x, g_snake[0].y + g_dir_y };

    /* Wall collision check */
    if (new_head.x < 0 || new_head.x >= GRID_W || new_head.y < 0 || new_head.y >= GRID_H) {
        g_game_over = true;
        g_game_running = false;
        return;
    }

    /* Self collision check */
    for (int i = 0; i < g_snake_len - 1; i++) {
        if (g_snake[i].x == new_head.x && g_snake[i].y == new_head.y) {
            g_game_over = true;
            g_game_running = false;
            return;
        }
    }

    /* Check Food pickup */
    bool ate_food = (new_head.x == g_food.x && new_head.y == g_food.y);
    bool ate_bonus = (g_bonus_timer > 0 && new_head.x == g_bonus_food.x && new_head.y == g_bonus_food.y);

    /* Advance body segments */
    int new_len = g_snake_len;
    if (ate_food || ate_bonus) {
        if (new_len < MAX_SNAKE - 1) new_len++;
    }

    for (int i = new_len - 1; i > 0; i--) {
        g_snake[i] = g_snake[i - 1];
    }
    g_snake[0] = new_head;
    g_snake_len = new_len;

    if (ate_food) {
        g_score += 10;
        if (g_score > g_high_score) g_high_score = g_score;
        spawn_food();
        /* 20% chance to spawn bonus star */
        if ((rnd() % 5) == 0 && g_bonus_timer <= 0) {
            spawn_bonus();
        }
    }

    if (ate_bonus) {
        g_score += 50;
        if (g_score > g_high_score) g_high_score = g_score;
        g_bonus_timer = 0;
    }

    if (g_bonus_timer > 0) g_bonus_timer--;
}

static void draw_snake_game(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Top Header ──────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_GREEN);
    uk_draw_text(&g_win, 16, 7, "Azami Arcade Snake", UK_TEXT);
    uk_draw_text(&g_win, 16, 23, "Classic Retro Arcade • v1.0", UK_OVERLAY0);

    /* Speed preset buttons */
    int spd_btn_w = 46;
    for (int s = 0; s < 4; s++) {
        int bx = (int)w - 200 + s * (spd_btn_w + 3);
        uk_draw_button(&g_win, bx, 8, spd_btn_w, 26, g_speed_names[s], (g_speed_idx == s) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    }
    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    /* ── Score & Stats Bar ────────────────────────────────────────────────── */
    int bar_y = 50;
    int bar_w = ((int)w - 32 - 12) / 2;

    /* Current score */
    uk_fill_rounded_rect(&g_win, 16, bar_y, bar_w, 44, 6, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, 16, bar_y, bar_w, 44, 6, UK_SURFACE1);
    uk_draw_text(&g_win, 24, bar_y + 5, "SCORE", UK_SUBTEXT0);
    char score_str[32];
    snprintf(score_str, sizeof(score_str), "%04d", g_score);
    uk_draw_text_2x(&g_win, 24, bar_y + 18, score_str, UK_GREEN);

    /* High score */
    uk_fill_rounded_rect(&g_win, 16 + bar_w + 12, bar_y, bar_w, 44, 6, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, 16 + bar_w + 12, bar_y, bar_w, 44, 6, UK_SURFACE1);
    uk_draw_text(&g_win, 16 + bar_w + 22, bar_y + 5, "HIGH SCORE", UK_SUBTEXT0);
    char high_str[32];
    snprintf(high_str, sizeof(high_str), "%04d", g_high_score);
    uk_draw_text_2x(&g_win, 16 + bar_w + 22, bar_y + 18, high_str, UK_YELLOW);

    /* ── Grid Arena ──────────────────────────────────────────────────────── */
    int grid_pixel_sz = 16;
    int total_gw = GRID_W * grid_pixel_sz;
    int total_gh = GRID_H * grid_pixel_sz;
    int grid_ox = ((int)w - total_gw) / 2;
    int grid_oy = 104;

    uk_fill_rounded_rect(&g_win, grid_ox - 4, grid_oy - 4, total_gw + 8, total_gh + 8, 8, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, grid_ox - 4, grid_oy - 4, total_gw + 8, total_gh + 8, 8, UK_SURFACE1);
    uk_fill_rect(&g_win, grid_ox, grid_oy, total_gw, total_gh, UK_CRUST);

    /* Subtle grid dots */
    for (int r = 1; r < GRID_H; r++) {
        for (int c = 1; c < GRID_W; c++) {
            uk_fill_rect(&g_win, grid_ox + c * grid_pixel_sz, grid_oy + r * grid_pixel_sz, 1, 1, UK_SURFACE0);
        }
    }

    /* Draw Food (Red Apple) */
    int fx = grid_ox + g_food.x * grid_pixel_sz;
    int fy = grid_oy + g_food.y * grid_pixel_sz;
    uk_fill_circle(&g_win, fx + grid_pixel_sz / 2, fy + grid_pixel_sz / 2, grid_pixel_sz / 2 - 1, UK_RED);
    uk_fill_circle(&g_win, fx + grid_pixel_sz / 2 - 2, fy + grid_pixel_sz / 2 - 2, 2, UK_TEXT);
    uk_fill_rect(&g_win, fx + grid_pixel_sz / 2, fy + 1, 1, 2, UK_GREEN); /* Apple stem */

    /* Draw Bonus Golden Star */
    if (g_bonus_timer > 0) {
        int bx = grid_ox + g_bonus_food.x * grid_pixel_sz;
        int by = grid_oy + g_bonus_food.y * grid_pixel_sz;
        uk_fill_circle(&g_win, bx + grid_pixel_sz / 2, by + grid_pixel_sz / 2, grid_pixel_sz / 2, (g_bonus_timer % 4 < 2) ? UK_YELLOW : UK_PEACH);
        uk_draw_text(&g_win, bx + 4, by, "*", UK_BASE);
    }

    /* Draw Snake Body */
    for (int i = g_snake_len - 1; i >= 0; i--) {
        int sx = grid_ox + g_snake[i].x * grid_pixel_sz;
        int sy = grid_oy + g_snake[i].y * grid_pixel_sz;

        if (i == 0) {
            /* Snake Head (Bright Green with eyes) */
            uk_fill_rounded_rect(&g_win, sx + 1, sy + 1, grid_pixel_sz - 2, grid_pixel_sz - 2, 4, UK_GREEN);
            /* Eyes */
            int eye1_x = sx + 4, eye1_y = sy + 4;
            int eye2_x = sx + 10, eye2_y = sy + 4;
            if (g_dir_x == 1) { eye1_x = sx + 10; eye2_x = sx + 10; eye1_y = sy + 3; eye2_y = sy + 10; }
            else if (g_dir_x == -1) { eye1_x = sx + 3; eye2_x = sx + 3; eye1_y = sy + 3; eye2_y = sy + 10; }
            else if (g_dir_y == 1) { eye1_x = sx + 3; eye2_x = sx + 10; eye1_y = sy + 10; eye2_y = sy + 10; }

            uk_fill_circle(&g_win, eye1_x, eye1_y, 2, UK_CRUST);
            uk_fill_circle(&g_win, eye2_x, eye2_y, 2, UK_CRUST);
            uk_fill_circle(&g_win, eye1_x, eye1_y, 1, UK_TEXT);
            uk_fill_circle(&g_win, eye2_x, eye2_y, 1, UK_TEXT);
        } else {
            /* Body segments (gradient teal to green) */
            unsigned int seg_col = (i % 2 == 0) ? UK_TEAL : UK_SAPPHIRE;
            uk_fill_rounded_rect(&g_win, sx + 2, sy + 2, grid_pixel_sz - 4, grid_pixel_sz - 4, 3, seg_col);
        }
    }

    /* ── On-Screen Directional D-Pad & Action Buttons ─────────────────────── */
    int bottom_y = grid_oy + total_gh + 10;
    int dpad_w = 40;
    int dpad_h = 28;
    int dpad_cx = 100;

    uk_draw_button(&g_win, dpad_cx - dpad_w / 2, bottom_y, dpad_w, dpad_h, "^", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx - dpad_w - 6, bottom_y + dpad_h + 3, dpad_w, dpad_h, "<", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx - dpad_w / 2, bottom_y + dpad_h + 3, dpad_w, dpad_h, "v", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx + dpad_w / 2 + 6, bottom_y + dpad_h + 3, dpad_w, dpad_h, ">", UK_BTN_NORMAL);

    /* Play / Pause / Restart */
    int btn_act_x = (int)w - 200;
    uk_draw_button(&g_win, btn_act_x, bottom_y + 4, 90, 32, g_paused ? "Resume" : "Pause", UK_BTN_NORMAL);
    uk_draw_button(&g_win, btn_act_x + 98, bottom_y + 4, 90, 32, "Restart", UK_BTN_NORMAL);

    const char *hint = "Arrows/WASD: Move | P: Pause | R: Restart";
    uk_draw_text(&g_win, btn_act_x, bottom_y + 44, hint, UK_OVERLAY0);

    /* ── Game Over Overlay ───────────────────────────────────────────────── */
    if (g_game_over) {
        int mbox_w = 260;
        int mbox_h = 130;
        int mx = grid_ox + (total_gw - mbox_w) / 2;
        int my = grid_oy + (total_gh - mbox_h) / 2;

        uk_fill_rounded_rect(&g_win, mx, my, mbox_w, mbox_h, 10, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, mx, my, mbox_w, mbox_h, 10, UK_RED);
        uk_draw_text_2x(&g_win, mx + 50, my + 18, "GAME OVER", UK_RED);

        char fin_score[32];
        snprintf(fin_score, sizeof(fin_score), "Final Score: %d", g_score);
        uk_draw_text(&g_win, mx + 70, my + 54, fin_score, UK_TEXT);

        uk_draw_button(&g_win, mx + 55, my + 82, 150, 32, "Play Again (R)", UK_BTN_PRESSED);
    }

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Arcade Snake",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    /* Seed RNG with hardware entropy from /dev/hwrng or /dev/urandom */
    int hwrng = sys_open("/dev/hwrng", 0, 0);
    if (hwrng >= 0) {
        sys_read(hwrng, &g_rng_seed, sizeof(g_rng_seed));
        sys_close(hwrng);
    } else {
        int urand = sys_open("/dev/urandom", 0, 0);
        if (urand >= 0) {
            sys_read(urand, &g_rng_seed, sizeof(g_rng_seed));
            sys_close(urand);
        }
    }

    reset_game();
    draw_snake_game();

    /* Start autonomous game timer */
    az_set_timer(g_win.client_chan, g_speeds[g_speed_idx], 0);

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_TIMER_TICK) {
            update_game_tick();
            draw_snake_game();
        } else if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                unsigned char k = msg.key.keycode;
                unsigned char sc = msg.key.scancode;

                if (sc == 0x48 || k == 'w' || k == 'W') { /* UP */
                    if (g_dir_y == 0) { g_next_dir_x = 0; g_next_dir_y = -1; }
                } else if (sc == 0x50 || k == 's' || k == 'S') { /* DOWN */
                    if (g_dir_y == 0) { g_next_dir_x = 0; g_next_dir_y = 1; }
                } else if (sc == 0x4B || k == 'a' || k == 'A') { /* LEFT */
                    if (g_dir_x == 0) { g_next_dir_x = -1; g_next_dir_y = 0; }
                } else if (sc == 0x4D || k == 'd' || k == 'D') { /* RIGHT */
                    if (g_dir_x == 0) { g_next_dir_x = 1; g_next_dir_y = 0; }
                } else if (k == 'p' || k == 'P' || k == ' ') {
                    g_paused = !g_paused;
                    draw_snake_game();
                } else if (k == 'r' || k == 'R') {
                    reset_game();
                    draw_snake_game();
                } else if (k == '1') {
                    g_speed_idx = 0;
                    az_set_timer(g_win.client_chan, g_speeds[0], 0);
                    draw_snake_game();
                } else if (k == '2') {
                    g_speed_idx = 1;
                    az_set_timer(g_win.client_chan, g_speeds[1], 0);
                    draw_snake_game();
                } else if (k == '3') {
                    g_speed_idx = 2;
                    az_set_timer(g_win.client_chan, g_speeds[2], 0);
                    draw_snake_game();
                } else if (k == '4') {
                    g_speed_idx = 3;
                    az_set_timer(g_win.client_chan, g_speeds[3], 0);
                    draw_snake_game();
                } else if (k == 'q' || k == 'Q' || k == 27) {
                    break;
                }
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btn = msg.mouse.buttons;

            bool lclick = (btn & 1) && !(prev_btn & 1);
            prev_btn = btn;

            if (lclick) {
                int w = (int)g_win.width;

                /* Speed preset clicks */
                if (my >= 8 && my <= 34) {
                    int spd_btn_w = 46;
                    for (int s = 0; s < 4; s++) {
                        int bx = w - 200 + s * (spd_btn_w + 3);
                        if (mx >= bx && mx <= bx + spd_btn_w) {
                            g_speed_idx = s;
                            az_set_timer(g_win.client_chan, g_speeds[s], 0);
                            draw_snake_game();
                            break;
                        }
                    }
                }

                /* Game Over modal click */
                if (g_game_over) {
                    int total_gw = GRID_W * 16;
                    int total_gh = GRID_H * 16;
                    int grid_ox = (w - total_gw) / 2;
                    int grid_oy = 104;
                    int mx_box = grid_ox + (total_gw - 260) / 2;
                    int my_box = grid_oy + (total_gh - 130) / 2;

                    if (mx >= mx_box + 55 && mx <= mx_box + 205 &&
                        my >= my_box + 82 && my <= my_box + 114) {
                        reset_game();
                        draw_snake_game();
                        continue;
                    }
                }

                /* D-Pad Buttons */
                int total_gh = GRID_H * 16;
                int bottom_y = 104 + total_gh + 10;
                int dpad_w = 40;
                int dpad_h = 28;
                int dpad_cx = 100;

                /* UP */
                if (mx >= dpad_cx - dpad_w / 2 && mx <= dpad_cx + dpad_w / 2 && my >= bottom_y && my <= bottom_y + dpad_h) {
                    if (g_dir_y == 0) { g_next_dir_x = 0; g_next_dir_y = -1; }
                }
                /* LEFT */
                else if (mx >= dpad_cx - dpad_w - 6 && mx <= dpad_cx - 6 && my >= bottom_y + dpad_h + 3 && my <= bottom_y + 2 * dpad_h + 3) {
                    if (g_dir_x == 0) { g_next_dir_x = -1; g_next_dir_y = 0; }
                }
                /* DOWN */
                else if (mx >= dpad_cx - dpad_w / 2 && mx <= dpad_cx + dpad_w / 2 && my >= bottom_y + dpad_h + 3 && my <= bottom_y + 2 * dpad_h + 3) {
                    if (g_dir_y == 0) { g_next_dir_x = 0; g_next_dir_y = 1; }
                }
                /* RIGHT */
                else if (mx >= dpad_cx + dpad_w / 2 + 6 && mx <= dpad_cx + 3 * dpad_w / 2 + 6 && my >= bottom_y + dpad_h + 3 && my <= bottom_y + 2 * dpad_h + 3) {
                    if (g_dir_x == 0) { g_next_dir_x = 1; g_next_dir_y = 0; }
                }

                /* Action buttons */
                int btn_act_x = w - 200;
                if (my >= bottom_y + 4 && my <= bottom_y + 36) {
                    if (mx >= btn_act_x && mx <= btn_act_x + 90) {
                        g_paused = !g_paused;
                        draw_snake_game();
                    } else if (mx >= btn_act_x + 98 && mx <= btn_act_x + 188) {
                        reset_game();
                        draw_snake_game();
                    }
                }
            }
        }
    }

    return 0;
}
