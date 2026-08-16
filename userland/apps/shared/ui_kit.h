/* ============================================================================
 * AzamiOS Desktop Environment — Shared UI Toolkit
 * File: userland/apps/shared/ui_kit.h
 *
 * Header-only toolkit used by all DE apps.
 * Include this after including the azwm protocol headers.
 *
 * Usage:
 *   #include "../azwm/protocol.h"
 *   #include "../azwm/de_protocol.h"
 *   #include "../azwm/de_font.h"
 *   #include "../shared/ui_kit.h"
 *
 * Every function is static inline to avoid ODR issues across TUs.
 * ============================================================================ */
#pragma once

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "de_log.h"

/* ============================================================================
 * Catppuccin Mocha palette (0xFFRRGGBB / ARGB)
 * ============================================================================ */
#define UK_CRUST      0xFF11111B
#define UK_MANTLE     0xFF181825
#define UK_BASE       0xFF1E1E2E
#define UK_SURFACE0   0xFF313244
#define UK_SURFACE1   0xFF45475A
#define UK_SURFACE2   0xFF585B70
#define UK_OVERLAY0   0xFF6C7086
#define UK_OVERLAY1   0xFF7F849C
#define UK_OVERLAY2   0xFF9399B2
#define UK_SUBTEXT0   0xFFA6ADC8
#define UK_SUBTEXT1   0xFFBAC2DE
#define UK_TEXT       0xFFCDD6F4
#define UK_LAVENDER   0xFFB4BEFE
#define UK_BLUE       0xFF89B4FA
#define UK_SAPPHIRE   0xFF74C7EC
#define UK_SKY        0xFF89DCEB
#define UK_TEAL       0xFF94E2D5
#define UK_GREEN      0xFFA6E3A1
#define UK_YELLOW     0xFFF9E2AF
#define UK_PEACH      0xFFFAB387
#define UK_MAROON     0xFFEBA0AC
#define UK_RED        0xFFF38BA8
#define UK_MAUVE      0xFFCBA6F7
#define UK_PINK       0xFFF5C2E7
#define UK_FLAMINGO   0xFFF2CDCD
#define UK_ROSEWATER  0xFFF5E0DC

/* ============================================================================
 * Window connection state
 * ============================================================================ */
typedef struct {
    unsigned int *pixels;       /* Mapped pixel buffer                    */
    unsigned int  width;        /* Client area width                      */
    unsigned int  height;       /* Client area height                     */
    unsigned int  wid;          /* Window ID assigned by compositor       */
    int           client_chan;  /* Our reply channel                      */
    int           server_chan;  /* azwm server channel (always 1)         */
    int           x;            /* Window X position                      */
    int           y;            /* Window Y position                      */
} uk_window_t;

/* ============================================================================
 * Drawing primitives
 * ============================================================================ */

static inline void uk_put_pixel(uk_window_t *w, int x, int y, unsigned int col)
{
    if (x < 0 || y < 0 || (unsigned int)x >= w->width || (unsigned int)y >= w->height)
        return;
    w->pixels[(unsigned int)y * w->width + (unsigned int)x] = col;
}

static inline unsigned int uk_blend(unsigned int dst, unsigned int src, unsigned int a)
{
    if (a == 0) return dst;
    if (a >= 255) return src;
    unsigned int inv_a = 255 - a;
    unsigned int rb = (((src & 0x00FF00FF) * a + (dst & 0x00FF00FF) * inv_a) >> 8) & 0x00FF00FF;
    unsigned int g  = (((src & 0x0000FF00) * a + (dst & 0x0000FF00) * inv_a) >> 8) & 0x0000FF00;
    return 0xFF000000 | rb | g;
}

