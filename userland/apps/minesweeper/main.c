/* ============================================================================
 * AzamiOS Desktop Environment — Minesweeper Game (v2.0)
 * File: userland/apps/minesweeper/main.c
 *
 * Features:
 *  • Classic Minesweeper gameplay with Easy (9x9), Medium (12x12), and Hard (16x16)
 *  • First-click safety (mines placed after first click to always open a blank space)
 *  • Flood-fill zero cell expansion algorithm
 *  • Full Chording support on numbered tiles (reveal safe neighbors when flagged count matches)
 *  • Debounced edge-triggered mouse input (prevents multi-firing on click/drag)
 *  • Keyboard shortcuts: 'R' restart, '1'/'2'/'3' difficulty, 'Q'/ESC quit
 *  • Interactive Smiley face status button (Playing, Scared, Won, Dead)
 *  • Live mine counter and digital elapsed-time stopwatch
 *  • Right-click flagging and 3D bevelled Catppuccin Mocha aesthetic
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include <stdbool.h>
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       480
#define WIN_H       530
#define MAP_ADDR    ((void *)0x6D000000)

#define MAX_GRID_SIZE 16

/* Cell states */
#define CELL_HIDDEN   0
#define CELL_REVEALED 1
#define CELL_FLAGGED  2
#define CELL_QUESTION 3

typedef struct {
    int is_mine;
    int adjacent_mines;
    int state;
} cell_t;

/* Difficulty presets */
typedef struct {
    const char *name;
    int cols;
    int rows;
    int mines;
} diff_t;

static const diff_t g_diffs[3] = {
    { "Easy",   9,  9, 10 },
    { "Medium", 12, 12, 24 },
    { "Hard",   16, 16, 40 }
};

static int g_current_diff = 0;
static int g_cols = 9;
static int g_rows = 9;
static int g_total_mines = 10;

static cell_t g_grid[MAX_GRID_SIZE][MAX_GRID_SIZE];
static bool g_game_started = false;
static bool g_game_over = false;
static bool g_game_won = false;
static int  g_flags_left = 10;
static int  g_time_seconds = 0;
static unsigned int g_rng_seed = 0x5EED1337;

static uk_window_t g_win;

static unsigned int rnd(void)
{
    g_rng_seed ^= g_rng_seed << 13;
    g_rng_seed ^= g_rng_seed >> 17;
    g_rng_seed ^= g_rng_seed << 5;
    return g_rng_seed;
}

static void count_adjacent_mines(void)
{
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            if (g_grid[r][c].is_mine) continue;
            int count = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr;
                    int nc = c + dc;
                    if (nr >= 0 && nr < g_rows && nc >= 0 && nc < g_cols) {
                        if (g_grid[nr][nc].is_mine) count++;
                    }
                }
            }
            g_grid[r][c].adjacent_mines = count;
        }
    }
}

static void place_mines_safe(int safe_r, int safe_c)
{
    int max_safe_mines = g_rows * g_cols - 9;
    if (max_safe_mines < 1) max_safe_mines = 1;
    if (g_total_mines > max_safe_mines) g_total_mines = max_safe_mines;

    int placed = 0;
    int attempts = 0;
    while (placed < g_total_mines && attempts < 10000) {
        attempts++;
        int r = (int)(rnd() % (unsigned int)g_rows);
        int c = (int)(rnd() % (unsigned int)g_cols);

        /* Avoid safe area around first click */
        if (abs(r - safe_r) <= 1 && abs(c - safe_c) <= 1) continue;
        if (g_grid[r][c].is_mine) continue;

        g_grid[r][c].is_mine = 1;
        placed++;
    }
    count_adjacent_mines();
}

static void reset_game(int diff)
{
    g_current_diff = diff;
    g_cols = g_diffs[diff].cols;
    g_rows = g_diffs[diff].rows;
    g_total_mines = g_diffs[diff].mines;
    g_flags_left = g_total_mines;
    g_time_seconds = 0;
    g_game_started = false;
    g_game_over = false;
    g_game_won = false;

    for (int r = 0; r < MAX_GRID_SIZE; r++) {
        for (int c = 0; c < MAX_GRID_SIZE; c++) {
            g_grid[r][c].is_mine = 0;
            g_grid[r][c].adjacent_mines = 0;
            g_grid[r][c].state = CELL_HIDDEN;
        }
    }
}

static void flood_reveal(int r, int c)
{
    if (r < 0 || r >= g_rows || c < 0 || c >= g_cols) return;
    if (g_grid[r][c].state == CELL_REVEALED || g_grid[r][c].state == CELL_FLAGGED) return;

    g_grid[r][c].state = CELL_REVEALED;

    if (g_grid[r][c].adjacent_mines == 0 && !g_grid[r][c].is_mine) {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                if (dr != 0 || dc != 0) {
                    flood_reveal(r + dr, c + dc);
                }
            }
        }
    }
}

