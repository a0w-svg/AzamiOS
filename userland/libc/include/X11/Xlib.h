/* ============================================================================
 * AzamiOS — Standard POSIX Xlib C API Header
 * File: userland/libc/include/X11/Xlib.h
 * ============================================================================ */
#ifndef _X11_XLIB_H_
#define _X11_XLIB_H_

#include "X.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Forward Typedef Declarations ────────────────────────────────────────── */
struct _XDisplay;
typedef struct _XDisplay Display;

struct _XGC;
typedef struct _XGC *GC;

/* ── Geometry & Primitives ────────────────────────────────────────────────── */
typedef struct {
    short x, y;
} XPoint;

typedef struct {
    short x, y;
    unsigned short width, height;
} XRectangle;

typedef struct {
    short x, y;
    unsigned short width, height;
    short angle1, angle2;
} XArc;

typedef struct {
    short x1, y1, x2, y2;
} XSegment;

/* ── Visual, Depth & Screen ──────────────────────────────────────────────── */
typedef struct {
    VisualID visualid;
    int c_class;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    int bits_per_rgb;
    int map_entries;
} Visual;

typedef struct {
    int depth;
    int nvisuals;
    Visual *visuals;
} Depth;

typedef struct {
    struct _XDisplay *display;
    Window root;
    int width, height;
    int mwidth, mheight;
    int ndepths;
    Depth *depths;
    int root_depth;
    Visual *root_visual;
    GC default_gc;
    Colormap cmap;
    unsigned long white_pixel;
    unsigned long black_pixel;
    int max_maps, min_maps;
    int backing_store;
    Bool save_unders;
    long root_input_mask;
} Screen;

/* ── Graphics Context Values & GC ────────────────────────────────────────── */
typedef struct {
    int function;
    unsigned long plane_mask;
    unsigned long foreground;
    unsigned long background;
    int line_width;
    int line_style;
    int cap_style;
    int join_style;
    int fill_style;
    int fill_rule;
    int arc_mode;
    Pixmap tile;
    Pixmap stipple;
    int ts_x_origin;
    int ts_y_origin;
    Font font;
    int subwindow_mode;
    Bool graphics_exposures;
    int clip_x_origin;
    int clip_y_origin;
    Pixmap clip_mask;
    int dash_offset;
    char dashes;
} XGCValues;

typedef struct _XGC {
    Display *display;
    Drawable drawable;
    XGCValues values;
    unsigned long mask;
} *GC;

/* ── Window Attributes ───────────────────────────────────────────────────── */
typedef struct {
    Pixmap background_pixmap;
    unsigned long background_pixel;
    Pixmap border_pixmap;
    unsigned long border_pixel;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    long event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    Colormap colormap;
    Cursor cursor;
} XSetWindowAttributes;

typedef struct {
    int x, y;
    int width, height;
    int border_width;
    int depth;
    Visual *visual;
    Window root;
    int c_class;
    int bit_gravity;
    int win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    Bool save_under;
    Colormap colormap;
    Bool map_installed;
    int map_state;
    long all_event_masks;
    long your_event_mask;
    long do_not_propagate_mask;
    Bool override_redirect;
    Screen *screen;
} XWindowAttributes;

/* ── Image Structure ─────────────────────────────────────────────────────── */
typedef struct _XImage {
    int width, height;
    int xoffset;
    int format;
    char *data;
    int byte_order;
    int bitmap_unit;
    int bitmap_bit_order;
    int bitmap_pad;
    int depth;
    int bytes_per_line;
    int bits_per_pixel;
    unsigned long red_mask;
    unsigned long green_mask;
    unsigned long blue_mask;
    void *obdata;
} XImage;

/* ── Events ──────────────────────────────────────────────────────────────── */
typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
} XAnyEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int keycode;
    Bool same_screen;
} XKeyEvent;
typedef XKeyEvent XKeyPressedEvent;
typedef XKeyEvent XKeyReleasedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int button;
    Bool same_screen;
} XButtonEvent;
typedef XButtonEvent XButtonPressedEvent;
typedef XButtonEvent XButtonReleasedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
    Window root;
    Window subwindow;
    Time time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    char is_hint;
    Bool same_screen;
} XMotionEvent;
typedef XMotionEvent XPointerMovedEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
    int x, y;
    int width, height;
    int count;
} XExposeEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window event;
    Window window;
    int x, y;
    int width, height;
    int border_width;
    Window above;
    Bool override_redirect;
} XConfigureEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window event;
    Window window;
} XDestroyWindowEvent;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    struct _XDisplay *display;
    Window window;
    Atom message_type;
    int format;
    union {
        char b[20];
        short s[10];
        long l[5];
    } data;
} XClientMessageEvent;

typedef union _XEvent {
    int type;
    XAnyEvent xany;
    XKeyEvent xkey;
    XButtonEvent xbutton;
    XMotionEvent xmotion;
    XExposeEvent xexpose;
    XConfigureEvent xconfigure;
    XDestroyWindowEvent xdestroywindow;
    XClientMessageEvent xclient;
    long pad[24];
} XEvent;

/* ── Display Structure ───────────────────────────────────────────────────── */
struct _XDisplay {
    int fd;                    /* POSIX connection file descriptor / IPC channel */
    int screen_num;
    Screen *screens;
    int nscreens;
    Window default_root;
    Visual default_visual;
    unsigned long next_xid;
    int width, height;
    void *client_state;        /* Azami internal window manager connection state */
};

