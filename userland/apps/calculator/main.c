/* ============================================================================
 * AzamiOS — Calculator
 * File: userland/apps/calculator/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       320
#define WIN_H       420
#define MAP_ADDR    ((void *)0x62000000)

/* ── Calculator logic ───────────────────────────────────────────────────────── */
#define DISPLAY_MAX  16

static char  g_display[DISPLAY_MAX + 1];   /* current display string      */
static int   g_display_len  = 0;
static long  g_accumulator  = 0;           /* left operand                */
static char  g_pending_op   = 0;           /* '+','-','*','/',0           */
static int   g_new_number   = 1;           /* true = next digit starts fresh */

/* Button layout (5×4 grid) */
#define NCOLS  4
#define NROWS  5
#define BTN_W  68
#define BTN_H  52
#define BTN_GAD 4
#define BTN_OX  8
#define BTN_OY  100

static uk_window_t g_win;
static int g_hovered = -1;

typedef struct { const char *label; char action; unsigned int accent; } calc_btn_t;

/* Row 0: C  ±  %  ÷
   Row 1: 7  8  9  ×
   Row 2: 4  5  6  -
   Row 3: 1  2  3  +
   Row 4: 0(wide) .  = */
static const calc_btn_t g_buttons[20] = {
    {"C",  'C', UK_MAROON},  {"+-",'N', UK_SURFACE1}, {"%", '%', UK_SURFACE1}, {"/", '/', UK_PEACH},
    {"7",  '7', UK_SURFACE0},{"8",  '8', UK_SURFACE0},{"9",  '9', UK_SURFACE0},{"x", '*', UK_PEACH},
    {"4",  '4', UK_SURFACE0},{"5",  '5', UK_SURFACE0},{"6",  '6', UK_SURFACE0},{"-", '-', UK_PEACH},
    {"1",  '1', UK_SURFACE0},{"2",  '2', UK_SURFACE0},{"3",  '3', UK_SURFACE0},{"+", '+', UK_PEACH},
    {"0",  '0', UK_SURFACE0},{"0",  '0', UK_SURFACE0},{".",  '.', UK_SURFACE0},{"=", '=', UK_MAUVE},
};

/* Long integer to decimal string (returns pointer to static buf) */
static char g_numbuf[24];
static const char *long_to_str(long v)
{
    if (v == 0) { g_numbuf[0] = '0'; g_numbuf[1] = '\0'; return g_numbuf; }
    int neg = (v < 0);
    unsigned long uv = neg ? (unsigned long)(-(v + 1)) + 1 : (unsigned long)v;
    int i = 22;
    g_numbuf[23] = '\0';
    while (uv > 0) { g_numbuf[i--] = '0' + (char)(uv % 10); uv /= 10; }
    if (neg) g_numbuf[i--] = '-';
    return &g_numbuf[i + 1];
}

static void display_set(const char *s)
{
    int i;
    for (i = 0; s[i] && i < DISPLAY_MAX; i++) g_display[i] = s[i];
    g_display[i] = '\0';
    g_display_len = i;
}

