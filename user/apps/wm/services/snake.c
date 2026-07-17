/**
 * snake.c — Arcade Snake Game Service for AzamiOS
 */
#include "../wm.h"

#define GRID_W 26
#define GRID_H 18
#define CELL_S 10

typedef struct {
    int x, y;
} point_t;

static point_t s_snake[200];
static int s_len = 5;
static int s_dx = 1, s_dy = 0;
static point_t s_food = {10, 8};
static int s_score = 0;
static bool s_game_over = false;

static void spawn_food(void) {
    s_food.x = 1 + (rand() % (GRID_W - 2));
    s_food.y = 1 + (rand() % (GRID_H - 2));
}

static void snake_on_init(window_t *w) {
    if (!w) return;
    wm_resize_window(w, GRID_W * CELL_S + 20, GRID_H * CELL_S + TITLEBAR_H + 30);
    s_len = 5;
    for (int i = 0; i < s_len; i++) {
        s_snake[i].x = 10 - i;
        s_snake[i].y = 10;
    }
    s_dx = 1; s_dy = 0;
    s_score = 0;
    s_game_over = false;
    spawn_food();
}

static void snake_on_open(window_t *w) {
    snake_on_init(w);
}

static void snake_on_key(window_t *w, int c, rtc_time_t *t, uint32_t frame_cnt) {
    (void)w; (void)t; (void)frame_cnt;
    if (c == 'r' || c == 'R') {
        snake_on_init(w);
        return;
    }
    if (s_game_over) return;
    if ((c == 'w' || c == 'W') && s_dy == 0) { s_dx = 0; s_dy = -1; }
    else if ((c == 's' || c == 'S') && s_dy == 0) { s_dx = 0; s_dy = 1; }
    else if ((c == 'a' || c == 'A') && s_dx == 0) { s_dx = -1; s_dy = 0; }
    else if ((c == 'd' || c == 'D') && s_dx == 0) { s_dx = 1; s_dy = 0; }
}

static void snake_on_render(window_t *w, rtc_time_t *t, uint32_t frame_cnt, int blink) {
    (void)t; (void)blink;
    if (!w) return;
    int bx = w->x + 10;
    int by = w->y + TITLEBAR_H + 10;

    /* Background grid */
    draw_rect(bx, by, GRID_W * CELL_S, GRID_H * CELL_S, 0x000F172A);
    draw_rect(bx - 1, by - 1, GRID_W * CELL_S + 2, GRID_H * CELL_S + 2, COL_TEXT_CYAN);

    if (!s_game_over && (frame_cnt % 5) == 0) {
        int nx = s_snake[0].x + s_dx;
        int ny = s_snake[0].y + s_dy;
        if (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H) {
            s_game_over = true;
        } else {
            for (int i = 1; i < s_len; i++) {
                if (s_snake[i].x == nx && s_snake[i].y == ny) { s_game_over = true; break; }
            }
        }
        if (!s_game_over) {
            if (nx == s_food.x && ny == s_food.y) {
                if (s_len < 190) s_len++;
                s_score += 10;
                spawn_food();
            }
            for (int i = s_len - 1; i > 0; i--) {
                s_snake[i] = s_snake[i - 1];
            }
            s_snake[0].x = nx;
            s_snake[0].y = ny;
        }
    }

    /* Draw food */
    draw_rect(bx + s_food.x * CELL_S + 1, by + s_food.y * CELL_S + 1, CELL_S - 2, CELL_S - 2, COL_TEXT_RED);

    /* Draw snake */
    for (int i = 0; i < s_len; i++) {
        uint32_t col = (i == 0) ? COL_TEXT_WHITE : COL_TEXT_GREEN;
        draw_rect(bx + s_snake[i].x * CELL_S + 1, by + s_snake[i].y * CELL_S + 1, CELL_S - 2, CELL_S - 2, col);
    }

    /* Score and status */
    char sbuf[32];
    int p = 0; const char *pfx = "Score: ";
    while (*pfx) sbuf[p++] = *pfx++;
    int sc = s_score;
    if (sc == 0) sbuf[p++] = '0';
    else { char tmp[10]; int tp=0; while(sc>0){tmp[tp++]='0'+sc%10; sc/=10;} while(tp>0) sbuf[p++]=tmp[--tp]; }
    sbuf[p] = '\0';
    draw_text(bx, by + GRID_H * CELL_S + 6, sbuf, COL_TEXT_WHITE, COL_WIN_BODY);

    if (s_game_over) {
        draw_text(bx + 60, by + GRID_H * CELL_S / 2, "GAME OVER! Press R", COL_TEXT_RED, 0x000F172A);
    }
}

void snake_service_init(void) {
    static const wm_service_t snake_srv = {
        WIN_SNAKE, "Snake", WM_SRV_FLAG_ANIMATED | WM_SRV_FLAG_GAME,
        snake_on_init, snake_on_open, NULL, snake_on_render, snake_on_key
    };
    wm_register_service(&snake_srv);
}
