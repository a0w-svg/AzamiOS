/**
 * minesweeper.c — Classic Minesweeper Service for AzamiOS
 */
#include "../wm.h"

#define M_W 14
#define M_H 10
#define CELL_S 20

typedef struct {
    bool mine;
    bool revealed;
    bool flagged;
    int  neighbor_mines;
} cell_t;

static cell_t s_grid[M_H][M_W];
static int s_cursor_x = 0, s_cursor_y = 0;
static bool s_game_over = false;
static bool s_win = false;

static void reset_board(void) {
    for (int y = 0; y < M_H; y++) {
        for (int x = 0; x < M_W; x++) {
            s_grid[y][x].mine = false;
            s_grid[y][x].revealed = false;
            s_grid[y][x].flagged = false;
            s_grid[y][x].neighbor_mines = 0;
        }
    }
    /* Place ~15 mines */
    int placed = 0;
    while (placed < 15) {
        int rx = rand() % M_W;
        int ry = rand() % M_H;
        if (!s_grid[ry][rx].mine && (rx != 0 || ry != 0)) {
            s_grid[ry][rx].mine = true;
            placed++;
        }
    }
    /* Calculate neighbors */
    for (int y = 0; y < M_H; y++) {
        for (int x = 0; x < M_W; x++) {
            if (s_grid[y][x].mine) continue;
            int count = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < M_W && ny >= 0 && ny < M_H && s_grid[ny][nx].mine) {
                        count++;
                    }
                }
            }
            s_grid[y][x].neighbor_mines = count;
        }
    }
    s_cursor_x = 0; s_cursor_y = 0;
    s_game_over = false;
    s_win = false;
}

static void reveal_cell(int x, int y) {
    if (x < 0 || x >= M_W || y < 0 || y >= M_H) return;
    if (s_grid[y][x].revealed || s_grid[y][x].flagged) return;
    s_grid[y][x].revealed = true;
    if (s_grid[y][x].mine) {
        s_game_over = true;
        return;
    }
    if (s_grid[y][x].neighbor_mines == 0) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                reveal_cell(x + dx, y + dy);
            }
        }
    }
}

static void check_win(void) {
    for (int y = 0; y < M_H; y++) {
        for (int x = 0; x < M_W; x++) {
            if (!s_grid[y][x].mine && !s_grid[y][x].revealed) return;
        }
    }
    s_win = true;
    s_game_over = true;
}

static void mines_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, M_W * CELL_S + 20, M_H * CELL_S + TITLEBAR_H + 40);
    reset_board();
}

static void mines_on_open(window_t *w) {
    mines_on_init(w);
}

static void mines_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == 'r' || c == 'R') { reset_board(); return; }
    if (s_game_over) return;

    if (c == 'w' || c == 'W') { if (s_cursor_y > 0) s_cursor_y--; }
    else if (c == 's' || c == 'S') { if (s_cursor_y < M_H - 1) s_cursor_y++; }
    else if (c == 'a' || c == 'A') { if (s_cursor_x > 0) s_cursor_x--; }
    else if (c == 'd' || c == 'D') { if (s_cursor_x < M_W - 1) s_cursor_x++; }
    else if (c == ' ') {
        reveal_cell(s_cursor_x, s_cursor_y);
        if (!s_game_over) check_win();
    } else if (c == 'f' || c == 'F') {
        if (!s_grid[s_cursor_y][s_cursor_x].revealed) {
            s_grid[s_cursor_y][s_cursor_x].flagged = !s_grid[s_cursor_y][s_cursor_x].flagged;
        }
    }
}

static void mines_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)frame_cnt; (void)blink;
    if (!w) return;
    int bx = w->x + 10;
    int by = w->y + TITLEBAR_H + 10;

    for (int y = 0; y < M_H; y++) {
        for (int x = 0; x < M_W; x++) {
            int cx = bx + x * CELL_S;
            int cy = by + y * CELL_S;
            cell_t *cell = &s_grid[y][x];

            if (cell->revealed) {
                draw_rect(cx, cy, CELL_S - 1, CELL_S - 1, 0x00E2E8F0);
                if (cell->mine) {
                    draw_rect(cx + 4, cy + 4, CELL_S - 9, CELL_S - 9, COL_TEXT_RED);
                } else if (cell->neighbor_mines > 0) {
                    char n[2] = { (char)('0' + cell->neighbor_mines), '\0' };
                    uint32_t col = (cell->neighbor_mines == 1) ? COL_TEXT_BLUE :
                                   ((cell->neighbor_mines == 2) ? COL_TEXT_GREEN : COL_TEXT_RED);
                    draw_text(cx + 6, cy + 4, n, col, 0x00E2E8F0);
                }
            } else {
                draw_rect(cx, cy, CELL_S - 1, CELL_S - 1, 0x0064748B);
                if (cell->flagged) {
                    draw_rect(cx + 6, cy + 6, CELL_S - 13, CELL_S - 13, COL_TEXT_YELLOW);
                }
            }
            if (x == s_cursor_x && y == s_cursor_y) {
                draw_rect(cx, cy, CELL_S - 1, 2, COL_TEXT_CYAN);
                draw_rect(cx, cy + CELL_S - 3, CELL_S - 1, 2, COL_TEXT_CYAN);
                draw_rect(cx, cy, 2, CELL_S - 1, COL_TEXT_CYAN);
                draw_rect(cx + CELL_S - 3, cy, 2, CELL_S - 1, COL_TEXT_CYAN);
            }
        }
    }

    draw_text(bx, by + M_H * CELL_S + 6, "WASD: Move | SPACE: Reveal | F: Flag | R: Restart", COL_TEXT_DARK, COL_WIN_BODY);
    if (s_win) draw_text(bx + 40, by + M_H * CELL_S / 2, "YOU WIN! Press R", COL_TEXT_GREEN, 0x00E2E8F0);
    else if (s_game_over) draw_text(bx + 40, by + M_H * CELL_S / 2, "BOOM! GAME OVER! Press R", COL_TEXT_RED, 0x00E2E8F0);
}

void minesweeper_service_init(void) {
    static const wm_service_t mines_srv = {
        WIN_MINESWEEPER, "Minesweeper", WM_SRV_FLAG_GAME,
        mines_on_init, mines_on_open, NULL, mines_on_render, mines_on_key
    };
    wm_register_service(&mines_srv);
}
