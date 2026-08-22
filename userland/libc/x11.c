/* ============================================================================
 * AzamiOS — Standard POSIX libX11 Client Implementation
 * File: userland/libc/x11.c
 *
 * Implements the POSIX X11 client library (libX11) interface, bridging standard
 * X11 client calls to the Azami Window Manager / X server.
 * ============================================================================ */

#include "include/X11/Xlib.h"
#include "include/X11/Xutil.h"
#include "include/X11/keysym.h"
#include "include/X11/Xatom.h"
#include "include/X11/Xproto.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/stdio.h"
#include "include/az/ipc.h"
#include "../apps/azwm/protocol.h"

#define MAX_WINDOWS_PER_CLIENT 32

typedef struct {
    Window       wid;
    unsigned int assigned_wid;
    unsigned int width;
    unsigned int height;
    unsigned int shmem_id;
    uint32_t    *pixels;
    char         title[64];
    long         event_mask;
    int          x, y;
    Bool         mapped;
    unsigned long background;
    unsigned long border;
} x11_win_entry_t;

typedef struct {
    unsigned int    client_chan;
    unsigned int    server_chan;
    int             screen_w;
    int             screen_h;
    x11_win_entry_t windows[MAX_WINDOWS_PER_CLIENT];
    int             num_windows;
    XEvent          event_queue[32];
    int             event_head;
    int             event_tail;
    int             event_count;
} x11_client_state_t;

