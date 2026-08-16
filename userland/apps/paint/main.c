/* ============================================================================
 * AzamiOS Desktop Environment — Paint & Sketch Tool
 * File: userland/apps/paint/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"
#include "../shared/de_log.h"

#define SERVER_CHAN     1
#define WIN_W          640
#define WIN_H          480
#define MAP_ADDR       ((void *)0x64000000)

#define CANVAS_X       16
#define CANVAS_Y       60
#define CANVAS_W       608
#define CANVAS_H       360

static uk_window_t g_win;

/* Paint state */
static unsigned int g_canvas[CANVAS_W * CANVAS_H];
static unsigned int g_brush_color = UK_MAUVE;
static int          g_brush_size = 2; /* 1, 2, 4, 8 */
static int          g_tool_mode = 0;  /* 0: Pen, 1: Eraser, 2: Bucket Fill */
static int          g_prev_mx = -1;
static int          g_prev_my = -1;

static unsigned int g_palette[] = {
    UK_MAUVE,
    UK_RED,
    UK_PEACH,
    UK_YELLOW,
    UK_GREEN,
    UK_TEAL,
    UK_SAPPHIRE,
    UK_BLUE,
    UK_LAVENDER,
    UK_TEXT,
    UK_SUBTEXT0,
    UK_BASE
};
#define NUM_COLORS ((int)(sizeof(g_palette) / sizeof(g_palette[0])))

static void init_canvas(void)
{
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        g_canvas[i] = 0xFFFFFFFF; /* Clean white canvas */
    }
}

static void draw_canvas_pixel(int cx, int cy, unsigned int col, int size)
{
    if (cx < 0 || cx >= CANVAS_W || cy < 0 || cy >= CANVAS_H) return;

    for (int dy = -size; dy <= size; dy++) {
        for (int dx = -size; dx <= size; dx++) {
            if (dx * dx + dy * dy <= size * size) {
                int px = cx + dx;
                int py = cy + dy;
                if (px >= 0 && px < CANVAS_W && py >= 0 && py < CANVAS_H) {
                    g_canvas[py * CANVAS_W + px] = col;
                }
            }
        }
    }
}

static void draw_canvas_line(int x0, int y0, int x1, int y1, unsigned int col, int size)
{
    int dx = (x1 >= x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 >= y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        draw_canvas_pixel(x0, y0, col, size);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
    }
}

/* Flood Fill implementation for Paint Bucket tool */
static void flood_fill(int start_x, int start_y, unsigned int target_col, unsigned int fill_col)
{
    if (target_col == fill_col) return;
    if (start_x < 0 || start_x >= CANVAS_W || start_y < 0 || start_y >= CANVAS_H) return;

    /* Simple stack-based flood fill with fixed buffer */
    static short stack_x[8192];
    static short stack_y[8192];
    int sp = 0;

    stack_x[sp] = (short)start_x;
    stack_y[sp] = (short)start_y;
    sp++;

    while (sp > 0) {
        sp--;
        int x = stack_x[sp];
        int y = stack_y[sp];

        if (x < 0 || x >= CANVAS_W || y < 0 || y >= CANVAS_H) continue;
        if (g_canvas[y * CANVAS_W + x] != target_col) continue;

        g_canvas[y * CANVAS_W + x] = fill_col;

        if (sp < 8188) {
            if (x + 1 < CANVAS_W && g_canvas[y * CANVAS_W + (x + 1)] == target_col) {
                stack_x[sp] = (short)(x + 1); stack_y[sp] = (short)y; sp++;
            }
            if (x - 1 >= 0 && g_canvas[y * CANVAS_W + (x - 1)] == target_col) {
                stack_x[sp] = (short)(x - 1); stack_y[sp] = (short)y; sp++;
            }
            if (y + 1 < CANVAS_H && g_canvas[(y + 1) * CANVAS_W + x] == target_col) {
                stack_x[sp] = (short)x; stack_y[sp] = (short)(y + 1); sp++;
            }
            if (y - 1 >= 0 && g_canvas[(y - 1) * CANVAS_W + x] == target_col) {
                stack_x[sp] = (short)x; stack_y[sp] = (short)(y - 1); sp++;
            }
        }
    }
}