static inline void uk_fill_rect(uk_window_t *w,
                                int rx, int ry, int rw, int rh,
                                unsigned int col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if ((unsigned int)x1 > w->width)  x1 = (int)w->width;
    if ((unsigned int)y1 > w->height) y1 = (int)w->height;
    if (x0 >= x1 || y0 >= y1) return;

    int fill_w = x1 - x0;
    unsigned long long col64 = ((unsigned long long)col << 32) | (unsigned long long)col;

    for (int y = y0; y < y1; y++) {
        unsigned int *dst = &w->pixels[(unsigned int)y * w->width + (unsigned int)x0];
        int count = fill_w;
        if (((unsigned long)dst & 7) && count > 0) {
            *dst++ = col;
            count--;
        }
        unsigned long long *dst64 = (unsigned long long *)dst;
        while (count >= 4) {
            dst64[0] = col64;
            dst64[1] = col64;
            dst64 += 2;
            count -= 4;
        }
        if (count >= 2) {
            *dst64++ = col64;
            count -= 2;
        }
        dst = (unsigned int *)dst64;
        if (count > 0) {
            *dst = col;
        }
    }
}

/* Vertical gradient fill (top→bottom) */
static inline void uk_gradient_v(uk_window_t *w,
                                 int rx, int ry, int rw, int rh,
                                 unsigned int top_col, unsigned int bot_col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if ((unsigned int)x1 > w->width)  x1 = (int)w->width;
    if ((unsigned int)y1 > w->height) y1 = (int)w->height;
    if (x0 >= x1 || y0 >= y1) return;

    int fill_w = x1 - x0;
    for (int y = y0; y < y1; y++) {
        unsigned int t = (rh > 1) ? (unsigned int)((y - ry) * 255 / (rh - 1)) : 0;
        unsigned int col = uk_blend(top_col, bot_col, t);
        unsigned int *dst = &w->pixels[(unsigned int)y * w->width + (unsigned int)x0];
        unsigned long long col64 = ((unsigned long long)col << 32) | (unsigned long long)col;
        int count = fill_w;
        if (((unsigned long)dst & 7) && count > 0) {
            *dst++ = col;
            count--;
        }
        unsigned long long *dst64 = (unsigned long long *)dst;
        while (count >= 4) {
            dst64[0] = col64;
            dst64[1] = col64;
            dst64 += 2;
            count -= 4;
        }
        if (count >= 2) {
            *dst64++ = col64;
            count -= 2;
        }
        dst = (unsigned int *)dst64;
        if (count > 0) {
            *dst = col;
        }
    }
}

/* Horizontal gradient fill (left→right) */
static inline void uk_gradient_h(uk_window_t *w,
                                 int rx, int ry, int rw, int rh,
                                 unsigned int left_col, unsigned int right_col)
{
    if (rw <= 0 || rh <= 0 || !w || !w->pixels) return;
    int x0 = rx < 0 ? 0 : rx;
    int y0 = ry < 0 ? 0 : ry;
    int x1 = rx + rw;
    int y1 = ry + rh;
    if ((unsigned int)x1 > w->width)  x1 = (int)w->width;
    if ((unsigned int)y1 > w->height) y1 = (int)w->height;
    if (x0 >= x1 || y0 >= y1) return;

    for (int x = x0; x < x1; x++) {
        unsigned int t = (rw > 1) ? (unsigned int)((x - rx) * 255 / (rw - 1)) : 0;
        unsigned int col = uk_blend(left_col, right_col, t);
        for (int y = y0; y < y1; y++) {
            w->pixels[(unsigned int)y * w->width + (unsigned int)x] = col;
        }
    }
}

/* Draw a circle (filled) */
static inline void uk_fill_circle(uk_window_t *w, int cx, int cy, int r, unsigned int col)
{
    int x, y;
    for (y = cy - r; y <= cy + r; y++) {
        for (x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r)
                uk_put_pixel(w, x, y, col);
        }
    }
}