static void check_win(void)
{
    if (g_game_over) return;
    int unrevealed_safe = 0;
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            if (!g_grid[r][c].is_mine && g_grid[r][c].state != CELL_REVEALED) {
                unrevealed_safe++;
            }
        }
    }
    if (unrevealed_safe == 0) {
        g_game_won = true;
        g_game_over = true;
        g_flags_left = 0;
    }
}

static void chord_reveal(int r, int c)
{
    if (r < 0 || r >= g_rows || c < 0 || c >= g_cols) return;
    if (g_grid[r][c].state != CELL_REVEALED || g_grid[r][c].adjacent_mines == 0) return;

    int flagged_count = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int nr = r + dr;
            int nc = c + dc;
            if (nr >= 0 && nr < g_rows && nc >= 0 && nc < g_cols) {
                if (g_grid[nr][nc].state == CELL_FLAGGED) flagged_count++;
            }
        }
    }

    if (flagged_count == g_grid[r][c].adjacent_mines) {
        for (int dr = -1; dr <= 1; dr++) {
            for (int dc = -1; dc <= 1; dc++) {
                int nr = r + dr;
                int nc = c + dc;
                if (nr >= 0 && nr < g_rows && nc >= 0 && nc < g_cols) {
                    if (g_grid[nr][nc].state == CELL_HIDDEN) {
                        if (g_grid[nr][nc].is_mine) {
                            /* Detonated mine on incorrect flag */
                            g_grid[nr][nc].state = CELL_REVEALED;
                            g_game_over = true;
                            for (int ar = 0; ar < g_rows; ar++)
                                for (int ac = 0; ac < g_cols; ac++)
                                    if (g_grid[ar][ac].is_mine) g_grid[ar][ac].state = CELL_REVEALED;
                        } else {
                            flood_reveal(nr, nc);
                        }
                    }
                }
            }
        }
        check_win();
    }
}

static const unsigned int g_number_colors[9] = {
    UK_TEXT,      /* 0 */
    UK_BLUE,      /* 1 */
    UK_GREEN,     /* 2 */
    UK_RED,       /* 3 */
    UK_MAUVE,     /* 4 */
    UK_MAROON,    /* 5 */
    UK_TEAL,      /* 6 */
    UK_LAVENDER,  /* 7 */
    UK_PEACH      /* 8 */
};

