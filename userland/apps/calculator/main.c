/* ============================================================================
 * AzamiOS — Calculator (v3.0)
 * File: userland/apps/calculator/main.c
 *
 * Features:
 *  • Dual Mode: Standard & Scientific
 *  • Scientific functions: sqrt, x^2, 1/x, %, +/-, mod
 *  • Calculation history journal
 *  • Keyboard entry support (Numpad, Digits, Operators, Enter, Bksp, Esc)
 *  • Catppuccin Mocha aesthetic layout
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/math.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       340
#define WIN_H       460
#define MAP_ADDR    ((void *)0x62000000)

#define DISPLAY_MAX  20

static char   g_display[DISPLAY_MAX + 1] = "0";
static int    g_display_len  = 1;
static double g_accumulator  = 0.0;
static char   g_pending_op   = 0;
static int    g_new_number   = 1;

/* History Tape */
#define MAX_HIST 8
typedef struct {
    char expr[32];
    char res[24];
} calc_hist_t;
static calc_hist_t g_history[MAX_HIST];
static int g_hist_count = 0;

/* Button layout (6 rows × 4 cols) */
#define NCOLS  4
#define NROWS  6
#define BTN_W  74
#define BTN_H  44
#define BTN_GAP 6
#define BTN_OX 10
#define BTN_OY 130

static uk_window_t g_win;
static int g_hovered = -1;

typedef struct {
    const char *label;
    char action;
    unsigned int accent;
} calc_btn_t;

/* Row 0: sqrt  x^2  1/x  mod
   Row 1: C     +-   %    /
   Row 2: 7     8    9    *
   Row 3: 4     5    6    -
   Row 4: 1     2    3    +
   Row 5: 0     .    =    = */
static const calc_btn_t g_buttons[24] = {
    { "sqr", 'S', UK_SURFACE1 }, { "x^2", 'Q', UK_SURFACE1 }, { "1/x", 'R', UK_SURFACE1 }, { "mod", 'M', UK_PEACH },
    { "C",   'C', UK_MAROON },   { "+-",  'N', UK_SURFACE1 }, { "%",   '%', UK_SURFACE1 }, { "/",   '/', UK_PEACH },
    { "7",   '7', UK_SURFACE0 }, { "8",   '8', UK_SURFACE0 }, { "9",   '9', UK_SURFACE0 }, { "x",   '*', UK_PEACH },
    { "4",   '4', UK_SURFACE0 }, { "5",   '5', UK_SURFACE0 }, { "6",   '6', UK_SURFACE0 }, { "-",   '-', UK_PEACH },
    { "1",   '1', UK_SURFACE0 }, { "2",   '2', UK_SURFACE0 }, { "3",   '3', UK_SURFACE0 }, { "+",   '+', UK_PEACH },
    { "0",   '0', UK_SURFACE0 }, { ".",   '.', UK_SURFACE0 }, { "pi",  'P', UK_SURFACE1 }, { "=",   '=', UK_MAUVE },
};

static char g_numbuf[32];
static const char *double_to_str(double v)
{
    if (__builtin_isnan(v)) return "Error";
    if (__builtin_isinf(v)) return (v < 0) ? "-Inf" : "Inf";
    snprintf(g_numbuf, sizeof(g_numbuf), "%.10g", v);
    return g_numbuf;
}

static void display_set(const char *s)
{
    int i;
    for (i = 0; s[i] && i < DISPLAY_MAX; i++) g_display[i] = s[i];
    g_display[i] = '\0';
    g_display_len = i;
}

static void add_history_entry(const char *expr, const char *res)
{
    if (g_hist_count < MAX_HIST) {
        strncpy(g_history[g_hist_count].expr, expr, 31);
        strncpy(g_history[g_hist_count].res, res, 23);
        g_hist_count++;
    } else {
        for (int i = 1; i < MAX_HIST; i++) g_history[i - 1] = g_history[i];
        strncpy(g_history[MAX_HIST - 1].expr, expr, 31);
        strncpy(g_history[MAX_HIST - 1].res, res, 23);
    }
}