static inline void uk_fill_rounded_rect(uk_window_t *w,
                                        int rx, int ry, int rw, int rh,
                                        int radius, unsigned int col)
{
    if (rw <= 0 || rh <= 0) return;
    if (radius * 2 > rw) radius = rw / 2;
    if (radius * 2 > rh) radius = rh / 2;
    if (radius <= 0) {
        uk_fill_rect(w, rx, ry, rw, rh, col);
        return;
    }
    uk_fill_rect(w, rx + radius, ry, rw - 2 * radius, rh, col);
    uk_fill_rect(w, rx, ry + radius, radius, rh - 2 * radius, col);
    uk_fill_rect(w, rx + rw - radius, ry + radius, radius, rh - 2 * radius, col);
    uk_fill_circle(w, rx + radius,          ry + radius,          radius, col);
    uk_fill_circle(w, rx + rw - radius - 1, ry + radius,          radius, col);
    uk_fill_circle(w, rx + radius,          ry + rh - radius - 1, radius, col);
    uk_fill_circle(w, rx + rw - radius - 1, ry + rh - radius - 1, radius, col);
}

/* ============================================================================
 * Text rendering (uses de_font.h)
 * ============================================================================ */

static inline void uk_draw_char(uk_window_t *w, int x, int y, char c, unsigned int col)
{
    de_font_draw_char(w->pixels, w->width, w->width, w->height, x, y, c, col);
}

static inline void uk_draw_text(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    int i;
    for (i = 0; s[i]; i++)
        uk_draw_char(w, x + i * 8, y, s[i], col);
}

static inline void uk_draw_text_2x(uk_window_t *w, int x, int y, const char *s, unsigned int col)
{
    de_font_draw_str_2x(w->pixels, w->width, w->width, w->height, x, y, s, col);
}

/* Draw text clipped to max_px width */
static inline void uk_draw_text_clip(uk_window_t *w, int x, int y,
                                      const char *s, unsigned int col, int max_px)
{
    int i;
    for (i = 0; s[i] && i * 8 + 8 <= max_px; i++)
        uk_draw_char(w, x + i * 8, y, s[i], col);
}

/* String length (no strlen in minimal libc) */
static inline int uk_strlen(const char *s)
{
    int i = 0;
    while (s[i]) i++;
    return i;
}

/* Centre text horizontally in a region */
static inline void uk_draw_text_centred(uk_window_t *w, int cx, int y,
                                         const char *s, unsigned int col)
{
    int len = uk_strlen(s);
    uk_draw_text(w, cx - (len * 8) / 2, y, s, col);
}

/* ============================================================================
 * Button widget
 * ============================================================================ */

typedef enum {
    UK_BTN_NORMAL = 0,
    UK_BTN_HOVER,
    UK_BTN_PRESSED,
    UK_BTN_DISABLED
} uk_btn_state_t;

static inline void uk_draw_button(uk_window_t *w,
                                  int bx, int by, int bw, int bh,
                                  const char *label, uk_btn_state_t state)
{
    if (bw <= 0 || bh <= 0) return;
    unsigned int top_bg, bot_bg, fg, border, highlight;
    switch (state) {
    case UK_BTN_HOVER:
        top_bg = UK_SURFACE2; bot_bg = UK_SURFACE1; fg = UK_TEXT;    border = UK_BLUE;     highlight = 0x50FFFFFF; break;
    case UK_BTN_PRESSED:
        top_bg = UK_MAUVE;    bot_bg = 0xFFB48EAD;  fg = UK_BASE;    border = UK_MAUVE;    highlight = 0x20000000; break;
    case UK_BTN_DISABLED:
        top_bg = UK_SURFACE0; bot_bg = UK_SURFACE0; fg = UK_OVERLAY0; border = UK_SURFACE1; highlight = 0x00000000; break;
    default: /* NORMAL */
        top_bg = UK_SURFACE1; bot_bg = UK_SURFACE0; fg = UK_TEXT;    border = UK_SURFACE2; highlight = 0x30FFFFFF; break;
    }

    uk_fill_rounded_rect(w, bx, by, bw, bh, 5, bot_bg);
    uk_gradient_v(w, bx + 1, by + 1, bw - 2, bh - 2, top_bg, bot_bg);

    /* Specular highlight line */
    if (highlight && bh > 4) {
        for (int x = bx + 4; x < bx + bw - 4; x++) {
            if (x >= 0 && (unsigned int)x < w->width && (by + 1) >= 0 && (unsigned int)(by + 1) < w->height) {
                unsigned int *p = &w->pixels[(by + 1) * w->width + x];
                *p = uk_blend(*p, 0xFFFFFFFF, (highlight >> 24) & 0xFF);
            }
        }
    }

    /* 1px Border */
    for (int x = bx + 3; x < bx + bw - 3; x++) {
        uk_put_pixel(w, x, by,          border);
        uk_put_pixel(w, x, by + bh - 1, border);
    }
    for (int y = by + 3; y < by + bh - 3; y++) {
        uk_put_pixel(w, bx,          y, border);
        uk_put_pixel(w, bx + bw - 1, y, border);
    }

    /* Centred label with subtle text drop shadow */
    int len = uk_strlen(label);
    int tx = bx + bw / 2 - (len * 8) / 2;
    int ty = by + (bh - 16) / 2;
    if (state != UK_BTN_PRESSED && state != UK_BTN_DISABLED) {
        uk_draw_text(w, tx + 1, ty + 1, label, 0xFF11111B);
    }
    uk_draw_text(w, tx, ty, label, fg);
}

