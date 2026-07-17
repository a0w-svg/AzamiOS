/**
 * tetris.c — Arcade Tetris Service for AzamiOS
 */
#include "../wm.h"

#define T_W 10
#define T_H 20
#define CELL_S 16

static int s_grid[T_H][T_W];
static int s_piece[4][2];
static int s_px = 4, s_py = 0;
static int s_type = 1;
static int s_score = 0;
static bool s_game_over = false;

static const int SHAPES[7][4][2] = {
    {{0,1},{1,1},{2,1},{3,1}}, /* I */
    {{0,0},{0,1},{1,1},{2,1}}, /* J */
    {{2,0},{0,1},{1,1},{2,1}}, /* L */
    {{1,0},{2,0},{1,1},{2,1}}, /* O */
    {{1,0},{2,0},{0,1},{1,1}}, /* S */
    {{1,0},{0,1},{1,1},{2,1}}, /* T */
    {{0,0},{1,0},{1,1},{2,1}}  /* Z */
};

static const uint32_t COLORS[8] = {
    0x000F172A, 0x0006B6D4, 0x003B82F6, 0x00F97316,
    0x00EAB308, 0x0010B981, 0x00A855F7, 0x00EF4444
};

static bool can_move(int nx, int ny, int np[4][2]) {
    for (int i = 0; i < 4; i++) {
        int x = nx + np[i][0];
        int y = ny + np[i][1];
        if (x < 0 || x >= T_W || y >= T_H) return false;
        if (y >= 0 && s_grid[y][x] != 0) return false;
    }
    return true;
}

static void spawn_piece(void) {
    s_type = 1 + (rand() % 7);
    s_px = 3; s_py = 0;
    for (int i = 0; i < 4; i++) {
        s_piece[i][0] = SHAPES[s_type - 1][i][0];
        s_piece[i][1] = SHAPES[s_type - 1][i][1];
    }
    if (!can_move(s_px, s_py, s_piece)) {
        s_game_over = true;
    }
}

static void clear_lines(void) {
    for (int y = T_H - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < T_W; x++) {
            if (s_grid[y][x] == 0) { full = false; break; }
        }
        if (full) {
            s_score += 100;
            for (int yy = y; yy > 0; yy--) {
                for (int x = 0; x < T_W; x++) s_grid[yy][x] = s_grid[yy - 1][x];
            }
            for (int x = 0; x < T_W; x++) s_grid[0][x] = 0;
            y++;
        }
    }
}

static void lock_piece(void) {
    for (int i = 0; i < 4; i++) {
        int x = s_px + s_piece[i][0];
        int y = s_py + s_piece[i][1];
        if (y >= 0 && y < T_H && x >= 0 && x < T_W) {
            s_grid[y][x] = s_type;
        }
    }
    clear_lines();
    spawn_piece();
}

static void tetris_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, T_W * CELL_S + 140, T_H * CELL_S + TITLEBAR_H + 20);
    for (int y = 0; y < T_H; y++)
        for (int x = 0; x < T_W; x++) s_grid[y][x] = 0;
    s_score = 0;
    s_game_over = false;
    spawn_piece();
}

static void tetris_on_open(window_t *w) { tetris_on_init(w); }

static void tetris_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == 'r' || c == 'R') { tetris_on_init(w); return; }
    if (s_game_over) return;

    if (c == 'a' || c == 'A') {
        if (can_move(s_px - 1, s_py, s_piece)) s_px--;
    } else if (c == 'd' || c == 'D') {
        if (can_move(s_px + 1, s_py, s_piece)) s_px++;
    } else if (c == 's' || c == 'S') {
        if (can_move(s_px, s_py + 1, s_piece)) s_py++;
    } else if (c == 'w' || c == 'W') {
        /* Rotate */
        int np[4][2];
        for (int i = 0; i < 4; i++) {
            np[i][0] = -s_piece[i][1] + 2;
            np[i][1] = s_piece[i][0];
        }
        if (can_move(s_px, s_py, np)) {
            for (int i = 0; i < 4; i++) {
                s_piece[i][0] = np[i][0];
                s_piece[i][1] = np[i][1];
            }
        }
    }
}

static void tetris_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)blink;
    if (!w) return;
    int bx = w->x + 10;
    int by = w->y + TITLEBAR_H + 10;

    draw_rect(bx, by, T_W * CELL_S, T_H * CELL_S, 0x000F172A);
    draw_rect(bx - 1, by - 1, T_W * CELL_S + 2, T_H * CELL_S + 2, COL_TEXT_CYAN);

    if (!s_game_over && (frame_cnt % 15) == 0) {
        if (can_move(s_px, s_py + 1, s_piece)) {
            s_py++;
        } else {
            lock_piece();
        }
    }

    /* Draw grid */
    for (int y = 0; y < T_H; y++) {
        for (int x = 0; x < T_W; x++) {
            if (s_grid[y][x] != 0) {
                draw_rect(bx + x * CELL_S + 1, by + y * CELL_S + 1, CELL_S - 2, CELL_S - 2, COLORS[s_grid[y][x]]);
            }
        }
    }

    /* Draw active piece */
    if (!s_game_over) {
        for (int i = 0; i < 4; i++) {
            int x = s_px + s_piece[i][0];
            int y = s_py + s_piece[i][1];
            if (y >= 0 && y < T_H && x >= 0 && x < T_W) {
                draw_rect(bx + x * CELL_S + 1, by + y * CELL_S + 1, CELL_S - 2, CELL_S - 2, COLORS[s_type]);
            }
        }
    }

    /* Side info */
    int sx = bx + T_W * CELL_S + 16;
    draw_text(sx, by, "TETRIS v6.0", COL_TEXT_CYAN, COL_WIN_BODY);
    draw_text(sx, by + 30, "Score:", COL_TEXT_DARK, COL_WIN_BODY);

    char sbuf[32]; int p=0; int sc = s_score;
    if (sc == 0) sbuf[p++] = '0';
    else { char tmp[10]; int tp=0; while(sc>0){tmp[tp++]='0'+sc%10; sc/=10;} while(tp>0) sbuf[p++]=tmp[--tp]; }
    sbuf[p] = '\0';
    draw_text(sx, by + 46, sbuf, COL_TEXT_GREEN, COL_WIN_BODY);

    draw_text(sx, by + 90, "Controls:", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(sx, by + 106, "A/D: Move", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(sx, by + 122, "W: Rotate", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(sx, by + 138, "S: Drop", COL_TEXT_DARK, COL_WIN_BODY);
    draw_text(sx, by + 154, "R: Restart", COL_TEXT_DARK, COL_WIN_BODY);

    if (s_game_over) {
        draw_text(bx + 20, by + T_H * CELL_S / 2, "GAME OVER! Press R", COL_TEXT_RED, 0x000F172A);
    }
}

void tetris_service_init(void) {
    static const wm_service_t tetris_srv = {
        WIN_TETRIS, "Tetris", WM_SRV_FLAG_ANIMATED | WM_SRV_FLAG_GAME,
        tetris_on_init, tetris_on_open, NULL, tetris_on_render, tetris_on_key
    };
    wm_register_service(&tetris_srv);
}