static void draw_minesweeper(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Header ──────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_MAUVE);
    uk_draw_text(&g_win, 16, 8, "Azami Minesweeper", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "Classic Logic Puzzle • v2.0", UK_OVERLAY0);

    /* Difficulty buttons */
    int btn_w = 60;
    uk_draw_button(&g_win, (int)w - 200, 8, btn_w, 26, "Easy", (g_current_diff == 0) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, (int)w - 134, 8, btn_w, 26, "Med",  (g_current_diff == 1) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, (int)w - 68,  8, btn_w, 26, "Hard", (g_current_diff == 2) ? UK_BTN_PRESSED : UK_BTN_NORMAL);

    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    /* ── Score & Status Bar ──────────────────────────────────────────────── */
    int bar_y = 50;
    int bar_w = (int)w - 32;
    uk_fill_rounded_rect(&g_win, 16, bar_y, bar_w, 46, 8, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, 16, bar_y, bar_w, 46, 8, UK_SURFACE1);

    /* Mines Counter Digital Box */
    uk_fill_rounded_rect(&g_win, 24, bar_y + 8, 70, 30, 4, UK_CRUST);
    char mine_str[16];
    snprintf(mine_str, sizeof(mine_str), "%03d", g_flags_left >= 0 ? g_flags_left : 0);
    uk_draw_text_2x(&g_win, 32, bar_y + 11, mine_str, UK_RED);

    /* Smiley Status Face Button */
    int face_x = (int)w / 2 - 20;
    int face_y = bar_y + 7;
    uk_fill_rounded_rect(&g_win, face_x, face_y, 40, 32, 6, UK_SURFACE0);
    uk_draw_rounded_rect_outline(&g_win, face_x, face_y, 40, 32, 6, UK_SURFACE2);

    const char *face = ":)";
    unsigned int face_col = UK_YELLOW;
    if (g_game_won) {
        face = "B)";
        face_col = UK_GREEN;
    } else if (g_game_over) {
        face = "X(";
        face_col = UK_RED;
    }
    uk_draw_text_2x(&g_win, face_x + 6, face_y + 4, face, face_col);

    /* Timer Digital Box */
    uk_fill_rounded_rect(&g_win, (int)w - 104, bar_y + 8, 70, 30, 4, UK_CRUST);
    char time_str[16];
    snprintf(time_str, sizeof(time_str), "%03d", g_time_seconds <= 999 ? g_time_seconds : 999);
    uk_draw_text_2x(&g_win, (int)w - 96, bar_y + 11, time_str, UK_GREEN);

    /* ── Minefield Grid ──────────────────────────────────────────────────── */
    int grid_area_w = (int)w - 32;
    int grid_area_h = (int)h - 110 - 32;
    int cell_size = (grid_area_w - 8) / g_cols;
    if ((grid_area_h - 8) / g_rows < cell_size) {
        cell_size = (grid_area_h - 8) / g_rows;
    }
    if (cell_size > 36) cell_size = 36;

    int total_grid_w = cell_size * g_cols;
    int total_grid_h = cell_size * g_rows;
    int grid_ox = ((int)w - total_grid_w) / 2;
    int grid_oy = 106 + ((int)h - 106 - 32 - total_grid_h) / 2;

    /* Grid outer well */
    uk_fill_rounded_rect(&g_win, grid_ox - 4, grid_oy - 4, total_grid_w + 8, total_grid_h + 8, 6, UK_MANTLE);
    uk_draw_rounded_rect_outline(&g_win, grid_ox - 4, grid_oy - 4, total_grid_w + 8, total_grid_h + 8, 6, UK_SURFACE1);

    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            int cx = grid_ox + c * cell_size;
            int cy = grid_oy + r * cell_size;
            int st = g_grid[r][c].state;

            if (st == CELL_REVEALED) {
                if (g_grid[r][c].is_mine) {
                    /* Detonated Mine */
                    uk_fill_rect(&g_win, cx, cy, cell_size - 1, cell_size - 1, UK_RED);
                    uk_draw_char(&g_win, cx + (cell_size - 8) / 2, cy + (cell_size - 16) / 2, '*', UK_BASE);
                } else {
                    /* Safe Numbered Cell */
                    uk_fill_rect(&g_win, cx, cy, cell_size - 1, cell_size - 1, UK_SURFACE0);
                    int n = g_grid[r][c].adjacent_mines;
                    if (n > 0) {
                        char digit = '0' + (char)n;
                        uk_draw_char(&g_win, cx + (cell_size - 8) / 2, cy + (cell_size - 16) / 2, digit, g_number_colors[n]);
                    }
                }
            } else {
                /* Hidden or Flagged 3D Bevelled Tile */
                uk_fill_rect(&g_win, cx, cy, cell_size - 1, cell_size - 1, UK_SURFACE1);
                /* Bevel top-left light */
                uk_fill_rect(&g_win, cx, cy, cell_size - 1, 2, UK_SURFACE2);
                uk_fill_rect(&g_win, cx, cy, 2, cell_size - 1, UK_SURFACE2);
                /* Bevel bottom-right shadow */
                uk_fill_rect(&g_win, cx, cy + cell_size - 3, cell_size - 1, 2, UK_CRUST);
                uk_fill_rect(&g_win, cx + cell_size - 3, cy, 2, cell_size - 1, UK_CRUST);

                if (st == CELL_FLAGGED) {
                    uk_draw_char(&g_win, cx + (cell_size - 8) / 2, cy + (cell_size - 16) / 2, 'F', UK_RED);
                }
            }
        }
    }

    /* ── Bottom Status Bar ───────────────────────────────────────────────── */
    int sby = (int)h - 24;
    uk_fill_rect(&g_win, 0, sby, (int)w, 24, UK_CRUST);
    uk_hline(&g_win, 0, sby, (int)w, UK_SURFACE1);

    const char *status_msg = "L-Click: Reveal | R-Click: Flag | Click Num: Chord | R: Restart";
    if (g_game_won) status_msg = "VICTORY! You cleared all mines safely! Press Face to play again.";
    else if (g_game_over) status_msg = "GAME OVER! Click the Face to restart.";
    uk_draw_text(&g_win, 12, sby + 4, status_msg, g_game_won ? UK_GREEN : (g_game_over ? UK_RED : UK_OVERLAY0));

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

    int ret = uk_window_connect(&g_win, "Minesweeper",
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

    reset_game(0);
    draw_minesweeper();

    /* 1 second autonomous timer */
    az_set_timer(g_win.client_chan, 1000, 0);

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_TIMER_TICK) {
            if (g_game_started && !g_game_over) {
                g_time_seconds++;
                draw_minesweeper();
            }
        } else if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                unsigned char k = msg.key.keycode;
                if (k == 'r' || k == 'R') {
                    reset_game(g_current_diff);
                    draw_minesweeper();
                } else if (k == '1') {
                    reset_game(0);
                    draw_minesweeper();
                } else if (k == '2') {
                    reset_game(1);
                    draw_minesweeper();
                } else if (k == '3') {
                    reset_game(2);
                    draw_minesweeper();
                } else if (k == 'q' || k == 'Q' || k == 27) {
                    break;
                }
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btn = msg.mouse.buttons;

            bool lclick = (btn & 1) && !(prev_btn & 1);
            bool rclick = (btn & 2) && !(prev_btn & 2);
            prev_btn = btn;

            if (lclick) {
                /* Header difficulty buttons */
                if (my >= 8 && my <= 34) {
                    int w = (int)g_win.width;
                    if (mx >= w - 200 && mx <= w - 140) { reset_game(0); draw_minesweeper(); continue; }
                    if (mx >= w - 134 && mx <= w - 74)  { reset_game(1); draw_minesweeper(); continue; }
                    if (mx >= w - 68 && mx <= w - 8)    { reset_game(2); draw_minesweeper(); continue; }
                }

                /* Smiley restart button */
                int face_x = (int)g_win.width / 2 - 20;
                if (mx >= face_x && mx <= face_x + 40 && my >= 57 && my <= 89) {
                    reset_game(g_current_diff);
                    draw_minesweeper();
                    continue;
                }

                /* Grid cell click */
                int grid_area_w = (int)g_win.width - 32;
                int grid_area_h = (int)g_win.height - 110 - 32;
                int cell_size = (grid_area_w - 8) / g_cols;
                if ((grid_area_h - 8) / g_rows < cell_size) cell_size = (grid_area_h - 8) / g_rows;
                if (cell_size > 36) cell_size = 36;

                int total_grid_w = cell_size * g_cols;
                int total_grid_h = cell_size * g_rows;
                int grid_ox = ((int)g_win.width - total_grid_w) / 2;
                int grid_oy = 106 + ((int)g_win.height - 106 - 32 - total_grid_h) / 2;

                if (mx >= grid_ox && mx < grid_ox + total_grid_w &&
                    my >= grid_oy && my < grid_oy + total_grid_h && !g_game_over) {
                    int c = (mx - grid_ox) / cell_size;
                    int r = (my - grid_oy) / cell_size;

                    if (!g_game_started) {
                        g_game_started = true;
                        place_mines_safe(r, c);
                    }

                    if (g_grid[r][c].state == CELL_HIDDEN) {
                        if (g_grid[r][c].is_mine) {
                            /* Hit a mine */
                            g_grid[r][c].state = CELL_REVEALED;
                            g_game_over = true;
                            /* Reveal all mines */
                            for (int ar = 0; ar < g_rows; ar++)
                                for (int ac = 0; ac < g_cols; ac++)
                                    if (g_grid[ar][ac].is_mine) g_grid[ar][ac].state = CELL_REVEALED;
                        } else {
                            flood_reveal(r, c);
                            check_win();
                        }
                    } else if (g_grid[r][c].state == CELL_REVEALED) {
                        /* Chord auto-clear on numbered tile */
                        chord_reveal(r, c);
                    }
                    draw_minesweeper();
                }
            } else if (rclick) {
                /* Right click: Flag */
                int grid_area_w = (int)g_win.width - 32;
                int grid_area_h = (int)g_win.height - 110 - 32;
                int cell_size = (grid_area_w - 8) / g_cols;
                if ((grid_area_h - 8) / g_rows < cell_size) cell_size = (grid_area_h - 8) / g_rows;
                if (cell_size > 36) cell_size = 36;

                int total_grid_w = cell_size * g_cols;
                int total_grid_h = cell_size * g_rows;
                int grid_ox = ((int)g_win.width - total_grid_w) / 2;
                int grid_oy = 106 + ((int)g_win.height - 106 - 32 - total_grid_h) / 2;

                if (mx >= grid_ox && mx < grid_ox + total_grid_w &&
                    my >= grid_oy && my < grid_oy + total_grid_h && !g_game_over) {
                    int c = (mx - grid_ox) / cell_size;
                    int r = (my - grid_oy) / cell_size;

                    if (g_grid[r][c].state == CELL_HIDDEN && g_flags_left > 0) {
                        g_grid[r][c].state = CELL_FLAGGED;
                        g_flags_left--;
                    } else if (g_grid[r][c].state == CELL_FLAGGED) {
                        g_grid[r][c].state = CELL_HIDDEN;
                        g_flags_left++;
                    }
                    draw_minesweeper();
                }
            }
        }
    }

    return 0;
}