/* ============================================================================
 * Panel / section header
 * ============================================================================ */

static inline void uk_draw_panel(uk_window_t *w,
                                  int px, int py, int pw, int ph,
                                  unsigned int bg)
{
    uk_fill_rounded_rect(w, px, py, pw, ph, 6, bg);
}

static inline void uk_draw_section_header(uk_window_t *w,
                                            int x, int y, int width,
                                            const char *title,
                                            unsigned int accent)
{
    /* Accent bar */
    uk_fill_rect(w, x, y, 3, 16, accent);
    /* Title text */
    uk_draw_text(w, x + 8, y, title, UK_TEXT);
    /* Divider line */
    uk_fill_rect(w, x, y + 18, width, 1, UK_SURFACE1);
}

/* ============================================================================
 * Scrollbar (simple vertical)
 * ============================================================================ */

static inline void uk_draw_scrollbar(uk_window_t *w,
                                      int x, int y, int h,
                                      int thumb_pos, int thumb_h)
{
    uk_fill_rect(w, x, y, 8, h, UK_SURFACE0);
    uk_fill_rect(w, x + 1, y + thumb_pos, 6, thumb_h, UK_SURFACE2);
}

/* ============================================================================
 * Tab bar (horizontal)
 * ============================================================================ */

#define UK_MAX_TABS  8

static inline void uk_draw_tab_bar(uk_window_t *w,
                                    int x, int y, int tab_w, int tab_h,
                                    const char **labels, int count, int active)
{
    int i;
    for (i = 0; i < count; i++) {
        int tx = x + i * (tab_w + 2);
        unsigned int bg     = (i == active) ? UK_SURFACE1 : UK_SURFACE0;
        unsigned int fg     = (i == active) ? UK_TEXT     : UK_OVERLAY1;
        unsigned int accent = (i == active) ? UK_MAUVE    : UK_BASE;

        uk_fill_rect(w, tx, y, tab_w, tab_h, bg);
        /* Active tab: bottom accent bar */
        uk_fill_rect(w, tx, y + tab_h - 2, tab_w, 2, accent);

        int llen = uk_strlen(labels[i]);
        uk_draw_text(w, tx + tab_w / 2 - (llen * 8) / 2,
                     y + (tab_h - 16) / 2, labels[i], fg);
    }
}

/* ============================================================================
 * Pixel-art icons (32×32 each)
 * All drawn relative to a top-left (ix, iy) anchor.
 * ============================================================================ */

/* Generic app icon base: rounded square background */
static inline void uk_icon_base(uk_window_t *w, int ix, int iy, unsigned int bg)
{
    uk_fill_rounded_rect(w, ix, iy, 32, 32, 5, bg);
}