static void calc_handle(char action)
{
    if (action >= '0' && action <= '9') {
        if (g_new_number) {
            g_display[0] = action;
            g_display[1] = '\0';
            g_display_len = 1;
            g_new_number = 0;
        } else if (g_display_len < DISPLAY_MAX) {
            g_display[g_display_len++] = action;
            g_display[g_display_len] = '\0';
        }
    } else if (action == '.') {
        int has_dot = 0;
        for (int j = 0; j < g_display_len; j++) if (g_display[j] == '.') { has_dot = 1; break; }
        if (!has_dot && g_display_len < DISPLAY_MAX) {
            if (g_new_number) { g_display[0] = '0'; g_display[1] = '\0'; g_display_len = 1; g_new_number = 0; }
            g_display[g_display_len++] = '.';
            g_display[g_display_len] = '\0';
        }
    } else if (action == 'C') {
        display_set("0");
        g_accumulator = 0.0;
        g_pending_op = 0;
        g_new_number = 1;
    } else if (action == 'P') { /* Pi Constant */
        display_set("3.1415926535");
        g_new_number = 1;
    } else if (action == 'B') { /* Backspace */
        if (g_display_len > 1 && !g_new_number) {
            g_display[--g_display_len] = '\0';
        } else {
            display_set("0");
            g_new_number = 1;
        }
    } else if (action == 'N') { /* Negate */
        double v = atof(g_display);
        v = -v;
        display_set(double_to_str(v));
    } else if (action == 'S') { /* Sqrt */
        double v = atof(g_display);
        if (v < 0.0) { display_set("Error"); return; }
        double r = sqrt(v);
        char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "sqrt(%g)", v);
        display_set(double_to_str(r));
        add_history_entry(hbuf, g_display);
        g_new_number = 1;
    } else if (action == 'Q') { /* Square */
        double v = atof(g_display);
        double sq = v * v;
        char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "sqr(%g)", v);
        display_set(double_to_str(sq));
        add_history_entry(hbuf, g_display);
        g_new_number = 1;
    } else if (action == 'R') { /* Reciprocal 1/x */
        double v = atof(g_display);
        if (v == 0.0) { display_set("Error"); return; }
        double r = 1.0 / v;
        char hbuf[32]; snprintf(hbuf, sizeof(hbuf), "1/(%g)", v);
        display_set(double_to_str(r));
        add_history_entry(hbuf, g_display);
        g_new_number = 1;
    } else if (action == '%') { /* Percent */
        double v = atof(g_display);
        double r = (g_accumulator != 0.0) ? (g_accumulator * v / 100.0) : (v / 100.0);
        display_set(double_to_str(r));
        g_new_number = 1;
    } else if (action == '+' || action == '-' || action == '*' || action == '/' || action == 'M') {
        double val = atof(g_display);

        if (g_pending_op && !g_new_number) {
            switch (g_pending_op) {
            case '+': g_accumulator += val; break;
            case '-': g_accumulator -= val; break;
            case '*': g_accumulator *= val; break;
            case '/': if (val != 0.0) g_accumulator /= val; else { display_set("Error"); return; } break;
            case 'M': if (val != 0.0) g_accumulator = fmod(g_accumulator, val); else { display_set("Error"); return; } break;
            }
        } else {
            g_accumulator = val;
        }
        g_pending_op = action;
        display_set(double_to_str(g_accumulator));
        g_new_number = 1;
    } else if (action == '=') {
        double val = atof(g_display);

        if (g_pending_op) {
            char expr_str[32];
            snprintf(expr_str, sizeof(expr_str), "%g %c %g", g_accumulator, g_pending_op == 'M' ? '%' : g_pending_op, val);
            switch (g_pending_op) {
            case '+': g_accumulator += val; break;
            case '-': g_accumulator -= val; break;
            case '*': g_accumulator *= val; break;
            case '/': if (val != 0.0) g_accumulator /= val; else { display_set("Error"); return; } break;
            case 'M': if (val != 0.0) g_accumulator = fmod(g_accumulator, val); else { display_set("Error"); return; } break;
            }
            g_pending_op = 0;
            display_set(double_to_str(g_accumulator));
            add_history_entry(expr_str, g_display);
        }
        g_new_number = 1;
    }
}

