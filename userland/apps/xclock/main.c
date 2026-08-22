/* ============================================================================
 * AzamiOS — Standard POSIX X11 Clock (xclock)
 * File: userland/apps/xclock/main.c
 *
 * Classic analog & digital clock written using standard POSIX X11/Xlib.h APIs.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define WIN_W 240
#define WIN_H 240

static void draw_clock(Display *d, Window w, GC gc, struct tm *tm)
{
    /* Clear background */
    XSetForeground(d, gc, 0xFF1E1E2E); /* Base background */
    XFillRectangle(d, w, gc, 0, 0, WIN_W, WIN_H);

    int cx = WIN_W / 2;
    int cy = WIN_H / 2 - 10;
    int radius = 80;

    /* Draw outer clock circle */
    XSetForeground(d, gc, 0xFF45475A); /* Surface1 */
    XFillArc(d, w, gc, cx - radius - 4, cy - radius - 4, (radius + 4) * 2, (radius + 4) * 2, 0, 360 * 64);

    XSetForeground(d, gc, 0xFF181825); /* Mantle face */
    XFillArc(d, w, gc, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360 * 64);

    XSetForeground(d, gc, 0xFF89B4FA); /* Blue border */
    XDrawArc(d, w, gc, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360 * 64);

    /* Draw hour tick marks */
    XSetForeground(d, gc, 0xFFCDD6F4); /* Text color */
    for (int h = 0; h < 12; h++) {
        double angle = h * (M_PI / 6.0) - (M_PI / 2.0);
        int x1 = cx + (int)((radius - 12) * cos(angle));
        int y1 = cy + (int)((radius - 12) * sin(angle));
        int x2 = cx + (int)((radius - 4) * cos(angle));
        int y2 = cy + (int)((radius - 4) * sin(angle));
        XDrawLine(d, w, gc, x1, y1, x2, y2);
    }

    /* Calculate hand angles */
    double sec_ang = tm->tm_sec * (M_PI / 30.0) - (M_PI / 2.0);
    double min_ang = (tm->tm_min + tm->tm_sec / 60.0) * (M_PI / 30.0) - (M_PI / 2.0);
    double hr_ang  = ((tm->tm_hour % 12) + tm->tm_min / 60.0) * (M_PI / 6.0) - (M_PI / 2.0);

    /* Hour hand (Thick lavender) */
    XSetForeground(d, gc, 0xFFB4BEFE);
    int hx = cx + (int)((radius * 0.5) * cos(hr_ang));
    int hy = cy + (int)((radius * 0.5) * sin(hr_ang));
    XDrawLine(d, w, gc, cx, cy, hx, hy);
    XDrawLine(d, w, gc, cx + 1, cy, hx + 1, hy);
    XDrawLine(d, w, gc, cx, cy + 1, hx, hy + 1);

    /* Minute hand (Cyan/Sky) */
    XSetForeground(d, gc, 0xFF89DCEB);
    int mx = cx + (int)((radius * 0.75) * cos(min_ang));
    int my = cy + (int)((radius * 0.75) * sin(min_ang));
    XDrawLine(d, w, gc, cx, cy, mx, my);
    XDrawLine(d, w, gc, cx + 1, cy, mx + 1, my);

    /* Second hand (Peach/Red) */
    XSetForeground(d, gc, 0xFFF38BA8);
    int sx = cx + (int)((radius * 0.85) * cos(sec_ang));
    int sy = cy + (int)((radius * 0.85) * sin(sec_ang));
    XDrawLine(d, w, gc, cx, cy, sx, sy);

    /* Center pivot pin */
    XSetForeground(d, gc, 0xFFF9E2AF); /* Yellow */
    XFillArc(d, w, gc, cx - 3, cy - 3, 6, 6, 0, 360 * 64);

    /* Digital time string */
    char timestr[32];
    snprintf(timestr, sizeof(timestr), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
    XSetForeground(d, gc, 0xFFA6E3A1); /* Green */
    XDrawString(d, w, gc, cx - 32, WIN_H - 18, timestr, (int)strlen(timestr));

    XFlush(d);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[xclock] Starting POSIX X11 Clock...\n");

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "[xclock] Error: Cannot open display\n");
        return 1;
    }

    Window root = XDefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 100, 100, WIN_W, WIN_H, 1, 0xFF45475A, 0xFF1E1E2E);
    XStoreName(d, w, "XClock (POSIX X11)");
    XSelectInput(d, w, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(d, w);

    GC gc = XCreateGC(d, w, 0, NULL);

    time_t rawtime;
    struct tm *timeinfo;

    time(&rawtime);
    timeinfo = localtime(&rawtime);
    draw_clock(d, w, gc, timeinfo);

    int last_sec = -1;
    for (;;) {
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        if (timeinfo->tm_sec != last_sec) {
            last_sec = timeinfo->tm_sec;
            draw_clock(d, w, gc, timeinfo);
        }

        /* Check for exit key */
        XEvent ev;
        if (XPending(d)) {
            XNextEvent(d, &ev);
            if (ev.type == KeyPress) {
                KeySym ks;
                char buf[8];
                XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
                if (ks == XK_Escape || ks == XK_q || ks == XK_Q) break;
            } else if (ev.type == Expose) {
                draw_clock(d, w, gc, timeinfo);
            }
        }

        usleep(50000); /* 50ms tick */
    }

    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