/* ── Text Editor icon: horizontal lines ─────────────────────────────────────── */
static inline void uk_icon_texteditor(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_BLUE);
    int row;
    /* Document lines */
    for (row = 0; row < 5; row++) {
        int lw = (row == 4) ? 10 : 18;
        uk_fill_rect(w, ix + 7, iy + 7 + row * 4, lw, 2, UK_BASE);
    }
    /* Cursor */
    uk_fill_rect(w, ix + 7, iy + 7, 1, 18, UK_YELLOW);
}

/* ── Calculator icon: grid of buttons ───────────────────────────────────────── */
static inline void uk_icon_calculator(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_GREEN);
    /* Display bar */
    uk_fill_rect(w, ix + 5, iy + 5, 22, 6, UK_BASE);
    /* 4 buttons in 2×2 grid */
    uk_fill_rect(w, ix + 5,  iy + 14, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 18, iy + 14, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 5,  iy + 22, 9, 5, UK_BASE);
    uk_fill_rect(w, ix + 18, iy + 22, 9, 5, UK_BASE);
}

/* ── Clock icon: circle with hands ─────────────────────────────────────────── */
static inline void uk_icon_clock(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_PEACH);
    int cx = ix + 16, cy = iy + 16;
    uk_fill_circle(w, cx, cy, 10, UK_BASE);
    uk_fill_circle(w, cx, cy,  8, UK_MANTLE);
    /* Hour hand (pointing up-right) */
    int hx, hy;
    for (hx = 0; hx <= 4; hx++) { hy = -hx; uk_put_pixel(w, cx+hx, cy+hy, UK_TEXT); }
    /* Minute hand (pointing down) */
    int my2;
    for (my2 = 0; my2 <= 6; my2++) uk_put_pixel(w, cx, cy + my2, UK_TEXT);
    /* Centre dot */
    uk_fill_circle(w, cx, cy, 1, UK_MAUVE);
}

/* ── System Monitor icon: bar chart ─────────────────────────────────────────── */
static inline void uk_icon_sysmon(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_TEAL);
    /* Bar chart bars */
    uk_fill_rect(w, ix + 5,  iy + 18, 5, 8,  UK_BASE);
    uk_fill_rect(w, ix + 12, iy + 12, 5, 14, UK_BASE);
    uk_fill_rect(w, ix + 19, iy + 8,  5, 18, UK_BASE);
    /* X axis */
    uk_fill_rect(w, ix + 4, iy + 26, 22, 1, UK_BASE);
}

/* ── File Manager icon: folder ──────────────────────────────────────────────── */
static inline void uk_icon_filemanager(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_YELLOW);
    /* Folder tab */
    uk_fill_rounded_rect(w, ix + 4, iy + 10, 24, 16, 2, UK_BASE);
    uk_fill_rect(w, ix + 4, iy + 8, 10, 4, UK_BASE);
    /* Files inside (3 lines) */
    uk_fill_rect(w, ix + 7, iy + 13, 14, 2, UK_CRUST);
    uk_fill_rect(w, ix + 7, iy + 17, 14, 2, UK_CRUST);
    uk_fill_rect(w, ix + 7, iy + 21, 10, 2, UK_CRUST);
}

/* ── Terminal icon: >_ prompt ───────────────────────────────────────────────── */
static inline void uk_icon_terminal(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_SURFACE0);
    /* > arrow */
    uk_put_pixel(w, ix + 7,  iy + 12, UK_GREEN);
    uk_put_pixel(w, ix + 9,  iy + 14, UK_GREEN);
    uk_put_pixel(w, ix + 11, iy + 16, UK_GREEN);
    uk_put_pixel(w, ix + 9,  iy + 18, UK_GREEN);
    uk_put_pixel(w, ix + 7,  iy + 20, UK_GREEN);
    uk_put_pixel(w, ix + 8,  iy + 13, UK_GREEN);
    uk_put_pixel(w, ix + 10, iy + 15, UK_GREEN);
    uk_put_pixel(w, ix + 10, iy + 17, UK_GREEN);
    uk_put_pixel(w, ix + 8,  iy + 19, UK_GREEN);
    /* _ underscore */
    uk_fill_rect(w, ix + 14, iy + 20, 10, 2, UK_TEXT);
}

