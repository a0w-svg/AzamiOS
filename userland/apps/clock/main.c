/* ============================================================================
 * AzamiOS — Clock
 * File: userland/apps/clock/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       380
#define WIN_H       260
#define MAP_ADDR    ((void *)0x63000000)

static uk_window_t g_win;
static unsigned int g_tick = 0;

static void uint2str2(unsigned int v, char *buf)
{
    buf[0] = '0' + (char)((v / 10) % 10);
    buf[1] = '0' + (char)(v % 10);
    buf[2] = '\0';
}

static void draw_clock(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    /* Background */
    uk_gradient_v(&g_win, 0, 0, (int)w, (int)h, UK_MANTLE, UK_CRUST);

    /* ── Analog clock face ─────────────────────────────────────────────── */
    int cx = (int)w / 2;
    int cy = 120;
    int r  = 80;

    /* Outer ring */
    uk_fill_circle(&g_win, cx, cy, r + 4, UK_SURFACE1);
    uk_fill_circle(&g_win, cx, cy, r + 2, UK_MAUVE);
    uk_fill_circle(&g_win, cx, cy, r,     UK_BASE);

    /* Helper to fill a rectangle between two points regardless of direction */
    #define DRAW_SEGMENT(x0, y0, x1, y1, thick, col) do { \
        int rx = ((x0) < (x1)) ? (x0) : (x1); \
        int ry = ((y0) < (y1)) ? (y0) : (y1); \
        int rw = ((x0) < (x1)) ? ((x1) - (x0) + (thick)) : ((x0) - (x1) + (thick)); \
        int rh = ((y0) < (y1)) ? ((y1) - (y0) + (thick)) : ((y0) - (y1) + (thick)); \
        uk_fill_rect(&g_win, rx, ry, rw, rh, (col)); \
    } while(0)

    /* Hour markers (12 ticks) */
    int m;
    for (m = 0; m < 12; m++) {
        /* Use integer approximation of sin/cos via lookup */
        static const int sin12[12] = { 0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50 };
        static const int cos12[12] = { 100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87 };
        int inner = r - 10;
        int outer = r - 2;
        int x0 = cx + inner * sin12[m] / 100;
        int y0 = cy - inner * cos12[m] / 100;
        int x1 = cx + outer * sin12[m] / 100;
        int y1 = cy - outer * cos12[m] / 100;
        DRAW_SEGMENT(x0, y0, x1, y1, 2, UK_SURFACE2);
        /* Single pixel endpoints */
        uk_put_pixel(&g_win, x1, y1, UK_TEXT);
    }

    /* Time calculation from syscall */
    unsigned long ts[2] = {0, 0}; /* tv_sec, tv_nsec */
    syscall2(SYS_clock_gettime, 0, (long)ts);
    
    unsigned int unix_sec = (unsigned int)ts[0];
    
    unsigned int s = unix_sec % 60;
    unsigned int mn = (unix_sec / 60) % 60;
    unsigned int hr = (unix_sec / 3600) % 24;

    /* Hour hand */
    int hr_angle = (int)hr * 30 + (int)mn / 2; /* degrees */
    /* Use integer sin/cos (x100) */
    static const int sin360[12] = { 0, 50, 87, 100, 87, 50, 0, -50, -87, -100, -87, -50 };
    static const int cos360[12] = { 100, 87, 50, 0, -50, -87, -100, -87, -50, 0, 50, 87 };
    int hi = (hr_angle / 30) % 12;
    int hx = cx + (r - 25) * sin360[hi] / 100;
    int hy = cy - (r - 25) * cos360[hi] / 100;
    DRAW_SEGMENT(cx, cy, hx, hy, 4, UK_TEXT);

    /* Minute hand */
    int mi = (int)(mn / 5) % 12;
    int mx2 = cx + (r - 12) * sin360[mi] / 100;
    int my2 = cy - (r - 12) * cos360[mi] / 100;
    DRAW_SEGMENT(cx, cy, mx2, my2, 2, UK_LAVENDER);

    /* Second hand */
    int si = (int)(s / 5) % 12;
    int sx2 = cx + (r - 6) * sin360[si] / 100;
    int sy2 = cy - (r - 6) * cos360[si] / 100;
    DRAW_SEGMENT(cx, cy, sx2, sy2, 1, UK_RED);

    /* Centre cap */
    uk_fill_circle(&g_win, cx, cy, 5, UK_MAUVE);
    uk_fill_circle(&g_win, cx, cy, 3, UK_TEXT);

    /* ── Digital time display ───────────────────────────────────────────── */
    char ts_str[9];
    char h2[3], m2[3], s2[3];
    uint2str2(hr, h2);
    uint2str2(mn, m2);
    uint2str2(s,  s2);
    ts_str[0]=h2[0]; ts_str[1]=h2[1]; ts_str[2]=':';
    ts_str[3]=m2[0]; ts_str[4]=m2[1]; ts_str[5]=':';
    ts_str[6]=s2[0]; ts_str[7]=s2[1]; ts_str[8]='\0';

    /* Backdrop for digital */
    uk_fill_rounded_rect(&g_win, cx - 60, (int)h - 48, 120, 32, 6, UK_SURFACE0);
    uk_draw_text_2x(&g_win, cx - 56, (int)h - 42, ts_str, UK_GREEN);

    /* Day indicator */
    uk_draw_text_centred(&g_win, cx, (int)h - 10,
                         "AzamiOS Clock  —  Monotonic", UK_OVERLAY0);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("[clock] Starting...");

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "Clock",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { puts("[clock] FATAL"); return -1; }

    draw_clock();

    /* Register autonomous 1-second timer tick (Bug 10 fix) */
    az_set_timer(g_win.client_chan, 1000, 0);

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_TIMER_TICK || msg.type == AZ_WM_MOUSE_EVENT || msg.type == AZ_WM_FOCUS_CHANGE) {
            g_tick++;
            draw_clock();
        }
    }
    sys_exit(0);
}
