/* ============================================================================
 * AzamiOS Desktop Environment — Paint & Sketch Tool (v3.0)
 * File: userland/apps/paint/main.c
 *
 * Features:
 *  • Complete tool suite: Pen, Eraser, Bucket Fill, Line, Rect, Circle, Picker
 *  • Undo buffer support (Ctrl+Z)
 *  • Dynamic brush size adjustment (1, 2, 4, 8, 16px)
 *  • 12-color Catppuccin swatch palette + custom color sampler
 *  • High-performance scanline canvas blitting
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
#define WIN_W          680
#define WIN_H          500
#define MAP_ADDR       ((void *)0x64000000)

#define CANVAS_X       16
#define CANVAS_Y       56
#define CANVAS_W       648
#define CANVAS_H       400

static uk_window_t g_win;

/* Paint state */
static unsigned int g_canvas[CANVAS_W * CANVAS_H];
static unsigned int g_undo_buf[CANVAS_W * CANVAS_H];
static int          g_has_undo = 0;

static unsigned int g_brush_color = UK_MAUVE;
static int          g_brush_size = 2; /* 1, 2, 4, 8, 16 */
static int          g_tool_mode = 0;  /* 0: Pen, 1: Eraser, 2: Fill, 3: Line, 4: Rect, 5: Circle, 6: Picker */
static int          g_prev_mx = -1;
static int          g_prev_my = -1;
static int          g_drag_start_x = -1;
static int          g_drag_start_y = -1;
static int          g_mouse_down = 0;

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
    UK_CRUST
};
#define NUM_COLORS ((int)(sizeof(g_palette) / sizeof(g_palette[0])))

static void save_undo(void)
{
    memcpy(g_undo_buf, g_canvas, sizeof(g_canvas));
    g_has_undo = 1;
}

static void apply_undo(void)
{
    if (g_has_undo) {
        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
        g_has_undo = 0;
    }
}

