/* ============================================================================
 * AzamiOS — Standard POSIX X11 Calculator (xcalc)
 * File: userland/apps/xcalc/main.c
 *
 * Classic scientific/standard desktop calculator built on POSIX X11/Xlib.h APIs.
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define WIN_W 220
#define WIN_H 300

typedef struct {
    int x, y, w, h;
    const char *label;
    uint32_t bg;
    uint32_t fg;
} btn_t;

static const btn_t g_btns[] = {
    { 10,  60, 45, 38, "C",   0xFFF38BA8, 0xFF11111B },
    { 60,  60, 45, 38, "+/-", 0xFF45475A, 0xFFCDD6F4 },
    { 110, 60, 45, 38, "%",   0xFF45475A, 0xFFCDD6F4 },
    { 160, 60, 45, 38, "/",   0xFFFAB387, 0xFF11111B },

    { 10, 105, 45, 38, "7",   0xFF313244, 0xFFCDD6F4 },
    { 60, 105, 45, 38, "8",   0xFF313244, 0xFFCDD6F4 },
    { 110,105, 45, 38, "9",   0xFF313244, 0xFFCDD6F4 },
    { 160,105, 45, 38, "*",   0xFFFAB387, 0xFF11111B },

    { 10, 150, 45, 38, "4",   0xFF313244, 0xFFCDD6F4 },
    { 60, 150, 45, 38, "5",   0xFF313244, 0xFFCDD6F4 },
    { 110,150, 45, 38, "6",   0xFF313244, 0xFFCDD6F4 },
    { 160,150, 45, 38, "-",   0xFFFAB387, 0xFF11111B },

    { 10, 195, 45, 38, "1",   0xFF313244, 0xFFCDD6F4 },
    { 60, 195, 45, 38, "2",   0xFF313244, 0xFFCDD6F4 },
    { 110,195, 45, 38, "3",   0xFF313244, 0xFFCDD6F4 },
    { 160,195, 45, 38, "+",   0xFFFAB387, 0xFF11111B },

    { 10, 240, 95, 38, "0",   0xFF313244, 0xFFCDD6F4 },
    { 110,240, 45, 38, ".",   0xFF313244, 0xFFCDD6F4 },
    { 160,240, 45, 38, "=",   0xFFA6E3A1, 0xFF11111B },
};

static char g_display_str[32] = "0";
static double g_accum = 0;
static char g_pending_op = '\0';
static int g_clear_on_next = 0;

static void draw_calculator(Display *d, Window w, GC gc)
{
    /* Window background */
    XSetForeground(d, gc, 0xFF1E1E2E);
    XFillRectangle(d, w, gc, 0, 0, WIN_W, WIN_H);

    /* LCD Screen Display */
    XSetForeground(d, gc, 0xFF11111B);
    XFillRectangle(d, w, gc, 10, 10, WIN_W - 20, 40);

    XSetForeground(d, gc, 0xFF45475A);
    XDrawRectangle(d, w, gc, 10, 10, WIN_W - 20, 40);

    /* LCD Text */
    XSetForeground(d, gc, 0xFFCDD6F4);
    int txt_len = (int)strlen(g_display_str);
    int tx = WIN_W - 20 - (txt_len * 8) - 8;
    if (tx < 15) tx = 15;
    XDrawString(d, w, gc, tx, 35, g_display_str, txt_len);

    /* Draw Buttons */
    int num_btns = sizeof(g_btns) / sizeof(g_btns[0]);
    for (int i = 0; i < num_btns; i++) {
        const btn_t *b = &g_btns[i];
        XSetForeground(d, gc, b->bg);
        XFillRectangle(d, w, gc, b->x, b->y, b->w, b->h);

        XSetForeground(d, gc, 0xFF585B70);
        XDrawRectangle(d, w, gc, b->x, b->y, b->w, b->h);

        XSetForeground(d, gc, b->fg);
        int lbl_len = (int)strlen(b->label);
        int lx = b->x + (b->w - lbl_len * 8) / 2;
        int ly = b->y + b->h / 2 + 5;
        XDrawString(d, w, gc, lx, ly, b->label, lbl_len);
    }

    XFlush(d);
}

static void handle_click(const char *label)
{
    if (strcmp(label, "C") == 0) {
        strcpy(g_display_str, "0");
        g_accum = 0;
        g_pending_op = '\0';
        g_clear_on_next = 0;
    } else if (label[0] >= '0' && label[0] <= '9') {
        if (g_clear_on_next || strcmp(g_display_str, "0") == 0) {
            snprintf(g_display_str, sizeof(g_display_str), "%s", label);
            g_clear_on_next = 0;
        } else if (strlen(g_display_str) < 14) {
            strcat(g_display_str, label);
        }
    } else if (strcmp(label, ".") == 0) {
        if (!strchr(g_display_str, '.')) {
            strcat(g_display_str, ".");
        }
    } else if (strchr("+-*/", label[0])) {
        g_accum = atof(g_display_str);
        g_pending_op = label[0];
        g_clear_on_next = 1;
    } else if (strcmp(label, "=") == 0) {
        if (g_pending_op != '\0') {
            double cur = atof(g_display_str);
            double res = g_accum;
            if (g_pending_op == '+') res += cur;
            else if (g_pending_op == '-') res -= cur;
            else if (g_pending_op == '*') res *= cur;
            else if (g_pending_op == '/' && cur != 0) res /= cur;

            snprintf(g_display_str, sizeof(g_display_str), "%.6g", res);
            g_pending_op = '\0';
            g_clear_on_next = 1;
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("[xcalc] Starting POSIX X11 Calculator...\n");

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "[xcalc] Error: Cannot open display\n");
        return 1;
    }

    Window root = XDefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 150, 150, WIN_W, WIN_H, 1, 0xFF45475A, 0xFF1E1E2E);
    XStoreName(d, w, "XCalc (POSIX X11)");
    XSelectInput(d, w, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    XMapWindow(d, w);

    GC gc = XCreateGC(d, w, 0, NULL);
    draw_calculator(d, w, gc);

    XEvent ev;
    for (;;) {
        XNextEvent(d, &ev);
        if (ev.type == Expose) {
            draw_calculator(d, w, gc);
        } else if (ev.type == ButtonPress) {
            int mx = ev.xbutton.x;
            int my = ev.xbutton.y;
            int num_btns = sizeof(g_btns) / sizeof(g_btns[0]);
            for (int i = 0; i < num_btns; i++) {
                const btn_t *b = &g_btns[i];
                if (mx >= b->x && mx < b->x + b->w && my >= b->y && my < b->y + b->h) {
                    handle_click(b->label);
                    draw_calculator(d, w, gc);
                    break;
                }
            }
        } else if (ev.type == KeyPress) {
            char buf[8];
            KeySym ks;
            if (XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL) > 0) {
                if (buf[0] == 0x1B || buf[0] == 'q' || buf[0] == 'Q') break;
                if ((buf[0] >= '0' && buf[0] <= '9') || strchr("+-*/.=", buf[0])) {
                    char l[2] = { buf[0], '\0' };
                    handle_click(l);
                    draw_calculator(d, w, gc);
                } else if (buf[0] == '\r' || buf[0] == '\n') {
                    handle_click("=");
                    draw_calculator(d, w, gc);
                } else if (buf[0] == 'c' || buf[0] == 'C') {
                    handle_click("C");
                    draw_calculator(d, w, gc);
                }
            }
        }
    }

    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    XCloseDisplay(d);
    return 0;
}