/* ── Built-in 8x16 Monospace ASCII Bitmap Font ───────────────────────────── */
static const unsigned char g_x11_font_8x16[128][16] = {
    [' ']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!']  = {0x00,0x00,0x18,0x3c,0x3c,0x3c,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    ['"']  = {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['#']  = {0x00,0x00,0x6c,0x6c,0xfe,0x6c,0x6c,0x6c,0xfe,0x6c,0x6c,0x00,0x00,0x00,0x00,0x00},
    ['$']  = {0x18,0x18,0x7c,0xc6,0xc2,0xc0,0x7c,0x06,0x06,0x86,0xc6,0x7c,0x18,0x18,0x00,0x00},
    ['%']  = {0x00,0x00,0xc6,0xcc,0x18,0x30,0x60,0xc6,0x8c,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['&']  = {0x00,0x38,0x6c,0x6c,0x38,0x76,0xdc,0xcc,0xcc,0x76,0x00,0x00,0x00,0x00,0x00,0x00},
    ['\''] = {0x00,0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['(']  = {0x00,0x0c,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0c,0x00,0x00,0x00,0x00,0x00},
    [')']  = {0x00,0x30,0x18,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    ['*']  = {0x00,0x00,0x00,0x66,0x3c,0xff,0x3c,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['+']  = {0x00,0x00,0x18,0x18,0x18,0xff,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [',']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x08,0x10,0x00,0x00,0x00},
    ['-']  = {0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['.']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    ['/']  = {0x00,0x00,0x02,0x06,0x0c,0x18,0x30,0x60,0xc0,0x80,0x00,0x00,0x00,0x00,0x00,0x00},
    ['0']  = {0x00,0x00,0x7c,0xc6,0xce,0xde,0xf6,0xe6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['1']  = {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['2']  = {0x00,0x00,0x7c,0xc6,0x06,0x0c,0x18,0x30,0x60,0xfe,0x00,0x00,0x00,0x00,0x00,0x00},
    ['3']  = {0x00,0x00,0x7c,0xc6,0x06,0x1c,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['4']  = {0x00,0x00,0x0c,0x1c,0x3c,0x6c,0xcc,0xfe,0x0c,0x1e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['5']  = {0x00,0x00,0xfe,0xc0,0xc0,0xfc,0x06,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['6']  = {0x00,0x00,0x38,0x60,0xc0,0xfc,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['7']  = {0x00,0x00,0xfe,0xc6,0x06,0x0c,0x18,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00},
    ['8']  = {0x00,0x00,0x7c,0xc6,0xc6,0x7c,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['9']  = {0x00,0x00,0x7c,0xc6,0xc6,0xc6,0x7e,0x06,0x0c,0x78,0x00,0x00,0x00,0x00,0x00,0x00},
    [':']  = {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    [';']  = {0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x08,0x10,0x00,0x00,0x00,0x00,0x00},
    ['<']  = {0x00,0x06,0x18,0x60,0xc0,0x60,0x18,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['=']  = {0x00,0x00,0x00,0xff,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['>']  = {0x00,0x60,0x18,0x06,0x03,0x06,0x18,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['?']  = {0x00,0x00,0x7c,0xc6,0x0c,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['@']  = {0x00,0x7c,0xc6,0xde,0xde,0xde,0xdc,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['A']  = {0x00,0x00,0x18,0x3c,0x66,0xc3,0xff,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['B']  = {0x00,0x00,0xfe,0xc3,0xc3,0xfe,0xc3,0xc3,0xc3,0xfe,0x00,0x00,0x00,0x00,0x00,0x00},
    ['C']  = {0x00,0x00,0x7e,0xc3,0xc0,0xc0,0xc0,0xc0,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['D']  = {0x00,0x00,0xfc,0xc6,0xc3,0xc3,0xc3,0xc3,0xc6,0xfc,0x00,0x00,0x00,0x00,0x00,0x00},
    ['E']  = {0x00,0x00,0xff,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xff,0x00,0x00,0x00,0x00,0x00,0x00},
    ['F']  = {0x00,0x00,0xff,0xc0,0xc0,0xfc,0xc0,0xc0,0xc0,0xc0,0x00,0x00,0x00,0x00,0x00,0x00},
    ['G']  = {0x00,0x00,0x7e,0xc3,0xc0,0xc0,0xcf,0xc3,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['H']  = {0x00,0x00,0xc3,0xc3,0xc3,0xff,0xc3,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['I']  = {0x00,0x00,0x7e,0x18,0x18,0x18,0x18,0x18,0x18,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['J']  = {0x00,0x00,0x0f,0x03,0x03,0x03,0x03,0xc3,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['K']  = {0x00,0x00,0xc6,0xcc,0xd8,0xf0,0xf8,0xdc,0xce,0xc6,0x00,0x00,0x00,0x00,0x00,0x00},
    ['L']  = {0x00,0x00,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xc0,0xff,0x00,0x00,0x00,0x00,0x00,0x00},
    ['M']  = {0x00,0x00,0xc3,0xe7,0xff,0xdb,0xc3,0xc3,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['N']  = {0x00,0x00,0xc3,0xe3,0xf3,0xdb,0xcf,0xc7,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['O']  = {0x00,0x00,0x7e,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['P']  = {0x00,0x00,0xfe,0xc3,0xc3,0xfe,0xc0,0xc0,0xc0,0xc0,0x00,0x00,0x00,0x00,0x00,0x00},
    ['Q']  = {0x00,0x00,0x7e,0xc3,0xc3,0xc3,0xc3,0xcb,0xc7,0x7e,0x03,0x00,0x00,0x00,0x00,0x00},
    ['R']  = {0x00,0x00,0xfe,0xc3,0xc3,0xfe,0xd8,0xcc,0xc6,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['S']  = {0x00,0x00,0x7e,0xc3,0xc0,0x7e,0x03,0x03,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['T']  = {0x00,0x00,0xff,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['U']  = {0x00,0x00,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0xc3,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['V']  = {0x00,0x00,0xc3,0xc3,0xc3,0xc3,0xc3,0x66,0x3c,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['W']  = {0x00,0x00,0xc3,0xc3,0xc3,0xdb,0xff,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    ['X']  = {0x00,0x00,0xc3,0x66,0x3c,0x18,0x3c,0x66,0xc3,0xc3,0x00,0x00,0x00,0x00,0x00,0x00},
    ['Y']  = {0x00,0x00,0xc3,0x66,0x3c,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['Z']  = {0x00,0x00,0xff,0x06,0x0c,0x18,0x30,0x60,0xc0,0xff,0x00,0x00,0x00,0x00,0x00,0x00},
    ['[']  = {0x00,0x3c,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['\\'] = {0x00,0x00,0x80,0xc0,0x60,0x30,0x18,0x0c,0x06,0x02,0x00,0x00,0x00,0x00,0x00,0x00},
    [']']  = {0x00,0x3c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x0c,0x3c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['^']  = {0x18,0x3c,0x66,0xc3,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['_']  = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x00,0x00},
    ['`']  = {0x00,0x18,0x18,0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['a']  = {0x00,0x00,0x00,0x7c,0x06,0x7e,0xc6,0xc6,0xce,0x76,0x00,0x00,0x00,0x00,0x00,0x00},
    ['b']  = {0x00,0xc0,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0xc6,0xfc,0x00,0x00,0x00,0x00,0x00,0x00},
    ['c']  = {0x00,0x00,0x00,0x7c,0xc6,0xc0,0xc0,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['d']  = {0x00,0x06,0x06,0x7e,0xc6,0xc6,0xc6,0xc6,0xc6,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['e']  = {0x00,0x00,0x00,0x7c,0xc6,0xfe,0xc0,0xc0,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['f']  = {0x00,0x1c,0x30,0xfc,0x30,0x30,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00,0x00},
    ['g']  = {0x00,0x00,0x00,0x7e,0xc6,0xc6,0xc6,0x7e,0x06,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['h']  = {0x00,0xc0,0xc0,0xfc,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00,0x00,0x00},
    ['i']  = {0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['j']  = {0x00,0x06,0x06,0x00,0x0e,0x06,0x06,0x06,0x66,0x3c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['k']  = {0x00,0xc0,0xc0,0xcc,0xd8,0xf0,0xf8,0xdc,0xce,0xc6,0x00,0x00,0x00,0x00,0x00,0x00},
    ['l']  = {0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['m']  = {0x00,0x00,0x00,0xfe,0xdb,0xdb,0xdb,0xdb,0xdb,0xdb,0x00,0x00,0x00,0x00,0x00,0x00},
    ['n']  = {0x00,0x00,0x00,0xfc,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x00,0x00,0x00,0x00,0x00,0x00},
    ['o']  = {0x00,0x00,0x00,0x7c,0xc6,0xc6,0xc6,0xc6,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['p']  = {0x00,0x00,0x00,0xfc,0xc6,0xc6,0xc6,0xfc,0xc0,0xc0,0x00,0x00,0x00,0x00,0x00,0x00},
    ['q']  = {0x00,0x00,0x00,0x7e,0xc6,0xc6,0xc6,0x7e,0x06,0x06,0x00,0x00,0x00,0x00,0x00,0x00},
    ['r']  = {0x00,0x00,0x00,0xde,0x76,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00,0x00},
    ['s']  = {0x00,0x00,0x00,0x7c,0xc6,0x70,0x1c,0x06,0xc6,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['t']  = {0x00,0x18,0x18,0x7e,0x18,0x18,0x18,0x18,0x18,0x0e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['u']  = {0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0xc6,0xc6,0x7e,0x00,0x00,0x00,0x00,0x00,0x00},
    ['v']  = {0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0x6c,0x38,0x10,0x00,0x00,0x00,0x00,0x00,0x00},
    ['w']  = {0x00,0x00,0x00,0xc6,0xc6,0xd6,0xfe,0xfe,0x6c,0x28,0x00,0x00,0x00,0x00,0x00,0x00},
    ['x']  = {0x00,0x00,0x00,0xc6,0x6c,0x38,0x38,0x6c,0xc6,0xc6,0x00,0x00,0x00,0x00,0x00,0x00},
    ['y']  = {0x00,0x00,0x00,0xc6,0xc6,0xc6,0xc6,0x7e,0x06,0x7c,0x00,0x00,0x00,0x00,0x00,0x00},
    ['z']  = {0x00,0x00,0x00,0xfe,0x0c,0x18,0x30,0x60,0xc0,0xfe,0x00,0x00,0x00,0x00,0x00,0x00},
    ['{']  = {0x00,0x0e,0x18,0x18,0x70,0x18,0x18,0x0e,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['|']  = {0x00,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    ['}']  = {0x00,0x70,0x18,0x18,0x0e,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['~']  = {0x76,0xdc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

static x11_win_entry_t *find_win(x11_client_state_t *st, Window w)
{
    if (!st) return NULL;
    for (int i = 0; i < st->num_windows; i++) {
        if (st->windows[i].wid == w) return &st->windows[i];
    }
    return NULL;
}

/* ── Display Connection ──────────────────────────────────────────────────── */

Display *XOpenDisplay(const char *display_name)
{
    (void)display_name;
    Display *d = (Display *)malloc(sizeof(Display));
    if (!d) return NULL;
    memset(d, 0, sizeof(Display));

    x11_client_state_t *st = (x11_client_state_t *)malloc(sizeof(x11_client_state_t));
    if (!st) {
        free(d);
        return NULL;
    }
    memset(st, 0, sizeof(x11_client_state_t));

    /* Create client communication channel */
    st->client_chan = (unsigned int)az_channel_create();
    st->server_chan = 1; /* azwm / xorg default server channel */
    st->screen_w = 1280;
    st->screen_h = 800;

    d->fd = (int)st->client_chan;
    d->nscreens = 1;
    d->screens = (Screen *)malloc(sizeof(Screen));
    if (d->screens) {
        memset(d->screens, 0, sizeof(Screen));
        d->screens[0].display = d;
        d->screens[0].root = 1;
        d->screens[0].width = 1280;
        d->screens[0].height = 800;
        d->screens[0].white_pixel = 0xFFFFFFFF;
        d->screens[0].black_pixel = 0xFF000000;
        d->screens[0].root_depth = 32;
    }
    d->default_root = 1;
    d->next_xid = 100;
    d->client_state = st;

    return d;
}

int XCloseDisplay(Display *display)
{
    if (!display) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    if (st) {
        for (int i = 0; i < st->num_windows; i++) {
            if (st->windows[i].pixels) {
                az_shmem_unmap((int)st->windows[i].shmem_id, st->windows[i].pixels);
            }
        }
        free(st);
    }
    if (display->screens) free(display->screens);
    free(display);
    return 0;
}

int XConnectionNumber(Display *display)
{
    return display ? display->fd : -1;
}

int XDefaultScreen(Display *display)
{
    (void)display;
    return 0;
}

Screen *XDefaultScreenOfDisplay(Display *display)
{
    return (display && display->screens) ? &display->screens[0] : NULL;
}

Screen *XScreenOfDisplay(Display *display, int screen_number)
{
    (void)screen_number;
    return XDefaultScreenOfDisplay(display);
}

Window XRootWindow(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 1;
}

Window XDefaultRootWindow(Display *display)
{
    return XRootWindow(display, 0);
}

Visual *XDefaultVisual(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return NULL;
}

GC XDefaultGC(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return NULL;
}

int XDefaultDepth(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 32;
}

int XDisplayWidth(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 1280;
}

int XDisplayHeight(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 800;
}

unsigned long XBlackPixel(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 0xFF000000;
}

unsigned long XWhitePixel(Display *display, int screen_number)
{
    (void)display; (void)screen_number;
    return 0xFFFFFFFF;
}

/* ── Window Management ───────────────────────────────────────────────────── */

Window XCreateWindow(Display *display, Window parent, int x, int y,
                     unsigned int width, unsigned int height, unsigned int border_width,
                     int depth, unsigned int c_class, Visual *visual,
                     unsigned long valuemask, XSetWindowAttributes *attributes)
{
    (void)parent; (void)border_width; (void)depth; (void)c_class; (void)visual;
    if (!display || !display->client_state) return None;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;

    if (st->num_windows >= MAX_WINDOWS_PER_CLIENT) return None;

    Window wid = display->next_xid++;

    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type = AZ_WM_CREATE_WINDOW;
    req.client_chan = st->client_chan;
    req.create.x = x;
    req.create.y = y;
    req.create.w = width;
    req.create.h = height;
    strncpy(req.create.title, "X11 Application", sizeof(req.create.title) - 1);

    if (az_channel_send(st->server_chan, (az_ipc_msg_t *)&req) < 0) {
        return None;
    }

    az_wm_msg_t resp;
    memset(&resp, 0, sizeof(resp));
    for (;;) {
        if (az_channel_recv(st->client_chan, (az_ipc_msg_t *)&resp) < 0) return None;
        if (resp.type == AZ_WM_WINDOW_CREATED) break;
    }

    x11_win_entry_t *win = &st->windows[st->num_windows++];
    win->wid = wid;
    win->assigned_wid = resp.created.assigned_wid;
    win->width = resp.created.width;
    win->height = resp.created.height;
    win->shmem_id = resp.created.shmem_id;
    void *map_addr = (void *)(0x40000000UL + ((unsigned long)resp.created.shmem_id * 0x1000000UL));
    if (az_shmem_map((int)resp.created.shmem_id, map_addr) >= 0) {
        win->pixels = (uint32_t *)map_addr;
    } else {
        win->pixels = NULL;
    }
    win->x = x;
    win->y = y;
    win->mapped = True;
    win->background = (valuemask & CWBackPixel && attributes) ? attributes->background_pixel : 0xFF1E1E2E;
    win->border = 0xFF45475A;
    strncpy(win->title, "X11 Application", sizeof(win->title) - 1);

    if (win->pixels) {
        for (unsigned int i = 0; i < win->width * win->height; i++) {
            win->pixels[i] = (uint32_t)win->background;
        }
    }

    return wid;
}

Window XCreateSimpleWindow(Display *display, Window parent, int x, int y,
                           unsigned int width, unsigned int height, unsigned int border_width,
                           unsigned long border, unsigned long background)
{
    XSetWindowAttributes attr;
    memset(&attr, 0, sizeof(attr));
    attr.background_pixel = background;
    attr.border_pixel = border;
    return XCreateWindow(display, parent, x, y, width, height, border_width,
                         32, InputOutput, NULL, CWBackPixel | CWBorderPixel, &attr);
}

int XDestroyWindow(Display *display, Window w)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (!win) return 0;

    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type = AZ_WM_DESTROY_WINDOW;
    req.wid = win->assigned_wid;
    req.client_chan = st->client_chan;
    az_channel_send(st->server_chan, (az_ipc_msg_t *)&req);

    if (win->pixels) {
        az_shmem_unmap((int)win->shmem_id, win->pixels);
        win->pixels = NULL;
    }
    return 0;
}

int XMapWindow(Display *display, Window w)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) {
        win->mapped = True;
        XFlush(display);
    }
    return 0;
}

int XMapRaised(Display *display, Window w)
{
    return XMapWindow(display, w);
}

int XUnmapWindow(Display *display, Window w)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) win->mapped = False;
    return 0;
}

int XMoveWindow(Display *display, Window w, int x, int y)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) {
        win->x = x; win->y = y;
        az_wm_msg_t req;
        memset(&req, 0, sizeof(req));
        req.type = AZ_WM_MOVE_WINDOW;
        req.wid = win->assigned_wid;
        req.client_chan = st->client_chan;
        req.move.x = x;
        req.move.y = y;
        az_channel_send(st->server_chan, (az_ipc_msg_t *)&req);
    }
    return 0;
}

int XResizeWindow(Display *display, Window w, unsigned int width, unsigned int height)
{
    (void)display; (void)w; (void)width; (void)height;
    return 0;
}

int XMoveResizeWindow(Display *display, Window w, int x, int y, unsigned int width, unsigned int height)
{
    XMoveWindow(display, w, x, y);
    return XResizeWindow(display, w, width, height);
}

int XStoreName(Display *display, Window w, const char *window_name)
{
    if (!display || !display->client_state || !window_name) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) {
        strncpy(win->title, window_name, sizeof(win->title) - 1);
    }
    return 0;
}

int XFetchName(Display *display, Window w, char **window_name_return)
{
    if (!display || !display->client_state || !window_name_return) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) {
        *window_name_return = strdup(win->title);
        return 1;
    }
    return 0;
}

int XSelectInput(Display *display, Window w, long event_mask)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win) win->event_mask = event_mask;
    return 0;
}

int XClearWindow(Display *display, Window w)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win && win->pixels) {
        for (unsigned int i = 0; i < win->width * win->height; i++) {
            win->pixels[i] = (uint32_t)win->background;
        }
    }
    return 0;
}

int XClearArea(Display *display, Window w, int x, int y, unsigned int width, unsigned int height, Bool exposures)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (win && win->pixels) {
        int x0 = x < 0 ? 0 : x;
        int y0 = y < 0 ? 0 : y;
        int x1 = (int)(x + width);
        int y1 = (int)(y + height);
        if (x1 > (int)win->width) x1 = (int)win->width;
        if (y1 > (int)win->height) y1 = (int)win->height;
        for (int cy = y0; cy < y1; cy++) {
            for (int cx = x0; cx < x1; cx++) {
                win->pixels[cy * win->width + cx] = (uint32_t)win->background;
            }
        }
    }
    (void)exposures;
    return 0;
}

int XGetWindowAttributes(Display *display, Window w, XWindowAttributes *attr)
{
    if (!display || !display->client_state || !attr) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, w);
    if (!win) return 0;
    memset(attr, 0, sizeof(XWindowAttributes));
    attr->x = win->x;
    attr->y = win->y;
    attr->width = win->width;
    attr->height = win->height;
    attr->depth = 32;
    attr->root = 1;
    attr->map_state = win->mapped ? NormalState : WithdrawnState;
    return 1;
}

int XGetGeometry(Display *display, Drawable d, Window *root_return, int *x_return, int *y_return,
                 unsigned int *width_return, unsigned int *height_return,
                 unsigned int *border_width_return, unsigned int *depth_return)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win) return 0;
    if (root_return) *root_return = 1;
    if (x_return) *x_return = win->x;
    if (y_return) *y_return = win->y;
    if (width_return) *width_return = win->width;
    if (height_return) *height_return = win->height;
    if (border_width_return) *border_width_return = 1;
    if (depth_return) *depth_return = 32;
    return 1;
}

/* ── Event Handling ──────────────────────────────────────────────────────── */

int XNextEvent(Display *display, XEvent *event_return)
{
    if (!display || !display->client_state || !event_return) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;

    for (;;) {
        az_wm_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        if (az_channel_recv(st->client_chan, (az_ipc_msg_t *)&msg) < 0) {
            return 0;
        }

        memset(event_return, 0, sizeof(XEvent));
        event_return->xany.display = display;

        Window target_wid = 0;
        for (int i = 0; i < st->num_windows; i++) {
            if (st->windows[i].assigned_wid == msg.wid || msg.wid == 0) {
                target_wid = st->windows[i].wid;
                break;
            }
        }
        if (!target_wid && st->num_windows > 0) target_wid = st->windows[0].wid;
        event_return->xany.window = target_wid;

        if (msg.type == AZ_WM_KEY_EVENT) {
            event_return->type = msg.key.pressed ? KeyPress : KeyRelease;
            event_return->xkey.keycode = msg.key.keycode ? msg.key.keycode : msg.key.scancode;
            event_return->xkey.state = msg.key.modifiers;
            return 1;
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            if (msg.mouse.buttons != 0) {
                event_return->type = ButtonPress;
                event_return->xbutton.button = (msg.mouse.buttons & 1) ? Button1 :
                                               (msg.mouse.buttons & 2) ? Button3 : Button2;
            } else {
                event_return->type = MotionNotify;
            }
            event_return->xbutton.x = msg.mouse.abs_x;
            event_return->xbutton.y = msg.mouse.abs_y;
            event_return->xbutton.x_root = msg.mouse.abs_x;
            event_return->xbutton.y_root = msg.mouse.abs_y;
            return 1;
        } else if (msg.type == AZ_WM_INVALIDATE || msg.type == AZ_WM_WINDOW_CREATED) {
            event_return->type = Expose;
            x11_win_entry_t *win = find_win(st, target_wid);
            if (win) {
                event_return->xexpose.width = win->width;
                event_return->xexpose.height = win->height;
            }
            return 1;
        } else if (msg.type == AZ_WM_DESTROY_WINDOW) {
            event_return->type = DestroyNotify;
            return 1;
        }
    }
}

int XPending(Display *display)
{
    (void)display;
    return 0;
}

int XEventsQueued(Display *display, int mode)
{
    (void)display; (void)mode;
    return 0;
}

Bool XCheckTypedEvent(Display *display, int event_type, XEvent *event_return)
{
    (void)display; (void)event_type; (void)event_return;
    return False;
}

Bool XCheckMaskEvent(Display *display, long event_mask, XEvent *event_return)
{
    (void)display; (void)event_mask; (void)event_return;
    return False;
}

Bool XCheckWindowEvent(Display *display, Window w, long event_mask, XEvent *event_return)
{
    (void)display; (void)w; (void)event_mask; (void)event_return;
    return False;
}

int XSendEvent(Display *display, Window w, Bool propagate, long event_mask, XEvent *event_send)
{
    (void)display; (void)w; (void)propagate; (void)event_mask; (void)event_send;
    return 1;
}

int XFlush(Display *display)
{
    if (!display || !display->client_state) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;

    for (int i = 0; i < st->num_windows; i++) {
        if (st->windows[i].mapped && st->windows[i].pixels) {
            az_wm_msg_t req;
            memset(&req, 0, sizeof(req));
            req.type = AZ_WM_INVALIDATE;
            req.wid = st->windows[i].assigned_wid;
            req.client_chan = st->client_chan;
            az_channel_send(st->server_chan, (az_ipc_msg_t *)&req);
        }
    }
    return 1;
}

int XSync(Display *display, Bool discard)
{
    (void)discard;
    return XFlush(display);
}

/* ── Graphics Contexts & 2D Rasterizer ───────────────────────────────────── */

GC XCreateGC(Display *display, Drawable d, unsigned long valuemask, XGCValues *values)
{
    GC gc = (GC)malloc(sizeof(struct _XGC));
    if (!gc) return NULL;
    memset(gc, 0, sizeof(struct _XGC));
    gc->display = display;
    gc->drawable = d;
    gc->values.foreground = 0xFFCDD6F4;
    gc->values.background = 0xFF1E1E2E;
    gc->values.line_width = 1;
    gc->mask = valuemask;

    if (values) {
        if (valuemask & GCForeground) gc->values.foreground = values->foreground;
        if (valuemask & GCBackground) gc->values.background = values->background;
        if (valuemask & GCLineWidth)  gc->values.line_width = values->line_width;
    }
    return gc;
}

int XFreeGC(Display *display, GC gc)
{
    (void)display;
    if (gc) free(gc);
    return 0;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground)
{
    (void)display;
    if (gc) gc->values.foreground = foreground;
    return 0;
}

int XSetBackground(Display *display, GC gc, unsigned long background)
{
    (void)display;
    if (gc) gc->values.background = background;
    return 0;
}

int XSetFunction(Display *display, GC gc, int function)
{
    (void)display;
    if (gc) gc->values.function = function;
    return 0;
}

int XSetLineAttributes(Display *display, GC gc, unsigned int line_width, int line_style, int cap_style, int join_style)
{
    (void)display; (void)line_style; (void)cap_style; (void)join_style;
    if (gc) gc->values.line_width = (int)line_width;
    return 0;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y)
{
    if (!display || !display->client_state || !gc) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return 0;

    if (x >= 0 && x < (int)win->width && y >= 0 && y < (int)win->height) {
        win->pixels[y * win->width + x] = (uint32_t)gc->values.foreground;
    }
    return 0;
}

int XDrawPoints(Display *display, Drawable d, GC gc, XPoint *points, int npoints, int mode)
{
    (void)mode;
    for (int i = 0; i < npoints; i++) {
        XDrawPoint(display, d, gc, points[i].x, points[i].y);
    }
    return 0;
}

int XDrawLine(Display *display, Drawable d, GC gc, int x1, int y1, int x2, int y2)
{
    if (!display || !display->client_state || !gc) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return 0;

    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;

    int cx = x1, cy = y1;
    uint32_t color = (uint32_t)gc->values.foreground;

    for (;;) {
        if (cx >= 0 && cx < (int)win->width && cy >= 0 && cy < (int)win->height) {
            win->pixels[cy * win->width + cx] = color;
        }
        if (cx == x2 && cy == y2) break;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            cx += sx;
        }
        if (e2 < dx) {
            err += dx;
            cy += sy;
        }
    }
    return 0;
}

int XDrawLines(Display *display, Drawable d, GC gc, XPoint *points, int npoints, int mode)
{
    (void)mode;
    for (int i = 0; i < npoints - 1; i++) {
        XDrawLine(display, d, gc, points[i].x, points[i].y, points[i+1].x, points[i+1].y);
    }
    return 0;
}

int XDrawRectangle(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height)
{
    int x2 = x + (int)width;
    int y2 = y + (int)height;
    XDrawLine(display, d, gc, x, y, x2, y);
    XDrawLine(display, d, gc, x2, y, x2, y2);
    XDrawLine(display, d, gc, x2, y2, x, y2);
    XDrawLine(display, d, gc, x, y2, x, y);
    return 0;
}

int XFillRectangle(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height)
{
    if (!display || !display->client_state || !gc) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return 0;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (int)(x + width);
    int y1 = (int)(y + height);
    if (x1 > (int)win->width) x1 = (int)win->width;
    if (y1 > (int)win->height) y1 = (int)win->height;

    uint32_t color = (uint32_t)gc->values.foreground;
    for (int cy = y0; cy < y1; cy++) {
        for (int cx = x0; cx < x1; cx++) {
            win->pixels[cy * win->width + cx] = color;
        }
    }
    return 0;
}

int XFillRectangles(Display *display, Drawable d, GC gc, XRectangle *rectangles, int nrectangles)
{
    for (int i = 0; i < nrectangles; i++) {
        XFillRectangle(display, d, gc, rectangles[i].x, rectangles[i].y, rectangles[i].width, rectangles[i].height);
    }
    return 0;
}

int XDrawArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2)
{
    (void)angle1; (void)angle2;
    int rx = (int)width / 2;
    int ry = (int)height / 2;
    int xc = x + rx;
    int yc = y + ry;

    int a = rx, b = ry;
    long a2 = (long)a * a;
    long b2 = (long)b * b;
    long twoa2 = 2 * a2;
    long twob2 = 2 * b2;

    int cx = 0;
    int cy = b;
    long px = 0;
    long py = twoa2 * cy;

    long p = (long)(b2 - (a2 * b) + (0.25 * a2));
    while (px < py) {
        XDrawPoint(display, d, gc, xc + cx, yc + cy);
        XDrawPoint(display, d, gc, xc - cx, yc + cy);
        XDrawPoint(display, d, gc, xc + cx, yc - cy);
        XDrawPoint(display, d, gc, xc - cx, yc - cy);
        cx++;
        px += twob2;
        if (p < 0) {
            p += b2 + px;
        } else {
            cy--;
            py -= twoa2;
            p += b2 + px - py;
        }
    }
    return 0;
}

int XFillArc(Display *display, Drawable d, GC gc, int x, int y, unsigned int width, unsigned int height, int angle1, int angle2)
{
    (void)angle1; (void)angle2;
    int rx = (int)width / 2;
    int ry = (int)height / 2;
    int xc = x + rx;
    int yc = y + ry;

    for (int dy = -ry; dy <= ry; dy++) {
        int dx = (int)(rx * __builtin_sqrt(1.0 - (double)(dy * dy) / (double)(ry * ry)));
        XDrawLine(display, d, gc, xc - dx, yc + dy, xc + dx, yc + dy);
    }
    return 0;
}

int XDrawString(Display *display, Drawable d, GC gc, int x, int y, const char *string, int length)
{
    if (!display || !display->client_state || !gc || !string) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return 0;

    int cur_x = x;
    uint32_t color = (uint32_t)gc->values.foreground;

    for (int i = 0; i < length && string[i]; i++) {
        unsigned char ch = (unsigned char)string[i];
        if (ch > 127) ch = '?';
        for (int row = 0; row < 16; row++) {
            unsigned char row_bits = g_x11_font_8x16[ch][row];
            int py = y - 12 + row; /* standard baseline offset */
            if (py < 0 || py >= (int)win->height) continue;
            for (int col = 0; col < 8; col++) {
                if (row_bits & (0x80 >> col)) {
                    int px = cur_x + col;
                    if (px >= 0 && px < (int)win->width) {
                        win->pixels[py * win->width + px] = color;
                    }
                }
            }
        }
        cur_x += 8;
    }
    return 0;
}

int XDrawImageString(Display *display, Drawable d, GC gc, int x, int y, const char *string, int length)
{
    return XDrawString(display, d, gc, x, y, string, length);
}

/* ── Image Primitives ────────────────────────────────────────────────────── */

XImage *XCreateImage(Display *display, Visual *visual, unsigned int depth, int format,
                     int offset, char *data, unsigned int width, unsigned int height,
                     int bitmap_pad, int bytes_per_line)
{
    (void)display; (void)visual; (void)offset; (void)bitmap_pad;
    XImage *img = (XImage *)malloc(sizeof(XImage));
    if (!img) return NULL;
    memset(img, 0, sizeof(XImage));

    img->width = (int)width;
    img->height = (int)height;
    img->depth = (int)depth;
    img->format = format;
    img->bits_per_pixel = 32;
    img->bytes_per_line = bytes_per_line ? bytes_per_line : (int)(width * 4);
    img->red_mask   = 0x00FF0000;
    img->green_mask = 0x0000FF00;
    img->blue_mask  = 0x000000FF;

    if (data) {
        img->data = data;
    } else {
        img->data = (char *)malloc((size_t)(img->bytes_per_line * img->height));
    }
    return img;
}

int XDestroyImage(XImage *image)
{
    if (!image) return 0;
    if (image->data) free(image->data);
    free(image);
    return 0;
}

int XPutImage(Display *display, Drawable d, GC gc, XImage *image,
              int src_x, int src_y, int dest_x, int dest_y,
              unsigned int width, unsigned int height)
{
    (void)gc;
    if (!display || !display->client_state || !image || !image->data) return 0;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return 0;

    uint32_t *src_pixels = (uint32_t *)image->data;
    int src_stride = image->bytes_per_line / 4;

    for (unsigned int r = 0; r < height; r++) {
        int sy = src_y + (int)r;
        int dy = dest_y + (int)r;
        if (sy < 0 || sy >= image->height || dy < 0 || dy >= (int)win->height) continue;

        for (unsigned int c = 0; c < width; c++) {
            int sx = src_x + (int)c;
            int dx = dest_x + (int)c;
            if (sx < 0 || sx >= image->width || dx < 0 || dx >= (int)win->width) continue;
            win->pixels[dy * win->width + dx] = src_pixels[sy * src_stride + sx];
        }
    }
    return 0;
}

XImage *XGetImage(Display *display, Drawable d, int x, int y,
                  unsigned int width, unsigned int height,
                  unsigned long plane_mask, int format)
{
    (void)plane_mask;
    if (!display || !display->client_state) return NULL;
    x11_client_state_t *st = (x11_client_state_t *)display->client_state;
    x11_win_entry_t *win = find_win(st, (Window)d);
    if (!win || !win->pixels) return NULL;

    XImage *img = XCreateImage(display, NULL, 32, format, 0, NULL, width, height, 32, (int)(width * 4));
    if (!img) return NULL;

    uint32_t *dst = (uint32_t *)img->data;
    for (unsigned int r = 0; r < height; r++) {
        int sy = y + (int)r;
        for (unsigned int c = 0; c < width; c++) {
            int sx = x + (int)c;
            if (sx >= 0 && sx < (int)win->width && sy >= 0 && sy < (int)win->height) {
                dst[r * width + c] = win->pixels[sy * win->width + sx];
            } else {
                dst[r * width + c] = 0;
            }
        }
    }
    return img;
}

/* ── Atoms & Properties ──────────────────────────────────────────────────── */

Atom XInternAtom(Display *display, const char *atom_name, Bool only_if_exists)
{
    (void)display; (void)only_if_exists;
    if (!atom_name) return None;
    if (strcmp(atom_name, "WM_NAME") == 0) return XA_WM_NAME;
    if (strcmp(atom_name, "WM_CLASS") == 0) return XA_WM_CLASS;
    if (strcmp(atom_name, "STRING") == 0) return XA_STRING;
    if (strcmp(atom_name, "WINDOW") == 0) return XA_WINDOW;
    return (Atom)100;
}

char *XGetAtomName(Display *display, Atom atom)
{
    (void)display;
    if (atom == XA_WM_NAME) return strdup("WM_NAME");
    if (atom == XA_WM_CLASS) return strdup("WM_CLASS");
    if (atom == XA_STRING) return strdup("STRING");
    return strdup("UNKNOWN_ATOM");
}

/* ── Utilities ───────────────────────────────────────────────────────────── */

XSizeHints *XAllocSizeHints(void)
{
    XSizeHints *h = (XSizeHints *)malloc(sizeof(XSizeHints));
    if (h) memset(h, 0, sizeof(XSizeHints));
    return h;
}

XWMHints *XAllocWMHints(void)
{
    XWMHints *h = (XWMHints *)malloc(sizeof(XWMHints));
    if (h) memset(h, 0, sizeof(XWMHints));
    return h;
}

XClassHint *XAllocClassHint(void)
{
    XClassHint *h = (XClassHint *)malloc(sizeof(XClassHint));
    if (h) memset(h, 0, sizeof(XClassHint));
    return h;
}

void XSetWMProperties(Display *display, Window w,
                      XTextProperty *window_name, XTextProperty *icon_name,
                      char **argv, int argc,
                      XSizeHints *normal_hints, XWMHints *wm_hints,
                      XClassHint *class_hints)
{
    (void)icon_name; (void)argv; (void)argc; (void)normal_hints; (void)wm_hints; (void)class_hints;
    if (window_name && window_name->value) {
        XStoreName(display, w, (const char *)window_name->value);
    }
}

void XSetWMNormalHints(Display *display, Window w, XSizeHints *hints)
{
    (void)display; (void)w; (void)hints;
}

void XSetWMHints(Display *display, Window w, XWMHints *hints)
{
    (void)display; (void)w; (void)hints;
}

void XSetClassHint(Display *display, Window w, XClassHint *class_hints)
{
    (void)display; (void)w; (void)class_hints;
}

int XLookupString(XKeyEvent *event_struct, char *buffer_return,
                  int bytes_buffer, KeySym *keysym_return,
                  void *status_in_out)
{
    (void)status_in_out;
    if (!event_struct || !buffer_return || bytes_buffer <= 0) return 0;

    unsigned int kc = event_struct->keycode;
    KeySym sym = XK_VoidSymbol;
    char ch = '\0';

    if (kc >= 0x20 && kc <= 0x7E) {
        ch = (char)kc;
        sym = (KeySym)kc;
    } else if (kc == '\n' || kc == '\r') {
        ch = '\n';
        sym = XK_Return;
    } else if (kc == '\b' || kc == 0x7F) {
        ch = '\b';
        sym = XK_BackSpace;
    } else if (kc == '\t') {
        ch = '\t';
        sym = XK_Tab;
    } else if (kc == 0x1B) {
        ch = 0x1B;
        sym = XK_Escape;
    }

    if (keysym_return) *keysym_return = sym;
    if (ch != '\0') {
        buffer_return[0] = ch;
        return 1;
    }
    return 0;
}