static void calc_handle(char action)
{
    if (action >= '0' && action <= '9') {
        if (g_new_number) { g_display[0] = action; g_display[1] = '\0'; g_display_len = 1; g_new_number = 0; }
        else if (g_display_len < DISPLAY_MAX) { g_display[g_display_len++] = action; g_display[g_display_len] = '\0'; }
    } else if (action == '.') {
        /* Check no existing dot */
        int has_dot = 0;
        int j;
        for (j = 0; j < g_display_len; j++) if (g_display[j] == '.') { has_dot = 1; break; }
        if (!has_dot && g_display_len < DISPLAY_MAX) {
            if (g_new_number) { g_display[0]='0'; g_display[1]='\0'; g_display_len=1; g_new_number=0; }
            g_display[g_display_len++] = '.'; g_display[g_display_len] = '\0';
        }
    } else if (action == 'C') {
        display_set("0"); g_accumulator = 0; g_pending_op = 0; g_new_number = 1;
    } else if (action == 'N') { /* negate */
        if (g_display[0] == '-') {
            int j;
            for (j = 0; j < g_display_len; j++) g_display[j] = g_display[j+1];
            g_display_len--;
        } else if (g_display_len < DISPLAY_MAX) {
            int j;
            for (j = g_display_len; j >= 0; j--) g_display[j+1] = g_display[j];
            g_display[0] = '-'; g_display_len++;
        }
    } else if (action == '+' || action == '-' || action == '*' || action == '/') {
        /* Parse current display as integer (integer-only for simplicity) */
        long val = 0; int neg = 0; int j = 0;
        if (g_display[0] == '-') { neg = 1; j = 1; }
        for (; g_display[j]; j++) {
            if (g_display[j] == '.') break;
            if (g_display[j] >= '0' && g_display[j] <= '9') val = val * 10 + (g_display[j] - '0');
        }
        if (neg) val = -val;

        if (g_pending_op && !g_new_number) {
            /* Apply previous op */
            switch (g_pending_op) {
            case '+': g_accumulator += val; break;
            case '-': g_accumulator -= val; break;
            case '*': g_accumulator *= val; break;
            case '/':
                if (val == 0) {
                    display_set("Error");
                    g_accumulator = 0;
                    g_pending_op = 0;
                    g_new_number = 1;
                    return;
                }
                g_accumulator /= val;
                break;
            }
        } else {
            g_accumulator = val;
        }
        g_pending_op = action;
        display_set(long_to_str(g_accumulator));
        g_new_number = 1;
    } else if (action == '=') {
        long val = 0; int neg = 0; int j = 0;
        if (g_display[0] == '-') { neg = 1; j = 1; }
        for (; g_display[j]; j++) {
            if (g_display[j] == '.') break;
            if (g_display[j] >= '0' && g_display[j] <= '9') val = val * 10 + (g_display[j] - '0');
        }
        if (neg) val = -val;
        if (g_pending_op) {
            switch (g_pending_op) {
            case '+': g_accumulator += val; break;
            case '-': g_accumulator -= val; break;
            case '*': g_accumulator *= val; break;
            case '/':
                if (val == 0) {
                    display_set("Error");
                    g_accumulator = 0;
                    g_pending_op = 0;
                    g_new_number = 1;
                    return;
                }
                g_accumulator /= val;
                break;
            }
        } else {
            g_accumulator = val;
        }
        g_pending_op = 0;
        display_set(long_to_str(g_accumulator));
        g_new_number = 1;
    } else if (action == '%') {
        long val = 0; int neg = 0; int j = 0;
        if (g_display[0] == '-') { neg = 1; j = 1; }
        for (; g_display[j]; j++) {
            if (g_display[j] == '.') break;
            if (g_display[j] >= '0' && g_display[j] <= '9') val = val * 10 + (g_display[j] - '0');
        }
        if (neg) val = -val;
        /* Percentage: calculate value / 100 with 2 decimals */
        long int_part = val / 100;
        long frac_part = val % 100;
        if (frac_part < 0) frac_part = -frac_part;
        if (frac_part == 0) {
            display_set(long_to_str(int_part));
        } else {
            char pbuf[32];
            snprintf(pbuf, sizeof(pbuf), "%s.%s%ld", long_to_str(int_part), (frac_part < 10 ? "0" : ""), frac_part);
            display_set(pbuf);
        }
        g_new_number = 1;
    }
}