static void init_canvas(void)
{
    for (int i = 0; i < CANVAS_W * CANVAS_H; i++) {
        g_canvas[i] = 0xFFFFFFFF; /* Clean white canvas */
    }
    save_undo();
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

static void draw_canvas_rect(int x0, int y0, int x1, int y1, unsigned int col, int size)
{
    draw_canvas_line(x0, y0, x1, y0, col, size);
    draw_canvas_line(x1, y0, x1, y1, col, size);
    draw_canvas_line(x1, y1, x0, y1, col, size);
    draw_canvas_line(x0, y1, x0, y0, col, size);
}

static int isqrt(int n)
{
    if (n <= 0) return 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

static void draw_canvas_circle(int cx, int cy, int r, unsigned int col, int size)
{
    if (r <= 0) return;
    int x = 0, y = r;
    int d = 3 - 2 * r;
    while (y >= x) {
        draw_canvas_pixel(cx + x, cy + y, col, size);
        draw_canvas_pixel(cx - x, cy + y, col, size);
        draw_canvas_pixel(cx + x, cy - y, col, size);
        draw_canvas_pixel(cx - x, cy - y, col, size);
        draw_canvas_pixel(cx + y, cy + x, col, size);
        draw_canvas_pixel(cx - y, cy + x, col, size);
        draw_canvas_pixel(cx + y, cy - x, col, size);
        draw_canvas_pixel(cx - y, cy - x, col, size);
        x++;
        if (d > 0) { y--; d = d + 4 * (x - y) + 10; }
        else d = d + 4 * x + 6;
    }
}

static void flood_fill(int start_x, int start_y, unsigned int target_col, unsigned int fill_col)
{
    if (target_col == fill_col) return;
    if (start_x < 0 || start_x >= CANVAS_W || start_y < 0 || start_y >= CANVAS_H) return;

    static short stack_x[16384];
    static short stack_y[16384];
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

        if (sp < 16380) {
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
    uk_gradient_h(&g_win, 0, 0, (int)w, 46, UK_SURFACE0, UK_BASE);
    uk_hline(&g_win, 0, 46, (int)w, UK_SURFACE1);

    /* Tool Buttons */
    const char *tools[] = { "Pen", "Eraser", "Fill", "Line", "Rect", "Circ", "Pick" };
    int tx = 10;
    for (int t = 0; t < 7; t++) {
        int tw = (t == 1 || t == 6) ? 56 : 46;
        uk_btn_state_t st = (g_tool_mode == t) ? UK_BTN_HOVER : UK_BTN_NORMAL;
        uk_draw_button(&g_win, tx, 8, tw, 28, tools[t], st);
        tx += tw + 4;
    }

    /* Undo & Clear Buttons */
    uk_draw_button(&g_win, tx, 8, 48, 28, "Undo", UK_BTN_NORMAL);
    tx += 52;
    uk_draw_button(&g_win, tx, 8, 48, 28, "Clear", UK_BTN_NORMAL);
    tx += 56;

    /* Brush Size Selector */
    char size_str[16];
    snprintf(size_str, sizeof(size_str), "%dpx", g_brush_size);
    uk_draw_text(&g_win, tx, 16, size_str, UK_SUBTEXT0);
    tx += 36;

    /* Swatch Palette */
    for (int i = 0; i < NUM_COLORS; i++) {
        int px = tx + i * 16;
        int py = 12;
        uk_fill_rounded_rect(&g_win, px, py, 14, 20, 3, g_palette[i]);
        if (g_brush_color == g_palette[i] && g_tool_mode != 1) {
            uk_hline(&g_win, px - 1, py + 22, 16, UK_TEXT);
        }
    }

    /* ── Canvas Border ─────────────────────────────────────────────────────── */
    uk_fill_rect(&g_win, CANVAS_X - 2, CANVAS_Y - 2, CANVAS_W + 4, CANVAS_H + 4, UK_SURFACE1);

    /* Fast Canvas Scanline Blit */
    for (int cy = 0; cy < CANVAS_H; cy++) {
        unsigned int *dst = &g_win.pixels[(CANVAS_Y + cy) * g_win.width + CANVAS_X];
        const unsigned int *src = &g_canvas[cy * CANVAS_W];
        memcpy(dst, src, CANVAS_W * sizeof(unsigned int));
    }

    /* ── Status Bar ────────────────────────────────────────────────────────── */
    int sby = (int)h - 22;
    uk_fill_rect(&g_win, 0, sby, (int)w, 22, UK_CRUST);
    uk_hline(&g_win, 0, sby, (int)w, UK_SURFACE1);

    char stat_l[80];
    snprintf(stat_l, sizeof(stat_l), "Tool: %s | Size: %dpx | Canvas: %dx%d",
             tools[g_tool_mode], g_brush_size, CANVAS_W, CANVAS_H);
    uk_draw_text(&g_win, 12, sby + 3, stat_l, UK_OVERLAY0);

    char stat_r[32];
    snprintf(stat_r, sizeof(stat_r), "Pos: %d,%d",
             (g_prev_mx >= 0) ? g_prev_mx : 0, (g_prev_my >= 0) ? g_prev_my : 0);
    uk_draw_text(&g_win, (int)w - (int)strlen(stat_r) * 8 - 12, sby + 3, stat_r, UK_SUBTEXT0);

    uk_invalidate(&g_win);
}

static void handle_click(int mx, int my)
{
    /* Top Toolbar button clicks */
    if (my >= 8 && my <= 36) {
        int tx = 10;
        for (int t = 0; t < 7; t++) {
            int tw = (t == 1 || t == 6) ? 56 : 46;
            if (mx >= tx && mx <= tx + tw) {
                g_tool_mode = t;
                return;
            }
            tx += tw + 4;
        }

        /* Undo */
        if (mx >= tx && mx <= tx + 48) {
            apply_undo();
            return;
        }
        tx += 52;

        /* Clear */
        if (mx >= tx && mx <= tx + 48) {
            save_undo();
            init_canvas();
            return;
        }
        tx += 56 + 36;

        /* Palette Swatches */
        for (int i = 0; i < NUM_COLORS; i++) {
            int px = tx + i * 16;
            if (mx >= px && mx <= px + 14) {
                g_brush_color = g_palette[i];
                if (g_tool_mode == 1) g_tool_mode = 0; /* switch back to pen */
                return;
            }
        }
    }
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

    int ret = uk_window_connect(&g_win, "Paint & Sketch Tool",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    init_canvas();
    render_paint_ui();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                if ((msg.key.modifiers & 2) && (msg.key.keycode == 'z' || msg.key.keycode == 'Z' || msg.key.keycode == 26)) {
                    apply_undo();
                    render_paint_ui();
                } else if (msg.key.keycode == '+' || msg.key.keycode == '=') {
                    if (g_brush_size < 16) g_brush_size *= 2;
                    render_paint_ui();
                } else if (msg.key.keycode == '-' || msg.key.keycode == '_') {
                    if (g_brush_size > 1) g_brush_size /= 2;
                    render_paint_ui();
                }
            }
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            int btn = msg.mouse.buttons;

            if (btn & 1) {
                if (!g_mouse_down) {
                    save_undo();
                    g_mouse_down = 1;
                    g_drag_start_x = mx - CANVAS_X;
                    g_drag_start_y = my - CANVAS_Y;
                    handle_click(mx, my);
                }

                int cx = mx - CANVAS_X;
                int cy = my - CANVAS_Y;

                if (cx >= 0 && cx < CANVAS_W && cy >= 0 && cy < CANVAS_H) {
                    g_prev_mx = cx;
                    g_prev_my = cy;

                    if (g_tool_mode == 0) { /* Pen */
                        if (g_drag_start_x >= 0)
                            draw_canvas_line(g_drag_start_x, g_drag_start_y, cx, cy, g_brush_color, g_brush_size);
                        else
                            draw_canvas_pixel(cx, cy, g_brush_color, g_brush_size);
                        g_drag_start_x = cx;
                        g_drag_start_y = cy;
                    } else if (g_tool_mode == 1) { /* Eraser */
                        if (g_drag_start_x >= 0)
                            draw_canvas_line(g_drag_start_x, g_drag_start_y, cx, cy, 0xFFFFFFFF, g_brush_size * 2);
                        else
                            draw_canvas_pixel(cx, cy, 0xFFFFFFFF, g_brush_size * 2);
                        g_drag_start_x = cx;
                        g_drag_start_y = cy;
                    } else if (g_tool_mode == 2) { /* Fill */
                        unsigned int target = g_canvas[cy * CANVAS_W + cx];
                        flood_fill(cx, cy, target, g_brush_color);
                        g_mouse_down = 0; /* one-shot */
                    } else if (g_tool_mode == 3 && g_drag_start_x >= 0) { /* Live Line Preview */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        draw_canvas_line(g_drag_start_x, g_drag_start_y, cx, cy, g_brush_color, g_brush_size);
                    } else if (g_tool_mode == 4 && g_drag_start_x >= 0) { /* Live Rect Preview */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        draw_canvas_rect(g_drag_start_x, g_drag_start_y, cx, cy, g_brush_color, g_brush_size);
                    } else if (g_tool_mode == 5 && g_drag_start_x >= 0) { /* Live Circle Preview */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        int r = isqrt((cx - g_drag_start_x)*(cx - g_drag_start_x) + (cy - g_drag_start_y)*(cy - g_drag_start_y));
                        draw_canvas_circle(g_drag_start_x, g_drag_start_y, r, g_brush_color, g_brush_size);
                    } else if (g_tool_mode == 6) { /* Picker */
                        g_brush_color = g_canvas[cy * CANVAS_W + cx];
                        g_tool_mode = 0; /* return to pen */
                        g_mouse_down = 0;
                    }
                }
                render_paint_ui();
            } else {
                if (g_mouse_down) {
                    /* On mouse release for geometric shapes */
                    int cx = mx - CANVAS_X;
                    int cy = my - CANVAS_Y;
                    if (g_tool_mode == 3 && g_drag_start_x >= 0) { /* Line */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        draw_canvas_line(g_drag_start_x, g_drag_start_y, cx, cy, g_brush_color, g_brush_size);
                    } else if (g_tool_mode == 4 && g_drag_start_x >= 0) { /* Rect */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        draw_canvas_rect(g_drag_start_x, g_drag_start_y, cx, cy, g_brush_color, g_brush_size);
                    } else if (g_tool_mode == 5 && g_drag_start_x >= 0) { /* Circle */
                        memcpy(g_canvas, g_undo_buf, sizeof(g_canvas));
                        int r = isqrt((cx - g_drag_start_x)*(cx - g_drag_start_x) + (cy - g_drag_start_y)*(cy - g_drag_start_y));
                        draw_canvas_circle(g_drag_start_x, g_drag_start_y, r, g_brush_color, g_brush_size);
                    }
                    g_mouse_down = 0;
                    g_drag_start_x = -1;
                    g_drag_start_y = -1;
                    render_paint_ui();
                }
            }
        }
    }

    return 0;
}
