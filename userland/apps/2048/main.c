/* ============================================================================
 * AzamiOS Desktop Environment — 2048 Sliding Puzzle Game (v1.0)
 * File: userland/apps/2048/main.c
 *
 * Features:
 *  • Classic 4x4 2048 gameplay with smooth tile animations
 *  • Keyboard controls (Arrow keys / WASD / HJKL) + on-screen buttons
 *  • Score & High-score tracking with Undo move feature
 *  • Catppuccin Mocha aesthetic with glowing colored tiles
 *  • Game Over & 2048 Victory celebration modal
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
#define WIN_W       460
#define WIN_H       530
#define MAP_ADDR    ((void *)0x77000000)

#define GRID_SIZE 4

static uk_window_t g_win;

static int g_board[GRID_SIZE][GRID_SIZE];
static int g_prev_board[GRID_SIZE][GRID_SIZE];
static int g_score = 0;
static int g_prev_score = 0;
static int g_high_score = 0;
static bool g_can_undo = false;
static bool g_game_over = false;
static bool g_game_won = false;
static bool g_keep_playing = false;
static unsigned int g_rng_seed = 0x2048FEED;

static unsigned int rnd(void)
{
    g_rng_seed ^= g_rng_seed << 13;
    g_rng_seed ^= g_rng_seed >> 17;
    g_rng_seed ^= g_rng_seed << 5;
    return g_rng_seed;
}

static void spawn_random_tile(void)
{
    int empty_r[16];
    int empty_c[16];
    int empty_count = 0;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g_board[r][c] == 0) {
                empty_r[empty_count] = r;
                empty_c[empty_count] = c;
                empty_count++;
            }
        }
    }

    if (empty_count == 0) return;

    int idx = (int)(rnd() % (unsigned int)empty_count);
    int r = empty_r[idx];
    int c = empty_c[idx];
    /* 90% chance of 2, 10% chance of 4 */
    g_board[r][c] = ((rnd() % 10) == 0) ? 4 : 2;
}

static void reset_game(void)
{
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            g_board[r][c] = 0;
            g_prev_board[r][c] = 0;
        }
    }
    g_score = 0;
    g_prev_score = 0;
    g_can_undo = false;
    g_game_over = false;
    g_game_won = false;
    g_keep_playing = false;

    spawn_random_tile();
    spawn_random_tile();
}

static void undo_move(void)
{
    if (!g_can_undo) return;
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            g_board[r][c] = g_prev_board[r][c];
        }
    }
    g_score = g_prev_score;
    g_can_undo = false;
    g_game_over = false;
}

static bool check_moves_available(void)
{
    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            if (g_board[r][c] == 0) return true;
            if (r + 1 < GRID_SIZE && g_board[r][c] == g_board[r + 1][c]) return true;
            if (c + 1 < GRID_SIZE && g_board[r][c] == g_board[r][c + 1]) return true;
        }
    }
    return false;
}

/* Slide and merge a single 4-element line to the left */
static bool slide_line(int line[GRID_SIZE], int *score_gain)
{
    int temp[GRID_SIZE] = {0};
    int t = 0;

    /* Step 1: Shift non-zeros to front */
    for (int i = 0; i < GRID_SIZE; i++) {
        if (line[i] != 0) {
            temp[t++] = line[i];
        }
    }

    /* Step 2: Merge adjacent equals */
    for (int i = 0; i < GRID_SIZE - 1; i++) {
        if (temp[i] != 0 && temp[i] == temp[i + 1]) {
            temp[i] *= 2;
            *score_gain += temp[i];
            if (temp[i] == 2048 && !g_keep_playing) {
                g_game_won = true;
            }
            temp[i + 1] = 0;
            i++;
        }
    }

    /* Step 3: Shift again to eliminate zeros created by merge */
    int final_line[GRID_SIZE] = {0};
    int f = 0;
    for (int i = 0; i < GRID_SIZE; i++) {
        if (temp[i] != 0) {
            final_line[f++] = temp[i];
        }
    }

    bool changed = false;
    for (int i = 0; i < GRID_SIZE; i++) {
        if (line[i] != final_line[i]) {
            changed = true;
            line[i] = final_line[i];
        }
    }
    return changed;
}

#define DIR_UP    0
#define DIR_DOWN  1
#define DIR_LEFT  2
#define DIR_RIGHT 3

