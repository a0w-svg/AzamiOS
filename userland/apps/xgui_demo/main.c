/* ============================================================================
 * AzamiOS — POSIX X11 Interactive GUI Demonstration (xgui_demo)
 * File: userland/apps/xgui_demo/main.c
 *
 * Demonstrates POSIX Xlib programming, interactive drawing, color palettes,
 * geometric primitives, and event multiplexing.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define WIN_W 500
#define WIN_H 380

static const uint32_t g_palette[] = {
    0xFFF38BA8, /* Red */
    0xFFFAB387, /* Peach/Orange */
    0xFFF9E2AF, /* Yellow */
    0xFFA6E3A1, /* Green */
    0xFF89DCEB, /* Sky Blue */
    0xFF89B4FA, /* Blue */
    0xFFCBA6F7, /* Mauve/Purple */
    0xFFCDD6F4, /* White */
    0xFF11111B  /* Black/Eraser */
};
#define NUM_COLORS (sizeof(g_palette) / sizeof(g_palette[0]))

static uint32_t g_cur_color = 0xFF89B4FA;
static int g_brush_size = 3;
static int g_last_draw_x = -1;
static int g_last_draw_y = -1;

static void draw_ui_chrome(Display *d, Window w, GC gc)
{
    /* Top Toolbar */
    XSetForeground(d, gc, 0xFF181825);
    XFillRectangle(d, w, gc, 0, 0, WIN_W, 45);

    XSetForeground(d, gc, 0xFF313244);
    XDrawLine(d, w, gc, 0, 45, WIN_W, 45);

    /* Title text */
    XSetForeground(d, gc, 0xFFCDD6F4);
    XDrawString(d, w, gc, 12, 26, "X11 Paint & GUI Demo", 20);

    /* Color Palette Buttons */
    for (size_t i = 0; i < NUM_COLORS; i++) {
        int bx = 200 + (int)(i * 28);
        int by = 10;
        XSetForeground(d, gc, g_palette[i]);
        XFillRectangle(d, w, gc, bx, by, 22, 22);

        /* Highlight selected color */
        if (g_palette[i] == g_cur_color) {
            XSetForeground(d, gc, 0xFFFFFFFF);
            XDrawRectangle(d, w, gc, bx - 2, by - 2, 25, 25);
        } else {
            XSetForeground(d, gc, 0xFF45475A);
            XDrawRectangle(d, w, gc, bx, by, 22, 22);
        }
    }

    /* Status bar at bottom */
    XSetForeground(d, gc, 0xFF181825);
    XFillRectangle(d, w, gc, 0, WIN_H - 25, WIN_W, 25);

    XSetForeground(d, gc, 0xFF313244);
    XDrawLine(d, w, gc, 0, WIN_H - 25, WIN_W, WIN_H - 25);

    XSetForeground(d, gc, 0xFFA6ADC8);
    XDrawString(d, w, gc, 10, WIN_H - 8, "Left Click: Draw | Press 'C': Clear | 'Q': Quit", 46);

    XFlush(d);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[xgui_demo] Starting POSIX X11 Interactive Demonstration...\n");

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "[xgui_demo] Error: Cannot open display\n");
        return 1;
    }

    Window root = XDefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 120, 120, WIN_W, WIN_H, 1, 0xFF45475A, 0xFF1E1E2E);
    XStoreName(d, w, "X11 GUI Demo (POSIX)");
    XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
    XMapWindow(d, w);

    GC gc = XCreateGC(d, w, 0, NULL);
    draw_ui_chrome(d, w, gc);

    int drawing = 0;
    XEvent ev;

    for (;;) {
        XNextEvent(d, &ev);

        if (ev.type == Expose) {
            draw_ui_chrome(d, w, gc);
        } else if (ev.type == ButtonPress) {
            int mx = ev.xbutton.x;
            int my = ev.xbutton.y;

            /* Check if clicked on color palette in top toolbar */
            if (my < 45) {
                for (size_t i = 0; i < NUM_COLORS; i++) {
                    int bx = 200 + (int)(i * 28);
                    int by = 10;
                    if (mx >= bx && mx < bx + 22 && my >= by && my < by + 22) {
                        g_cur_color = g_palette[i];
                        draw_ui_chrome(d, w, gc);
                        break;
                    }
                }
            } else if (my >= 45 && my < WIN_H - 25) {
                drawing = 1;
                g_last_draw_x = mx;
                g_last_draw_y = my;
                XSetForeground(d, gc, g_cur_color);
                XFillArc(d, w, gc, mx - g_brush_size, my - g_brush_size, g_brush_size * 2, g_brush_size * 2, 0, 360 * 64);
                XFlush(d);
            }
        } else if (ev.type == ButtonRelease) {
            drawing = 0;
            g_last_draw_x = -1;
            g_last_draw_y = -1;
        } else if (ev.type == MotionNotify) {
            int mx = ev.xmotion.x;
            int my = ev.xmotion.y;

            if (drawing && my >= 45 && my < WIN_H - 25) {
                XSetForeground(d, gc, g_cur_color);
                if (g_last_draw_x >= 0 && g_last_draw_y >= 0) {
                    XDrawLine(d, w, gc, g_last_draw_x, g_last_draw_y, mx, my);
                    XFillArc(d, w, gc, mx - g_brush_size, my - g_brush_size, g_brush_size * 2, g_brush_size * 2, 0, 360 * 64);
                }
                g_last_draw_x = mx;
                g_last_draw_y = my;
                XFlush(d);
            }
        } else if (ev.type == KeyPress) {
            KeySym ks;
            char buf[8];
            if (XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL) > 0) {
                if (buf[0] == 0x1B || buf[0] == 'q' || buf[0] == 'Q') break;
                if (buf[0] == 'c' || buf[0] == 'C') {
                    /* Clear canvas */
                    XSetForeground(d, gc, 0xFF1E1E2E);
                    XFillRectangle(d, w, gc, 0, 46, WIN_W, WIN_H - 71);
                    draw_ui_chrome(d, w, gc);
                }
            }
        }
    }

    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