/* ── Settings icon: gear ────────────────────────────────────────────────────── */
static inline void uk_icon_settings(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_OVERLAY1);
    int cx = ix + 16, cy = iy + 16;
    /* Outer ring (gear body) */
    uk_fill_circle(w, cx, cy, 9, UK_BASE);
    uk_fill_circle(w, cx, cy, 5, UK_SURFACE1);
    /* 4 gear teeth (N/S/E/W) */
    uk_fill_rect(w, cx - 1, cy - 13, 3, 4, UK_BASE);
    uk_fill_rect(w, cx - 1, cy +  9, 3, 4, UK_BASE);
    uk_fill_rect(w, cx - 13, cy - 1, 4, 3, UK_BASE);
    uk_fill_rect(w, cx +  9, cy - 1, 4, 3, UK_BASE);
    /* Centre hole */
    uk_fill_circle(w, cx, cy, 3, UK_SURFACE1);
}

/* ── About icon: info circle ────────────────────────────────────────────────── */
static inline void uk_icon_about(uk_window_t *w, int ix, int iy)
{
    uk_icon_base(w, ix, iy, UK_MAUVE);
    int cx = ix + 16, cy = iy + 16;
    uk_fill_circle(w, cx, cy, 11, UK_BASE);
    /* "i" letter */
    uk_fill_rect(w, cx - 1, cy -  6, 3, 3, UK_MAUVE);  /* dot */
    uk_fill_rect(w, cx - 1, cy -  1, 3, 9, UK_MAUVE);  /* stem */
}

/* ============================================================================
 * IPC helpers
 * ============================================================================ */

/* Send AZ_WM_INVALIDATE to the compositor */
static inline void uk_invalidate(uk_window_t *win)
{
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = win->wid;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&inv);
}

/*
 * uk_window_connect() — full IPC handshake to create a compositor window.
 *
 * Parameters:
 *   win         — output: populated uk_window_t
 *   title       — window title (shown in title bar and taskbar)
 *   x, y        — screen position
 *   w, h        — client area dimensions
 *   map_addr    — virtual address to map the pixel buffer at
 *   server_chan — always 1 (azwm IPC channel)
 *
 * Returns 0 on success, negative on failure.
 */
static inline int uk_window_connect(uk_window_t *win,
                                     const char *title,
                                     int x, int y,
                                     unsigned int w, unsigned int h,
                                     void *map_addr,
                                     int server_chan)
{
    win->server_chan = server_chan;
    win->width       = w;
    win->height      = h;
    win->x           = x;
    win->y           = y;

    win->client_chan = az_channel_create();
    if (win->client_chan < 0) return -1;

    /* Send create request */
    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type       = AZ_WM_CREATE_WINDOW;
    req.client_chan = (unsigned int)win->client_chan;
    req.create.x   = x;
    req.create.y   = y;
    req.create.w   = w;
    req.create.h   = h;

    /* Copy title */
    int i;
    for (i = 0; title[i] && i < 63; i++)
        req.create.title[i] = title[i];
    req.create.title[i] = '\0';

    de_log("[ui_kit] Sending AZ_WM_CREATE_WINDOW...");
    if (az_channel_send(server_chan, (az_ipc_msg_t *)&req) < 0) {
        de_log("[ui_kit] az_channel_send failed.");
        return -2;
    }

    de_log("[ui_kit] Waiting for AZ_WM_WINDOW_CREATED...");
    /* Wait for confirmation */
    az_wm_msg_t resp;
    for (;;) {
        int r = az_channel_recv(win->client_chan, (az_ipc_msg_t *)&resp);
        if (r < 0) {
            de_log("[ui_kit] az_channel_recv failed.");
            return -2;
        }
        if (resp.type == AZ_WM_WINDOW_CREATED) {
            if (resp.created.assigned_wid == 0) {
                de_log("[ui_kit] Server refused window creation.");
                return -1;
            }
            break;
        }
    }
    de_log("[ui_kit] Received AZ_WM_WINDOW_CREATED.");

    win->wid = resp.created.assigned_wid;

    /* Map pixel buffer */
    if (az_shmem_map((int)resp.created.shmem_id, map_addr) < 0) return -3;
    win->pixels = (unsigned int *)map_addr;

    return 0;
}