static bool move_board(int dir)
{
    if (g_game_over) return false;
    /* BUG-22 fix: block moves while the win overlay is displayed */
    if (g_game_won && !g_keep_playing) return false;

    int score_gain = 0;
    bool moved = false;

    int backup[GRID_SIZE][GRID_SIZE];
    for (int r = 0; r < GRID_SIZE; r++)
        for (int c = 0; c < GRID_SIZE; c++)
            backup[r][c] = g_board[r][c];

    if (dir == DIR_LEFT) {
        for (int r = 0; r < GRID_SIZE; r++) {
            int line[GRID_SIZE];
            for (int c = 0; c < GRID_SIZE; c++) line[c] = g_board[r][c];
            if (slide_line(line, &score_gain)) moved = true;
            for (int c = 0; c < GRID_SIZE; c++) g_board[r][c] = line[c];
        }
    } else if (dir == DIR_RIGHT) {
        for (int r = 0; r < GRID_SIZE; r++) {
            int line[GRID_SIZE];
            for (int c = 0; c < GRID_SIZE; c++) line[c] = g_board[r][GRID_SIZE - 1 - c];
            if (slide_line(line, &score_gain)) moved = true;
            for (int c = 0; c < GRID_SIZE; c++) g_board[r][GRID_SIZE - 1 - c] = line[c];
        }
    } else if (dir == DIR_UP) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int line[GRID_SIZE];
            for (int r = 0; r < GRID_SIZE; r++) line[r] = g_board[r][c];
            if (slide_line(line, &score_gain)) moved = true;
            for (int r = 0; r < GRID_SIZE; r++) g_board[r][c] = line[r];
        }
    } else if (dir == DIR_DOWN) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int line[GRID_SIZE];
            for (int r = 0; r < GRID_SIZE; r++) line[r] = g_board[GRID_SIZE - 1 - r][c];
            if (slide_line(line, &score_gain)) moved = true;
            for (int r = 0; r < GRID_SIZE; r++) g_board[GRID_SIZE - 1 - r][c] = line[r];
        }
    }

    if (moved) {
        /* Store previous board for Undo */
        for (int r = 0; r < GRID_SIZE; r++)
            for (int c = 0; c < GRID_SIZE; c++)
                g_prev_board[r][c] = backup[r][c];
        g_prev_score = g_score;
        g_can_undo = true;

        g_score += score_gain;
        if (g_score > g_high_score) g_high_score = g_score;

        spawn_random_tile();

        if (!check_moves_available()) {
            g_game_over = true;
        }
    }

    return moved;
}

/* ── Tile Color Palette ─────────────────────────────────────────────────────── */
typedef struct {
    int val;
    unsigned int bg;
    unsigned int fg;
} tile_color_t;

static const tile_color_t g_tile_palette[] = {
    { 0,    0xFF313244, 0xFF6C7086 }, /* Empty: Surface0 */
    { 2,    0xFF45475A, 0xFFCDD6F4 }, /* 2: Surface1 */
    { 4,    0xFF585B70, 0xFFCDD6F4 }, /* 4: Surface2 */
    { 8,    0xFFFAB387, 0xFF1E1E2E }, /* 8: Peach */
    { 16,   0xFFF9E2AF, 0xFF1E1E2E }, /* 16: Yellow */
    { 32,   0xFFF38BA8, 0xFF1E1E2E }, /* 32: Red */
    { 64,   0xFFEBA0AC, 0xFF1E1E2E }, /* 64: Maroon */
    { 128,  0xFFA6E3A1, 0xFF1E1E2E }, /* 128: Green */
    { 256,  0xFF94E2D5, 0xFF1E1E2E }, /* 256: Teal */
    { 512,  0xFF89DCEB, 0xFF1E1E2E }, /* 512: Sky */
    { 1024, 0xFF89B4FA, 0xFF1E1E2E }, /* 1024: Blue */
    { 2048, 0xFFCBA6F7, 0xFF1E1E2E }, /* 2048: Mauve */
    { 4096, 0xFFF5C2E7, 0xFF1E1E2E }, /* 4096: Pink */
    { 8192, 0xFFB4BEFE, 0xFF1E1E2E }, /* 8192: Lavender */
};
#define NUM_PALETTE ((int)(sizeof(g_tile_palette)/sizeof(g_tile_palette[0])))

static tile_color_t get_tile_color(int val)
{
    for (int i = 0; i < NUM_PALETTE; i++) {
        if (g_tile_palette[i].val == val) return g_tile_palette[i];
    }
    tile_color_t def = { val, 0xFFF5C2E7, 0xFF1E1E2E };
    return def;
}