/* ── Standard POSIX Xlib Function Prototypes ─────────────────────────────── */

/* Display Connection */
Display *XOpenDisplay(const char *display_name);
int      XCloseDisplay(Display *display);
int      XConnectionNumber(Display *display);
int      XDefaultScreen(Display *display);
Screen  *XDefaultScreenOfDisplay(Display *display);
Screen  *XScreenOfDisplay(Display *display, int screen_number);
Window   XRootWindow(Display *display, int screen_number);
Window   XDefaultRootWindow(Display *display);
Visual  *XDefaultVisual(Display *display, int screen_number);
GC       XDefaultGC(Display *display, int screen_number);
int      XDefaultDepth(Display *display, int screen_number);
int      XDisplayWidth(Display *display, int screen_number);
int      XDisplayHeight(Display *display, int screen_number);
unsigned long XBlackPixel(Display *display, int screen_number);
unsigned long XWhitePixel(Display *display, int screen_number);

/* Window Management */
Window XCreateWindow(Display *display, Window parent, int x, int y,
                     unsigned int width, unsigned int height, unsigned int border_width,
                     int depth, unsigned int c_class, Visual *visual,
                     unsigned long valuemask, XSetWindowAttributes *attributes);

Window XCreateSimpleWindow(Display *display, Window parent, int x, int y,
                           unsigned int width, unsigned int height, unsigned int border_width,
                           unsigned long border, unsigned long background);

int XDestroyWindow(Display *display, Window w);
int XMapWindow(Display *display, Window w);
int XMapRaised(Display *display, Window w);
int XUnmapWindow(Display *display, Window w);
int XMoveWindow(Display *display, Window w, int x, int y);
int XResizeWindow(Display *display, Window w, unsigned int width, unsigned int height);
int XMoveResizeWindow(Display *display, Window w, int x, int y, unsigned int width, unsigned int height);
int XStoreName(Display *display, Window w, const char *window_name);
int XFetchName(Display *display, Window w, char **window_name_return);
int XSelectInput(Display *display, Window w, long event_mask);
int XClearWindow(Display *display, Window w);
int XClearArea(Display *display, Window w, int x, int y, unsigned int width, unsigned int height, Bool exposures);
int XGetWindowAttributes(Display *display, Window w, XWindowAttributes *window_attributes_return);
int XGetGeometry(Display *display, Drawable d, Window *root_return, int *x_return, int *y_return,
                 unsigned int *width_return, unsigned int *height_return,
                 unsigned int *border_width_return, unsigned int *depth_return);

/* Event Handling */
int  XNextEvent(Display *display, XEvent *event_return);
int  XPending(Display *display);
int  XEventsQueued(Display *display, int mode);
Bool XCheckTypedEvent(Display *display, int event_type, XEvent *event_return);
Bool XCheckMaskEvent(Display *display, long event_mask, XEvent *event_return);
Bool XCheckWindowEvent(Display *display, Window w, long event_mask, XEvent *event_return);
int  XSendEvent(Display *display, Window w, Bool propagate, long event_mask, XEvent *event_send);
int  XFlush(Display *display);
int  XSync(Display *display, Bool discard);

/* Graphics Context & Drawing Primitives */
GC   XCreateGC(Display *display, Drawable d, unsigned long valuemask, XGCValues *values);
int  XFreeGC(Display *display, GC gc);
int  XSetForeground(Display *display, GC gc, unsigned long foreground);
int  XSetBackground(Display *display, GC gc, unsigned long background);
int  XSetFunction(Display *display, GC gc, int function);
int  XSetLineAttributes(Display *display, GC gc, unsigned int line_width, int line_style, int cap_style, int join_style);
int  XDrawPoint(Display *display, Drawable d, GC gc, int x, int y);
int  XDrawPoints(Display *display, Drawable d, GC gc, XPoint *points, int npoints, int mode);
int  XDrawLine(Display *display, Drawable d, GC gc, int x1, int y1, int x2, int y2);
int  XDrawLines(Display *display, Drawable d, GC gc, XPoint *points, int npoints, int mode);
int  XDrawRectangle(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height);
int  XFillRectangle(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height);
int  XFillRectangles(Display *display, Drawable d, GC gc, XRectangle *rectangles, int nrectangles);
int  XDrawArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2);
int  XFillArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2);
int  XDrawString(Display *display, Drawable d, GC gc, int x, int y, const char *string, int length);
int  XDrawImageString(Display *display, Drawable d, GC gc, int x, int y, const char *string, int length);

/* Images */
XImage *XCreateImage(Display *display, Visual *visual, unsigned int depth, int format,
                     int offset, char *data, unsigned int width, unsigned int height,
                     int bitmap_pad, int bytes_per_line);
int     XDestroyImage(XImage *image);
int     XPutImage(Display *display, Drawable d, GC gc, XImage *image,
                  int src_x, int src_y, int dest_x, int dest_y,
                  unsigned int width, unsigned int height);
XImage *XGetImage(Display *display, Drawable d, int x, int y,
                  unsigned int width, unsigned int height,
                  unsigned long plane_mask, int format);

/* Atoms & Properties */
Atom  XInternAtom(Display *display, const char *atom_name, Bool only_if_exists);
char *XGetAtomName(Display *display, Atom atom);

#ifdef __cplusplus
}
#endif

#endif /* _X11_XLIB_H_ */