static void draw_calc(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Display area ───────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, 0, (int)w, 92, UK_CRUST);
    uk_hline(&g_win, 0, 92, (int)w, UK_SURFACE1);

    /* Display value — right-aligned, 2× scale */
    int dlen = uk_strlen(g_display);
    int dx = (int)w - dlen * 16 - 16;
    if (dx < 8) dx = 8;
    uk_draw_text_2x(&g_win, dx, 32, g_display, UK_TEXT);

    /* Pending op indicator */
    if (g_pending_op) {
        char op_str[3] = { g_pending_op, '\0', '\0' };
        if (g_pending_op == '*') op_str[0] = 'x';
        uk_draw_text(&g_win, 12, 12, op_str, UK_MAUVE);
    }

    /* ── Button grid ────────────────────────────────────────────────────── */
    int i;
    for (i = 0; i < 20; i++) {
        int col = i % NCOLS;
        int row = i / NCOLS;

        if (row == 4 && col == 1) {
            continue; /* Skip col 1 because wide "0" button spans cols 0 and 1 */
        }

        /* Row 4 col 0: "0" button is double-wide */
        if (row == 4 && col == 0) {
            int bx = BTN_OX;
            int by = BTN_OY + row * (BTN_H + BTN_GAD);
            int bw = BTN_W * 2 + BTN_GAD;
            uk_btn_state_t state = (g_hovered == i) ? UK_BTN_HOVER : UK_BTN_NORMAL;
            uk_fill_rounded_rect(&g_win, bx, by, bw, BTN_H, 8, g_buttons[i].accent);
            uk_hline(&g_win, bx, by, bw, UK_SURFACE1);
            uk_hline(&g_win, bx, by + BTN_H - 1, bw, UK_CRUST);
            if (state == UK_BTN_HOVER)
                uk_fill_rounded_rect(&g_win, bx, by, bw, BTN_H, 8, UK_SURFACE1);
            uk_draw_text_centred(&g_win, bx + bw / 2, by + (BTN_H - 16) / 2, "0", UK_TEXT);
            continue;
        }

        int bx = BTN_OX + col * (BTN_W + BTN_GAD);
        int by = BTN_OY + row * (BTN_H + BTN_GAD);

        unsigned int bg = g_buttons[i].accent;
        if (g_hovered == i) bg = UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, bx, by, BTN_W, BTN_H, 8, bg);
        uk_hline(&g_win, bx, by, BTN_W, UK_SURFACE2);
        uk_hline(&g_win, bx, by + BTN_H - 1, BTN_W, UK_CRUST);

        unsigned int fg = (g_buttons[i].accent == UK_MAUVE || g_buttons[i].accent == UK_PEACH)
                          ? UK_BASE : UK_TEXT;
        if (g_hovered == i) fg = UK_TEXT;

        int tlen = uk_strlen(g_buttons[i].label);
        uk_draw_text(&g_win, bx + BTN_W / 2 - tlen * 4, by + (BTN_H - 16) / 2,
                     g_buttons[i].label, fg);
    }

    uk_invalidate(&g_win);
}

static int hit_button(int mx, int my)
{
    int i;
    for (i = 0; i < 20; i++) {
        int col = i % NCOLS;
        int row = i / NCOLS;

        if (row == 4 && col == 1) {
            continue;
        }

        int bx = BTN_OX + col * (BTN_W + BTN_GAD);
        int by = BTN_OY + row * (BTN_H + BTN_GAD);
        int bw = (row == 4 && col == 0) ? (BTN_W * 2 + BTN_GAD) : BTN_W;

        if (mx >= bx && mx < bx + bw && my >= by && my < by + BTN_H) {
            return i;
        }
    }
    return -1;
}

static void handle_key(unsigned char keycode, unsigned char scancode, unsigned char pressed, unsigned short modifiers)
{
    if (!pressed) return;
    int shift = (modifiers & 1) != 0;
    char action = 0;

    /* Direct ASCII operations & digits */
    if (keycode >= '0' && keycode <= '9') {
        action = (char)keycode;
    } else if (keycode == '+' || keycode == '-' || keycode == '*' || keycode == '/' || keycode == '%' || keycode == '.') {
        action = (char)keycode;
    } else if (keycode == '=' || keycode == '\n' || keycode == '\r' || scancode == 28) {
        action = '=';
    } else if (keycode == 'c' || keycode == 'C' || keycode == 27 || keycode == '\b' || scancode == 1 || scancode == 14) {
        action = 'C';
    } else if (scancode >= 2 && scancode <= 10) {
        if (shift && scancode == 9) action = '*';
        else if (shift && scancode == 6) action = '%';
        else action = '1' + (scancode - 2);
    } else if (scancode == 11) {
        action = '0';
    } else if (scancode == 12) {
        action = '-';
    } else if (scancode == 13) {
        action = shift ? '+' : '=';
    } else if (scancode == 52) {
        action = '.';
    } else if (scancode == 53) {
        action = '/';
    }

    if (action) {
        calc_handle(action);
        draw_calc();
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("[calculator] Starting...");

    display_set("0");

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "Calculator",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { puts("[calculator] FATAL"); return -1; }

    draw_calc();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_KEY_EVENT) {
            handle_key(msg.key.keycode, msg.key.scancode, msg.key.pressed, msg.key.modifiers);
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = (int)msg.mouse.abs_x;
            int my = (int)msg.mouse.abs_y;
            int prev_hover = g_hovered;
            g_hovered = hit_button(mx, my);
            if (g_hovered != prev_hover) draw_calc();

            static int was_down = 0;
            int is_down = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) != 0;

            if (is_down && !was_down && g_hovered >= 0) {
                calc_handle(g_buttons[g_hovered].action);
                draw_calc();
            }
            was_down = is_down;
        }
    }
    sys_exit(0);
}