/*
 * uk_set_zorder() — set a permanent Z-order band for this window.
 */
static inline void uk_set_zorder(uk_window_t *win, unsigned char band)
{
    az_wm_msg_t zmsg;
    memset(&zmsg, 0, sizeof(zmsg));
    zmsg.type = AZ_WM_SET_ZORDER_HINT;
    az_wm_zorder_payload_t *zpl = AZ_WM_MSG_ZORDER(&zmsg);
    zpl->wid  = win->wid;
    zpl->band = band;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&zmsg);
}

/*
 * uk_subscribe_events() — subscribe to broadcast window events.
 */
static inline void uk_subscribe_events(uk_window_t *win)
{
    az_wm_msg_t sub;
    memset(&sub, 0, sizeof(sub));
    sub.type = AZ_WM_SUBSCRIBE_EVENTS;
    az_wm_subscribe_payload_t *pl = AZ_WM_MSG_SUBSCRIBE(&sub);
    pl->subscriber_chan = (unsigned int)win->client_chan;
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&sub);
}

/*
 * uk_launch_app() — request azwm to spawn an ELF binary.
 */
static inline void uk_launch_app(uk_window_t *win, const char *path)
{
    az_wm_msg_t lmsg;
    memset(&lmsg, 0, sizeof(lmsg));
    lmsg.type = AZ_WM_LAUNCH_APP;
    az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
    int j;
    for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
        pl->path[j] = path[j];
    pl->path[j] = '\0';
    az_channel_send(win->server_chan, (az_ipc_msg_t *)&lmsg);
}

/* ============================================================================
 * Separator line helpers
 * ============================================================================ */

static inline void uk_hline(uk_window_t *w, int x, int y, int len, unsigned int col)
{
    uk_fill_rect(w, x, y, len, 1, col);
}

static inline void uk_vline(uk_window_t *w, int x, int y, int len, unsigned int col)
{
    uk_fill_rect(w, x, y, 1, len, col);
}

/* ============================================================================
 * Modern Extended Controls
 * ============================================================================ */

/* Slider control (horizontal) */
static inline void uk_draw_slider(uk_window_t *w, int x, int y, int width, int value_pct, unsigned int fill_col)
{
    int height = 6;
    uk_fill_rounded_rect(w, x, y, width, height, 3, UK_SURFACE0);
    int fill_w = (width * value_pct) / 100;
    if (fill_w > 0)
        uk_fill_rounded_rect(w, x, y, fill_w, height, 3, fill_col);
    int handle_x = x + fill_w - 6;
    if (handle_x < x) handle_x = x;
    if (handle_x > x + width - 12) handle_x = x + width - 12;
    uk_fill_circle(w, handle_x + 6, y + 3, 6, UK_TEXT);
}

/* Checkbox control */
static inline void uk_draw_checkbox(uk_window_t *w, int x, int y, int checked, const char *label)
{
    unsigned int bg = checked ? UK_MAUVE : UK_SURFACE0;
    uk_fill_rounded_rect(w, x, y, 18, 18, 4, bg);
    uk_fill_rounded_rect(w, x + 2, y + 2, 14, 14, 3, checked ? UK_MAUVE : UK_BASE);
    if (checked) {
        uk_draw_text(w, x + 5, y + 1, "v", UK_TEXT);
    }
    if (label && label[0]) {
        uk_draw_text(w, x + 24, y + 1, label, UK_TEXT);
    }
}

/* Status badge pill */
static inline void uk_draw_badge(uk_window_t *w, int x, int y, const char *text, unsigned int bg_col, unsigned int fg_col)
{
    int len = uk_strlen(text);
    int bw = len * 8 + 12;
    uk_fill_rounded_rect(w, x, y, bw, 20, 10, bg_col);
    uk_draw_text(w, x + 6, y + 2, text, fg_col);
}

