/* ============================================================================
 * AzamiOS — Standard POSIX X11 Utilities Header
 * File: userland/libc/include/X11/Xutil.h
 * ============================================================================ */
#ifndef _X11_XUTIL_H_
#define _X11_XUTIL_H_

#include "Xlib.h"
#include "keysym.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Size Hints ───────────────────────────────────────────────────────────── */
#define USPosition  (1L << 0)
#define USSize      (1L << 1)
#define PPosition   (1L << 2)
#define PSize       (1L << 3)
#define PMinSize    (1L << 4)
#define PMaxSize    (1L << 5)
#define PResizeInc  (1L << 6)
#define PAspect     (1L << 7)
#define PBaseSize   (1L << 8)
#define PWinGravity (1L << 9)

typedef struct {
    long flags;
    int x, y;
    int width, height;
    int min_width, min_height;
    int max_width, max_height;
    int width_inc, height_inc;
    struct {
        int x;
        int y;
    } min_aspect, max_aspect;
    int base_width, base_height;
    int win_gravity;
} XSizeHints;

/* ── Window Manager Hints ─────────────────────────────────────────────────── */
#define InputHint        (1L << 0)
#define StateHint        (1L << 1)
#define IconPixmapHint   (1L << 2)
#define IconWindowHint   (1L << 3)
#define IconPositionHint (1L << 4)
#define IconMaskHint     (1L << 5)
#define WindowGroupHint  (1L << 6)
#define AllHints         (InputHint|StateHint|IconPixmapHint|IconWindowHint|IconPositionHint|IconMaskHint|WindowGroupHint)

#define WithdrawnState 0
#define NormalState    1
#define IconicState    3

typedef struct {
    long flags;
    Bool input;
    int initial_state;
    Pixmap icon_pixmap;
    Window icon_window;
    int icon_x, icon_y;
    Pixmap icon_mask;
    XID window_group;
} XWMHints;

/* ── Class Hint ───────────────────────────────────────────────────────────── */
typedef struct {
    char *res_name;
    char *res_class;
} XClassHint;

/* ── Text Property ────────────────────────────────────────────────────────── */
typedef struct {
    unsigned char *value;
    Atom encoding;
    int format;
    unsigned long nitems;
} XTextProperty;

/* ── Utility Function Prototypes ─────────────────────────────────────────── */
XSizeHints *XAllocSizeHints(void);
XWMHints   *XAllocWMHints(void);
XClassHint *XAllocClassHint(void);

void XSetWMProperties(Display *display, Window w,
                      XTextProperty *window_name, XTextProperty *icon_name,
                      char **argv, int argc,
                      XSizeHints *normal_hints, XWMHints *wm_hints,
                      XClassHint *class_hints);

void XSetWMNormalHints(Display *display, Window w, XSizeHints *hints);
void XSetWMHints(Display *display, Window w, XWMHints *hints);
void XSetClassHint(Display *display, Window w, XClassHint *class_hints);

int XLookupString(XKeyEvent *event_struct, char *buffer_return,
                  int bytes_buffer, KeySym *keysym_return,
                  void *status_in_out);

#ifdef __cplusplus
}
#endif

#endif /* _X11_XUTIL_H_ */