static void render_paint_ui(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_MANTLE);

    /* ── Top Toolbar ──────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 48, UK_SURFACE0, UK_BASE);
    uk_hline(&g_win, 0, 48, (int)w, UK_SURFACE1);

    /* Tool Buttons: Pen (0), Eraser (1), Fill (2), Clear (3) */
    uk_draw_button(&g_win, 16, 10, 60, 28, "Pen", (g_tool_mode == 0) ? UK_BTN_HOVER : UK_BTN_NORMAL);
    uk_draw_button(&g_win, 82, 10, 68, 28, "Eraser", (g_tool_mode == 1) ? UK_BTN_HOVER : UK_BTN_NORMAL);
    uk_draw_button(&g_win, 156, 10, 58, 28, "Fill", (g_tool_mode == 2) ? UK_BTN_HOVER : UK_BTN_NORMAL);
    uk_draw_button(&g_win, 220, 10, 62, 28, "Clear", UK_BTN_NORMAL);

    /* Brush Size Indicator */
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "Size: %dpx", g_brush_size);
    uk_draw_text(&g_win, 296, 18, size_str, UK_SUBTEXT0);

    /* Palette colors */
    int pal_start_x = 380;
    for (int i = 0; i < NUM_COLORS; i++) {
        int px = pal_start_x + i * 20;
        int py = 14;
        uk_fill_rounded_rect(&g_win, px, py, 16, 20, 4, g_palette[i]);
        if (g_brush_color == g_palette[i] && g_tool_mode != 1) {
            /* Active color indicator */
            uk_hline(&g_win, px - 1, py + 22, 18, UK_TEXT);
        }
    }

    /* ── Canvas border & Drop Shadow ─────────────────────────────────────── */
    uk_fill_rect(&g_win, CANVAS_X - 2, CANVAS_Y - 2, CANVAS_W + 4, CANVAS_H + 4, UK_SURFACE1);

    /* Render Canvas to window buffer */
    for (int cy = 0; cy < CANVAS_H; cy++) {
        for (int cx = 0; cx < CANVAS_W; cx++) {
            uk_put_pixel(&g_win, CANVAS_X + cx, CANVAS_Y + cy, g_canvas[cy * CANVAS_W + cx]);
        }
    }

    /* ── Bottom Status Bar ────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, 0, (int)h - 28, (int)w, 28, UK_BASE);
    uk_hline(&g_win, 0, (int)h - 28, (int)w, UK_SURFACE0);
    uk_draw_text(&g_win, 16, (int)h - 20, "Azami Paint | 1-4: Brush Size | C: Clear Canvas | Left Drag to Draw", UK_OVERLAY0);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[paint] Starting Azami Paint application...");

    init_canvas();

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Azami Paint",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) {
        de_log("[paint] FATAL: window connect failed");
        return -1;
    }

    render_paint_ui();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                if (msg.key.keycode == '1') { g_brush_size = 1; render_paint_ui(); }
                else if (msg.key.keycode == '2') { g_brush_size = 2; render_paint_ui(); }
                else if (msg.key.keycode == '3') { g_brush_size = 4; render_paint_ui(); }
                else if (msg.key.keycode == '4') { g_brush_size = 8; render_paint_ui(); }
                else if (msg.key.keycode == 'c' || msg.key.keycode == 'C') {
                    init_canvas();
                    render_paint_ui();
                } else if (msg.key.keycode == 'p' || msg.key.keycode == 'P') {
                    g_tool_mode = 0;
                    render_paint_ui();
                } else if (msg.key.keycode == 'e' || msg.key.keycode == 'E') {
                    g_tool_mode = 1;
                    render_paint_ui();
                } else if (msg.key.keycode == 'f' || msg.key.keycode == 'F') {
                    g_tool_mode = 2;
                    render_paint_ui();
                }
            }
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            int lbtn = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT);

            if (lbtn) {
                /* Check Top toolbar clicks */
                if (my >= 10 && my <= 38) {
                    if (mx >= 16 && mx <= 76) { g_tool_mode = 0; render_paint_ui(); }
                    else if (mx >= 82 && mx <= 150) { g_tool_mode = 1; render_paint_ui(); }
                    else if (mx >= 156 && mx <= 214) { g_tool_mode = 2; render_paint_ui(); }
                    else if (mx >= 220 && mx <= 282) { init_canvas(); render_paint_ui(); }
                    else if (mx >= 380 && mx < 380 + NUM_COLORS * 20) {
                        int pidx = (mx - 380) / 20;
                        if (pidx >= 0 && pidx < NUM_COLORS) {
                            g_brush_color = g_palette[pidx];
                            if (g_tool_mode == 1) g_tool_mode = 0;
                            render_paint_ui();
                        }
                    }
                }
                /* Canvas drawing */
                else if (mx >= CANVAS_X && mx < CANVAS_X + CANVAS_W &&
                         my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H) {
                    int cx = mx - CANVAS_X;
                    int cy = my - CANVAS_Y;

                    unsigned int col = (g_tool_mode == 1) ? 0xFFFFFFFF : g_brush_color;

                    if (g_tool_mode == 2) {
                        /* Flood fill */
                        unsigned int target = g_canvas[cy * CANVAS_W + cx];
                        flood_fill(cx, cy, target, col);
                    } else {
                        if (g_prev_mx >= 0 && g_prev_my >= 0) {
                            draw_canvas_line(g_prev_mx, g_prev_my, cx, cy, col, g_brush_size);
                        } else {
                            draw_canvas_pixel(cx, cy, col, g_brush_size);
                        }
                    }

                    g_prev_mx = cx;
                    g_prev_my = cy;

                    /* Live blit updated canvas pixels to window surface */
                    for (int r = 0; r < CANVAS_H; r++) {
                        for (int c = 0; c < CANVAS_W; c++) {
                            uk_put_pixel(&g_win, CANVAS_X + c, CANVAS_Y + r, g_canvas[r * CANVAS_W + c]);
                        }
                    }
                    uk_invalidate(&g_win);
                }
            } else {
                g_prev_mx = -1;
                g_prev_my = -1;
            }
        }
    }

    az_wm_msg_t dmsg;
    memset(&dmsg, 0, sizeof(dmsg));
    dmsg.type = AZ_WM_DESTROY_WINDOW;
    dmsg.wid  = g_win.wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&dmsg);

    sys_exit(0);
}