/* Floating context menu */
static inline void uk_draw_context_menu(uk_window_t *w, int x, int y, const char *items[], int count, int hovered_idx)
{
    int max_w = 120;
    int item_h = 28;
    for (int i = 0; i < count; i++) {
        int l = uk_strlen(items[i]) * 8 + 24;
        if (l > max_w) max_w = l;
    }
    int total_h = count * item_h + 8;
    uk_fill_rounded_rect(w, x, y, max_w, total_h, 8, UK_MANTLE);
    uk_fill_rounded_rect(w, x + 1, y + 1, max_w - 2, total_h - 2, 7, UK_BASE);

    for (int i = 0; i < count; i++) {
        int iy = y + 4 + i * item_h;
        if (i == hovered_idx) {
            uk_fill_rounded_rect(w, x + 4, iy, max_w - 8, item_h - 2, 4, UK_SURFACE0);
        }
        uk_draw_text(w, x + 12, iy + 4, items[i], (i == hovered_idx) ? UK_TEXT : UK_SUBTEXT1);
    }
}

/* Rounded container card */
static inline void uk_draw_card(uk_window_t *w, int x, int y, int width, int height, unsigned int bg_col)
{
    uk_fill_rounded_rect(w, x, y, width, height, 8, bg_col);
    uk_hline(w, x + 4, y, width - 8, UK_SURFACE1);
    uk_hline(w, x + 4, y + height - 1, width - 8, UK_CRUST);
}

/* Glassmorphic card container with top specular highlight */
static inline void uk_draw_card_glass(uk_window_t *w, int x, int y, int width, int height, unsigned int bg_col, const char *header)
{
    uk_fill_rounded_rect(w, x, y, width, height, 8, bg_col);
    /* 1px Specular top highlight */
    for (int px = x + 4; px < x + width - 4; px++) {
        if (px >= 0 && (unsigned int)px < w->width && y >= 0 && (unsigned int)y < w->height) {
            unsigned int *p = &w->pixels[(unsigned int)y * w->width + (unsigned int)px];
            *p = uk_blend(*p, 0xFFFFFFFF, 0x40);
        }
    }
    uk_hline(w, x + 4, y + height - 1, width - 8, UK_CRUST);
    if (header && header[0]) {
        uk_draw_text(w, x + 12, y + 10, header, UK_TEXT);
        uk_hline(w, x + 8, y + 28, width - 16, UK_SURFACE1);
    }
}

/* Modern gradient progress bar with glowing fill */
static inline void uk_draw_progress_bar_modern(uk_window_t *w, int x, int y, int width, int height, int percent, unsigned int color)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    int r = height / 2;
    uk_fill_rounded_rect(w, x, y, width, height, r, UK_SURFACE0);
    int fill_w = (width * percent) / 100;
    if (fill_w > 0) {
        uk_fill_rounded_rect(w, x, y, fill_w, height, r, color);
        /* Glossy shine */
        if (height > 4) {
            for (int px = x + 2; px < x + fill_w - 2; px++) {
                if (px >= 0 && (unsigned int)px < w->width && y >= 0 && (unsigned int)y < w->height) {
                    unsigned int *p = &w->pixels[(unsigned int)y * w->width + (unsigned int)px];
                    *p = uk_blend(*p, 0xFFFFFFFF, 0x50);
                }
            }
        }
    }
}

/* Modern pill toggle switch */
static inline void uk_draw_toggle_modern(uk_window_t *w, int x, int y, int active)
{
    int width = 36;
    int height = 20;
    unsigned int bg = active ? UK_MAUVE : UK_SURFACE1;
    uk_fill_rounded_rect(w, x, y, width, height, 10, bg);
    int knob_x = active ? (x + width - 18) : (x + 2);
    uk_fill_circle(w, knob_x + 8, y + 10, 7, UK_TEXT);
}