static void draw_2048(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Header ──────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_MAUVE);
    uk_draw_text(&g_win, 16, 7, "Azami 2048", UK_TEXT);
    uk_draw_text(&g_win, 16, 23, "Sliding Number Puzzle • v1.0", UK_OVERLAY0);

    /* Header action buttons */
    uk_draw_button(&g_win, (int)w - 170, 8, 76, 26, "Undo", g_can_undo ? UK_BTN_NORMAL : UK_BTN_PRESSED);
    uk_draw_button(&g_win, (int)w - 86,  8, 76, 26, "New",  UK_BTN_NORMAL);
    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    /* ── Score & High-score Cards ────────────────────────────────────────── */
    int card_y = 52;
    int card_w = ((int)w - 32 - 12) / 2;

    /* Score card */
    uk_fill_rounded_rect(&g_win, 16, card_y, card_w, 48, 8, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, 16, card_y, card_w, 48, 8, UK_SURFACE1);
    uk_draw_text(&g_win, 26, card_y + 6, "SCORE", UK_SUBTEXT0);
    char score_str[32];
    snprintf(score_str, sizeof(score_str), "%d", g_score);
    uk_draw_text_2x(&g_win, 26, card_y + 20, score_str, UK_GREEN);

    /* Best score card */
    uk_fill_rounded_rect(&g_win, 16 + card_w + 12, card_y, card_w, 48, 8, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, 16 + card_w + 12, card_y, card_w, 48, 8, UK_SURFACE1);
    uk_draw_text(&g_win, 16 + card_w + 22, card_y + 6, "BEST", UK_SUBTEXT0);
    char best_str[32];
    snprintf(best_str, sizeof(best_str), "%d", g_high_score);
    uk_draw_text_2x(&g_win, 16 + card_w + 22, card_y + 20, best_str, UK_PEACH);

    /* ── Board Outer Well ────────────────────────────────────────────────── */
    int board_size = (int)w - 32;
    if (board_size > 360) board_size = 360;
    int board_ox = ((int)w - board_size) / 2;
    int board_oy = 110;

    uk_fill_rounded_rect(&g_win, board_ox, board_oy, board_size, board_size, 10, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, board_ox, board_oy, board_size, board_size, 10, UK_SURFACE1);

    int gap = 10;
    int cell_size = (board_size - gap * (GRID_SIZE + 1)) / GRID_SIZE;

    for (int r = 0; r < GRID_SIZE; r++) {
        for (int c = 0; c < GRID_SIZE; c++) {
            int cx = board_ox + gap + c * (cell_size + gap);
            int cy = board_oy + gap + r * (cell_size + gap);
            int val = g_board[r][c];

            tile_color_t tc = get_tile_color(val);
            uk_fill_rounded_rect(&g_win, cx, cy, cell_size, cell_size, 6, tc.bg);

            if (val > 0) {
                char vstr[16];
                snprintf(vstr, sizeof(vstr), "%d", val);
                int len = (int)strlen(vstr);

                if (len <= 2) {
                    int tx = cx + (cell_size - len * 16) / 2;
                    int ty = cy + (cell_size - 16) / 2;
                    uk_draw_text_2x(&g_win, tx, ty, vstr, tc.fg);
                } else {
                    int tx = cx + (cell_size - len * 8) / 2;
                    int ty = cy + (cell_size - 16) / 2;
                    uk_draw_text(&g_win, tx, ty, vstr, tc.fg);
                }
            }
        }
    }

    /* ── On-Screen Directional D-Pad Controls ─────────────────────────────── */
    int dpad_y = board_oy + board_size + 12;
    int dpad_w = 44;
    int dpad_h = 30;
    int dpad_cx = (int)w / 2;

    uk_draw_button(&g_win, dpad_cx - dpad_w / 2, dpad_y, dpad_w, dpad_h, "^", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx - dpad_w - 8, dpad_y + dpad_h + 4, dpad_w, dpad_h, "<", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx - dpad_w / 2, dpad_y + dpad_h + 4, dpad_w, dpad_h, "v", UK_BTN_NORMAL);
    uk_draw_button(&g_win, dpad_cx + dpad_w / 2 + 8, dpad_y + dpad_h + 4, dpad_w, dpad_h, ">", UK_BTN_NORMAL);

    /* ── Game Over / Won Overlay ─────────────────────────────────────────── */
    if (g_game_won && !g_keep_playing) {
        uk_fill_rounded_rect(&g_win, board_ox + 20, board_oy + 80, board_size - 40, 160, 12, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, board_ox + 20, board_oy + 80, board_size - 40, 160, 12, UK_MAUVE);
        uk_draw_text_2x(&g_win, board_ox + 70, board_oy + 105, "YOU WON!", UK_GREEN);
        uk_draw_text(&g_win, board_ox + 60, board_oy + 145, "Reached the 2048 tile!", UK_TEXT);
        uk_draw_button(&g_win, board_ox + 40, board_oy + 180, 110, 30, "Continue", UK_BTN_NORMAL);
        uk_draw_button(&g_win, board_ox + 160, board_oy + 180, 110, 30, "New Game", UK_BTN_NORMAL);
    } else if (g_game_over) {
        uk_fill_rounded_rect(&g_win, board_ox + 20, board_oy + 80, board_size - 40, 160, 12, UK_CRUST);
        uk_draw_rounded_rect_outline(&g_win, board_ox + 20, board_oy + 80, board_size - 40, 160, 12, UK_RED);
        uk_draw_text_2x(&g_win, board_ox + 60, board_oy + 105, "GAME OVER", UK_RED);
        uk_draw_text(&g_win, board_ox + 55, board_oy + 145, "No moves remaining!", UK_TEXT);
        uk_draw_button(&g_win, board_ox + 80, board_oy + 180, 150, 32, "Try Again", UK_BTN_PRESSED);
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

    int ret = uk_window_connect(&g_win, "2048 Puzzle",
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
    /* BUG-21 fix: XOR-shift RNG with seed==0 stays at 0 forever.
     * If both RNG sources failed or returned zero, restore the
     * compile-time constant so the game is at least playable. */
    if (g_rng_seed == 0) {
        g_rng_seed = 0x2048FEED;
    }

    reset_game();
    draw_2048();

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                unsigned char k = msg.key.keycode;
                unsigned char sc = msg.key.scancode;

                /* Arrow keys or WASD or HJKL */
                if (sc == 0x48 || k == 'w' || k == 'W' || k == 'k') { /* UP */
                    if (move_board(DIR_UP)) draw_2048();
                } else if (sc == 0x50 || k == 's' || k == 'S' || k == 'j') { /* DOWN */
                    if (move_board(DIR_DOWN)) draw_2048();
                } else if (sc == 0x4B || k == 'a' || k == 'A' || k == 'h') { /* LEFT */
                    if (move_board(DIR_LEFT)) draw_2048();
                } else if (sc == 0x4D || k == 'd' || k == 'D' || k == 'l') { /* RIGHT */
                    if (move_board(DIR_RIGHT)) draw_2048();
                } else if (k == 'u' || k == 'U' || k == 0x08 /* Backspace */) {
                    undo_move();
                    draw_2048();
                } else if (k == 'r' || k == 'R' || k == 'n' || k == 'N') {
                    reset_game();
                    draw_2048();
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

                /* Header buttons */
                if (my >= 8 && my <= 34) {
                    if (mx >= w - 170 && mx <= w - 94) {
                        undo_move();
                        draw_2048();
                        continue;
                    } else if (mx >= w - 86 && mx <= w - 10) {
                        reset_game();
                        draw_2048();
                        continue;
                    }
                }

                int board_size = w - 32;
                if (board_size > 360) board_size = 360;
                int board_ox = (w - board_size) / 2;
                int board_oy = 110;

                /* Modal dialog clicks */
                if (g_game_won && !g_keep_playing) {
                    if (my >= board_oy + 180 && my <= board_oy + 210) {
                        if (mx >= board_ox + 40 && mx <= board_ox + 150) {
                            g_keep_playing = true;
                            draw_2048();
                            continue;
                        } else if (mx >= board_ox + 160 && mx <= board_ox + 270) {
                            reset_game();
                            draw_2048();
                            continue;
                        }
                    }
                } else if (g_game_over) {
                    if (my >= board_oy + 180 && my <= board_oy + 212 &&
                        mx >= board_ox + 80 && mx <= board_ox + 230) {
                        reset_game();
                        draw_2048();
                        continue;
                    }
                }

                /* D-Pad Buttons */
                int dpad_y = board_oy + board_size + 12;
                int dpad_w = 44;
                int dpad_h = 30;
                int dpad_cx = w / 2;

                /* UP */
                if (mx >= dpad_cx - dpad_w / 2 && mx <= dpad_cx + dpad_w / 2 && my >= dpad_y && my <= dpad_y + dpad_h) {
                    if (move_board(DIR_UP)) draw_2048();
                }
                /* LEFT */
                else if (mx >= dpad_cx - dpad_w - 8 && mx <= dpad_cx - 8 && my >= dpad_y + dpad_h + 4 && my <= dpad_y + 2 * dpad_h + 4) {
                    if (move_board(DIR_LEFT)) draw_2048();
                }
                /* DOWN */
                else if (mx >= dpad_cx - dpad_w / 2 && mx <= dpad_cx + dpad_w / 2 && my >= dpad_y + dpad_h + 4 && my <= dpad_y + 2 * dpad_h + 4) {
                    if (move_board(DIR_DOWN)) draw_2048();
                }
                /* RIGHT */
                else if (mx >= dpad_cx + dpad_w / 2 + 8 && mx <= dpad_cx + 3 * dpad_w / 2 + 8 && my >= dpad_y + dpad_h + 4 && my <= dpad_y + 2 * dpad_h + 4) {
                    if (move_board(DIR_RIGHT)) draw_2048();
                }
            }
        }
    }

    return 0;
}