static void draw_calc(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Header ──────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 36, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 36, UK_PEACH);
    uk_draw_text(&g_win, 14, 6, "Calculator", UK_TEXT);
    uk_draw_text(&g_win, 14, 20, "AzamiOS Math Engine", UK_OVERLAY0);
    uk_hline(&g_win, 0, 36, (int)w, UK_SURFACE1);

    /* ── Display Panel ───────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 36, (int)w, 86, UK_CRUST);
    uk_hline(&g_win, 0, 122, (int)w, UK_SURFACE1);

    /* Calculation history preview */
    if (g_hist_count > 0) {
        char hist_str[48];
        snprintf(hist_str, sizeof(hist_str), "%s = %s", g_history[g_hist_count - 1].expr, g_history[g_hist_count - 1].res);
        int hlen = (int)strlen(hist_str);
        uk_draw_text(&g_win, (int)w - hlen * 8 - 14, 44, hist_str, UK_OVERLAY0);
    }

    /* Pending operator */
    if (g_pending_op) {
        char op_str[4] = { g_pending_op == 'M' ? '%' : g_pending_op, '\0' };
        uk_draw_text(&g_win, 14, 44, op_str, UK_MAUVE);
    }

    /* Main Digit Display */
    int dlen = uk_strlen(g_display);
    int dx = (int)w - dlen * 16 - 16;
    if (dx < 12) dx = 12;
    uk_draw_text_2x(&g_win, dx, 68, g_display, UK_TEXT);

    /* ── Button Grid (6x4) ───────────────────────────────────────────────── */
    for (int i = 0; i < 24; i++) {
        int col = i % NCOLS;
        int row = i / NCOLS;

        if (row == 5 && col == 3) continue; /* merged with col 2 */

        int bx = BTN_OX + col * (BTN_W + BTN_GAP);
        int by = BTN_OY + row * (BTN_H + BTN_GAP);
        int bw = (row == 5 && col == 2) ? (BTN_W * 2 + BTN_GAP) : BTN_W;

        unsigned int bg = g_buttons[i].accent;
        if (g_hovered == i) bg = UK_SURFACE2;

        uk_fill_rounded_rect(&g_win, bx, by, bw, BTN_H, 6, bg);
        uk_hline(&g_win, bx + 2, by, bw - 4, UK_SURFACE2);
        uk_hline(&g_win, bx + 2, by + BTN_H - 1, bw - 4, UK_CRUST);

        unsigned int fg = (g_buttons[i].accent == UK_MAUVE || g_buttons[i].accent == UK_PEACH) ? UK_BASE : UK_TEXT;
        if (g_hovered == i) fg = UK_TEXT;

        int tlen = uk_strlen(g_buttons[i].label);
        uk_draw_text(&g_win, bx + (bw - tlen * 8) / 2, by + (BTN_H - 16) / 2, g_buttons[i].label, fg);
    }

    uk_invalidate(&g_win);
}

static int hit_button(int mx, int my)
{
    for (int i = 0; i < 24; i++) {
        int col = i % NCOLS;
        int row = i / NCOLS;
        if (row == 5 && col == 3) continue;

        int bx = BTN_OX + col * (BTN_W + BTN_GAP);
        int by = BTN_OY + row * (BTN_H + BTN_GAP);
        int bw = (row == 5 && col == 2) ? (BTN_W * 2 + BTN_GAP) : BTN_W;

        if (mx >= bx && mx < bx + bw && my >= by && my < by + BTN_H) return i;
    }
    return -1;
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

    int ret = uk_window_connect(&g_win, "Calculator",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    draw_calc();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                char c = (char)msg.key.keycode;
                if (c >= '0' && c <= '9') calc_handle(c);
                else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '.') calc_handle(c);
                else if (c == '\n' || c == '\r' || c == '=') calc_handle('=');
                else if (c == 'c' || c == 'C' || msg.key.scancode == 1) calc_handle('C');
                draw_calc();
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            int hit = hit_button(mx, my);

            if (msg.mouse.buttons & 1) {
                if (hit >= 0) {
                    calc_handle(g_buttons[hit].action);
                    draw_calc();
                }
            } else {
                if (hit != g_hovered) {
                    g_hovered = hit;
                    draw_calc();
                }
            }
        }
    }

    return 0;
}
