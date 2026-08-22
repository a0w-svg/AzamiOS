/* ============================================================================
 * AzamiOS — Standard POSIX X11 Eyes (xeyes)
 * File: userland/apps/xeyes/main.c
 *
 * Classic interactive mouse-following eyes app built on standard POSIX Xlib.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define WIN_W 200
#define WIN_H 120

static void draw_eye(Display *d, Window w, GC gc, int cx, int cy, int rx, int ry, int target_x, int target_y)
{
    /* Eye sclera outline & white fill */
    XSetForeground(d, gc, 0xFF45475A);
    XFillArc(d, w, gc, cx - rx - 2, cy - ry - 2, (rx + 2) * 2, (ry + 2) * 2, 0, 360 * 64);

    XSetForeground(d, gc, 0xFFEFF1F5); /* White sclera */
    XFillArc(d, w, gc, cx - rx, cy - ry, rx * 2, ry * 2, 0, 360 * 64);

    XSetForeground(d, gc, 0xFF89B4FA); /* Blue outline */
    XDrawArc(d, w, gc, cx - rx, cy - ry, rx * 2, ry * 2, 0, 360 * 64);

    /* Pupil position calculation */
    double dx = target_x - cx;
    double dy = target_y - cy;
    double dist = sqrt(dx * dx + dy * dy);

    int pupil_r = 10;
    int max_offset_x = rx - pupil_r - 4;
    int max_offset_y = ry - pupil_r - 4;

    int px = cx;
    int py = cy;

    if (dist > 0.001) {
        double nx = dx / dist;
        double ny = dy / dist;
        double offset = (dist < (double)max_offset_x) ? dist : (double)max_offset_x;
        px = cx + (int)(nx * offset);
        py = cy + (int)(ny * (offset * ((double)max_offset_y / (double)max_offset_x)));
    }

    /* Draw black pupil with white specular highlight */
    XSetForeground(d, gc, 0xFF11111B);
    XFillArc(d, w, gc, px - pupil_r, py - pupil_r, pupil_r * 2, pupil_r * 2, 0, 360 * 64);

    XSetForeground(d, gc, 0xFFFFFFFF);
    XFillArc(d, w, gc, px - pupil_r / 2, py - pupil_r / 2, 4, 4, 0, 360 * 64);
}

static void draw_eyes(Display *d, Window w, GC gc, int mx, int my)
{
    /* Clear window */
    XSetForeground(d, gc, 0xFF1E1E2E);
    XFillRectangle(d, w, gc, 0, 0, WIN_W, WIN_H);

    /* Left eye */
    draw_eye(d, w, gc, 55, 60, 40, 48, mx, my);

    /* Right eye */
    draw_eye(d, w, gc, 145, 60, 40, 48, mx, my);

    XFlush(d);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[xeyes] Starting POSIX X11 Eyes...\n");

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "[xeyes] Error: Cannot open display\n");
        return 1;
    }

    Window root = XDefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 200, 200, WIN_W, WIN_H, 1, 0xFF45475A, 0xFF1E1E2E);
    XStoreName(d, w, "XEyes (POSIX X11)");
    XSelectInput(d, w, ExposureMask | PointerMotionMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(d, w);

    GC gc = XCreateGC(d, w, 0, NULL);
    draw_eyes(d, w, gc, WIN_W / 2, WIN_H / 2);

    int mx = WIN_W / 2;
    int my = WIN_H / 2;

    for (;;) {
        XEvent ev;
        XNextEvent(d, &ev);

        if (ev.type == MotionNotify) {
            mx = ev.xmotion.x;
            my = ev.xmotion.y;
            draw_eyes(d, w, gc, mx, my);
        } else if (ev.type == Expose) {
            draw_eyes(d, w, gc, mx, my);
        } else if (ev.type == KeyPress) {
            KeySym ks;
            char buf[8];
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Escape || ks == XK_q || ks == XK_Q) break;
        }
    }

    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
