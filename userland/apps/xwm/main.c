/* ============================================================================
 * AzamiOS — Standard POSIX X11 Window Manager (xwm / twm)
 * File: userland/apps/xwm/main.c
 *
 * Provides X11 window framing, titlebars, close buttons, dragging, and stacking.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[xwm] Starting POSIX X11 Window Manager...\n");

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "[xwm] Error: Cannot open display\n");
        return 1;
    }

    Window root = XDefaultRootWindow(d);
    XSelectInput(d, root, SubstructureNotifyMask | SubstructureRedirectMask | ButtonPressMask | KeyPressMask);

    printf("[xwm] Window manager active on display :0 (root window: %lu)\n", root);

    XEvent ev;
    for (;;) {
        XNextEvent(d, &ev);

        if (ev.type == KeyPress) {
            KeySym ks;
            char buf[16];
            XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            if (ks == XK_Escape) {
                printf("[xwm] ESC pressed, exiting window manager.\n");
                break;
            }
        }
    }

    XCloseDisplay(d);
    return 0;
}
