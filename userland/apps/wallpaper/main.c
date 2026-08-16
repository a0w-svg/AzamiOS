/* ============================================================================
 * AzamiOS Desktop Environment — Root Window & Animated Wallpaper
 * File: userland/apps/wallpaper/main.c   v3.0
 *
 * v3.0 Modernization
 * ──────────────────
 *  • Autonomous animation loop via SYS_AZ_SET_TIMER (AZ_WM_TIMER_TICK = 51)
 *  • Dynamic shifting 4-corner Catppuccin Mocha gradient with phase evolution
 *  • Twinkling starfield using deterministic phase offsets
 *  • Subtle procedural grain/noise overlay for film-like depth
 *  • Modernized interactive desktop icons & right-click context menu
 *  • Full ZORDER_BOTTOM lock and seamless root background integration
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/de_log.h"

/* ── Build-time defaults ───────────────────────────────────────────────────── */
#define DEFAULT_WIDTH   1280
#define DEFAULT_HEIGHT   800
#define SERVER_CHAN        1

#define WALLPAPER_MAP_ADDR  ((void *)0x70000000)

/* ============================================================================
 * Color and Math Primitives
 * ============================================================================ */

static inline int clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static unsigned int xorshift32(unsigned int *state)
{
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void wp_fill_rect(unsigned int *px, unsigned int w, unsigned int h,
                         int rx, int ry, int rw, int rh, unsigned int color)
{
    int x, y;
    for (y = ry; y < ry + rh; y++) {
        if (y < 0 || (unsigned int)y >= h) continue;
        for (x = rx; x < rx + rw; x++) {
            if (x < 0 || (unsigned int)x >= w) continue;
            px[(unsigned int)y * w + (unsigned int)x] = color;
        }
    }
}

static unsigned int __attribute__((unused)) wp_blend(unsigned int dst, unsigned int src, unsigned int alpha)
{
    if (alpha > 256) alpha = 256;
    unsigned int sr = (src >> 16) & 0xFF;
    unsigned int sg = (src >>  8) & 0xFF;
    unsigned int sb =  src        & 0xFF;
    unsigned int dr = (dst >> 16) & 0xFF;
    unsigned int dg = (dst >>  8) & 0xFF;
    unsigned int db =  dst        & 0xFF;
    unsigned int r = (sr * alpha + dr * (256 - alpha)) >> 8;
    unsigned int g = (sg * alpha + dg * (256 - alpha)) >> 8;
    unsigned int b = (sb * alpha + db * (256 - alpha)) >> 8;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* ============================================================================
 * Dynamic Animated Gradient Scene
 * ============================================================================ */

/* Integer triangle wave in range [-amplitude, +amplitude] */
static int tri_wave(unsigned int phase, unsigned int period, int amp)
{
    unsigned int p = phase % period;
    unsigned int half = period / 2;
    if (p < half) {
        return -amp + (int)((2 * amp * p) / half);
    } else {
        return amp - (int)((2 * amp * (p - half)) / half);
    }
}

static void wp_render_animated_gradient(unsigned int *px, unsigned int w, unsigned int h, unsigned int phase)
{
    if (w <= 0 || h <= 0) return;

    /* Dynamic corner colors shifted by triangle waves */
    int shift1 = tri_wave(phase, 120, 10);
    int shift2 = tri_wave(phase + 40, 140, 12);
    int shift3 = tri_wave(phase + 80, 160, 8);

    int tl_r = clamp_u8(0x1E + shift1), tl_g = 0x1E,              tl_b = clamp_u8(0x2E + shift2);
    int tr_r = 0x1A,                   tr_g = clamp_u8(0x18 + shift3), tr_b = clamp_u8(0x28 + shift1);
    int bl_r = 0x11,                   bl_g = clamp_u8(0x11 + shift2), bl_b = 0x1B;
    int br_r = clamp_u8(0x18 + shift2), br_g = 0x18,              br_b = clamp_u8(0x25 + shift3);

    int div_h = (h > 1) ? (int)(h - 1) : 1;
    int div_w = (w > 1) ? (int)(w - 1) : 1;

    for (unsigned int y = 0; y < h; y++) {
        int left_r  = tl_r + (bl_r - tl_r) * (int)y / div_h;
        int left_g  = tl_g + (bl_g - tl_g) * (int)y / div_h;
        int left_b  = tl_b + (bl_b - tl_b) * (int)y / div_h;

        int right_r = tr_r + (br_r - tr_r) * (int)y / div_h;
        int right_g = tr_g + (br_g - tr_g) * (int)y / div_h;
        int right_b = tr_b + (br_b - tr_b) * (int)y / div_h;

        int step_r = ((right_r - left_r) << 16) / div_w;
        int step_g = ((right_g - left_g) << 16) / div_w;
        int step_b = ((right_b - left_b) << 16) / div_w;

        int cur_r = left_r << 16;
        int cur_g = left_g << 16;
        int cur_b = left_b << 16;

        unsigned int *line = &px[y * w];
        for (unsigned int x = 0; x < w; x++) {
            unsigned int r = (unsigned int)(cur_r >> 16);
            unsigned int g = (unsigned int)(cur_g >> 16);
            unsigned int b = (unsigned int)(cur_b >> 16);
            line[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            cur_r += step_r;
            cur_g += step_g;
            cur_b += step_b;
        }
    }
}

/* Add subtle grain/film noise for modern texture */
static void wp_render_grain(unsigned int *px, unsigned int w, unsigned int h)
{
    unsigned int rng = 0x1337BEEF;
    unsigned int total = w * h;
    for (unsigned int i = 0; i < total; i += 3) {
        rng = xorshift32(&rng);
        int noise = (int)(rng & 0x07) - 3;
        unsigned int c = px[i];
        int r = (int)((c >> 16) & 0xFF) + noise;
        int g = (int)((c >>  8) & 0xFF) + noise;
        int b = (int)( c        & 0xFF) + noise;
        if (r < 0) { r = 0; }
        if (r > 255) { r = 255; }
        if (g < 0) { g = 0; }
        if (g > 255) { g = 255; }
        if (b < 0) { b = 0; }
        if (b > 255) { b = 255; }
        px[i] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
    }
}

/* Twinkling starfield */
static void wp_render_stars_animated(unsigned int *px, unsigned int w, unsigned int h, unsigned int phase)
{
    if (w == 0 || h == 0) return;
    unsigned int star_h = (h * 8) / 10;
    if (star_h == 0) return;
    unsigned int rng = 0xDEADBEEFu;

    for (unsigned int i = 0; i < 160; i++) {
        rng = xorshift32(&rng);
        unsigned int sx = rng % w;
        rng = xorshift32(&rng);
        unsigned int sy = rng % star_h;
        rng = xorshift32(&rng);

        unsigned int base = px[sy * w + sx];
        unsigned int br   = (base >> 16) & 0xFF;
        unsigned int bg   = (base >>  8) & 0xFF;
        unsigned int bb   =  base        & 0xFF;
        unsigned int lum  = (br + bg + bb) / 3;
        if (lum > 40) continue;

        /* Twinkle modulation */
        int twinkle = tri_wave(phase + i * 17, 60, 40);
        int brightness = 140 + (int)(rng % 60) + twinkle;
        if (brightness < 30) brightness = 30;
        if (brightness > 255) brightness = 255;

        unsigned int star_col = 0xFF000000
                              | ((unsigned int)brightness << 16)
                              | ((unsigned int)brightness << 8)
                              |  (unsigned int)brightness;
        px[sy * w + sx] = star_col;
    }
}

/* Logo and brand rendering */
static void wp_render_logo(unsigned int *px, unsigned int w, unsigned int h)
{
    const char *logo    = "AzamiOS";
    const char *tagline = "v7.0  x86_64 Microkernel";

    int logo_chars = 7;
    int logo_pixel_w = logo_chars * 16;
    int logo_x = (int)w / 2 - logo_pixel_w / 2;
    int logo_y = (int)h / 2 - 50;

    /* Drop Shadow */
    de_font_draw_str_2x(px, w, w, h, logo_x + 3, logo_y + 3, logo, 0xFF0B0B12);

    /* Main Mauve Logo */
    de_font_draw_str_2x(px, w, w, h, logo_x, logo_y, logo, 0xFFCBA6F7);

    /* Subtitle Tagline */
    int tag_chars = 0;
    while (tagline[tag_chars]) tag_chars++;
    int tag_x = (int)w / 2 - (tag_chars * 8) / 2;
    int tag_y = logo_y + 38;
    de_font_draw_str(px, w, w, h, tag_x + 1, tag_y + 1, tagline, 0xFF0B0B12);
    de_font_draw_str(px, w, w, h, tag_x,     tag_y,     tagline, 0xFF6C7086);
}

/* ============================================================================
 * Desktop Icons & Context Menu
 * ============================================================================ */

typedef struct {
    const char *label;
    const char *path;
    int x, y;
    unsigned int color;
} desktop_icon_t;

static desktop_icon_t g_desktop_icons[] = {
    {"Terminal",     "/bin/terminal.elf",     32,  40,  0xFF89B4FA}, /* Blue */
    {"Files",        "/bin/filemanager.elf",  32, 135,  0xFFF9E2AF}, /* Yellow */
    {"Calculator",   "/bin/calculator.elf",   32, 230,  0xFFFAB387}, /* Peach */
    {"Paint",        "/bin/paint.elf",        32, 325,  0xFFA6E3A1}, /* Green */
    {"Settings",     "/bin/settings.elf",     32, 420,  0xFFCBA6F7}, /* Mauve */
    {"Sys Monitor",  "/bin/sysmon.elf",       32, 515,  0xFFF38BA8}, /* Red */
};
#define NUM_DESKTOP_ICONS 6

static int g_selected_icon = -1;
static bool g_context_menu_open = false;
static int g_context_menu_x = 0;
static int g_context_menu_y = 0;
static int g_context_menu_hover = -1;
static int g_wallpaper_theme = 0;

static const char *g_ctx_items[] = {
    "Open Terminal",
    "File Manager",
    "System Monitor",
    "Text Editor",
    "System Settings",
    "Change Theme",
    "About AzamiOS"
};
#define NUM_CTX_ITEMS 7

static void wp_draw_icon(unsigned int *pixels, unsigned int w, unsigned int h, desktop_icon_t *icon, bool selected)
{
    unsigned int bg = selected ? 0x6645475A : 0x33313244;
    wp_fill_rect(pixels, w, h, icon->x, icon->y, 52, 52, bg);
    wp_fill_rect(pixels, w, h, icon->x + 14, icon->y + 14, 24, 24, icon->color);

    for (int i = 0; icon->label[i]; i++) {
        unsigned char c = (unsigned char)icon->label[i];
        if (c >= 0x20 && c < 0x7F) {
            const unsigned char *glyph = de_font8x16[c - 0x20];
            for (int r = 0; r < 16; r++) {
                unsigned char row = glyph[r];
                for (int b = 0; b < 8; b++) {
                    if (row & (0x80 >> b)) {
                        int px = icon->x + i * 8 + b - 4;
                        int py = icon->y + 58 + r;
                        if (px >= 0 && (unsigned int)px < w && py >= 0 && (unsigned int)py < h) {
                            pixels[py * w + px] = 0xFFCDD6F4;
                        }
                    }
                }
            }
        }
    }
}

static void wp_draw_context_menu(unsigned int *pixels, unsigned int w, unsigned int h, int x, int y, int hover)
{
    int menu_w = 160;
    int menu_h = NUM_CTX_ITEMS * 28 + 8;
    if (x + menu_w > (int)w) x = (int)w - menu_w - 4;
    if (y + menu_h > (int)h) y = (int)h - menu_h - 4;

    wp_fill_rect(pixels, w, h, x, y, menu_w, menu_h, 0xFF181825);
    wp_fill_rect(pixels, w, h, x + 1, y + 1, menu_w - 2, menu_h - 2, 0xFF1E1E2E);

    for (int i = 0; i < NUM_CTX_ITEMS; i++) {
        int iy = y + 4 + i * 28;
        if (i == hover) {
            wp_fill_rect(pixels, w, h, x + 4, iy, menu_w - 8, 24, 0xFF45475A);
        }
        for (int j = 0; g_ctx_items[i][j]; j++) {
            unsigned char c = (unsigned char)g_ctx_items[i][j];
            if (c >= 0x20 && c < 0x7F) {
                const unsigned char *glyph = de_font8x16[c - 0x20];
                for (int r = 0; r < 16; r++) {
                    unsigned char row = glyph[r];
                    for (int b = 0; b < 8; b++) {
                        if (row & (0x80 >> b)) {
                            int px = x + 12 + j * 8 + b;
                            int py = iy + 4 + r;
                            if (px >= 0 && (unsigned int)px < w && py >= 0 && (unsigned int)py < h) {
                                pixels[py * w + px] = (i == hover) ? 0xFFCDD6F4 : 0xFFBAC2DE;
                            }
                        }
                    }
                }
            }
        }
    }
}

#define BG_CACHE_MAP_ADDR ((void *)0x66000000)
static unsigned int *g_bg_cache = (unsigned int *)0;

static void wallpaper_render_base(unsigned int *px, unsigned int w, unsigned int h)
{
    if (g_wallpaper_theme == 1) {
        /* Cyber Grid: deep navy to dark cyan */
        for (unsigned int y = 0; y < h; y++) {
            unsigned int r = 10 + (y * 15) / h;
            unsigned int g = 18 + (y * 35) / h;
            unsigned int b = 38 + (y * 55) / h;
            for (unsigned int x = 0; x < w; x++) {
                px[y * w + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    } else if (g_wallpaper_theme == 2) {
        /* Aurora Wave: dark violet to emerald */
        for (unsigned int y = 0; y < h; y++) {
            unsigned int r = 24 + (y * 10) / h;
            unsigned int g = 32 + (y * 45) / h;
            unsigned int b = 40 + (y * 30) / h;
            for (unsigned int x = 0; x < w; x++) {
                px[y * w + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    } else if (g_wallpaper_theme == 3) {
        /* Sunset Dusk: crimson to deep indigo */
        for (unsigned int y = 0; y < h; y++) {
            unsigned int r = 45 - (y * 30) / h;
            unsigned int g = 20 + (y * 10) / h;
            unsigned int b = 40 + (y * 25) / h;
            for (unsigned int x = 0; x < w; x++) {
                px[y * w + x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            }
        }
    } else {
        /* Default Catppuccin Mocha */
        wp_render_animated_gradient(px, w, h, 0);
    }

    wp_render_grain(px, w, h);
    wp_render_logo(px, w, h);
}

static void wallpaper_render_frame(unsigned int *pixels, unsigned int w, unsigned int h, unsigned int phase)
{
    if (g_bg_cache) {
        unsigned long long *dst = (unsigned long long *)pixels;
        const unsigned long long *src = (const unsigned long long *)g_bg_cache;
        size_t qwords = (w * h * sizeof(unsigned int)) / 8;
        for (size_t i = 0; i < qwords; i++) dst[i] = src[i];
    } else {
        wallpaper_render_base(pixels, w, h);
    }

    wp_render_stars_animated(pixels, w, h, phase);

    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
        wp_draw_icon(pixels, w, h, &g_desktop_icons[i], (g_selected_icon == i));
    }

    if (g_context_menu_open) {
        wp_draw_context_menu(pixels, w, h, g_context_menu_x, g_context_menu_y, g_context_menu_hover);
    }
}

static void wp_launch(const char *path)
{
    az_wm_msg_t lmsg;
    memset(&lmsg, 0, sizeof(lmsg));
    lmsg.type = AZ_WM_LAUNCH_APP;
    az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
    int j;
    for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
        pl->path[j] = path[j];
    pl->path[j] = '\0';
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&lmsg);
}

/* ============================================================================
 * _start
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[wallpaper] v3.0 — animated root window starting");

    az_fb_info_t fb;
    unsigned int screen_w = DEFAULT_WIDTH;
    unsigned int screen_h = DEFAULT_HEIGHT;

    if (az_fb_info(&fb) == 0) {
        if (fb.width  > 0 && fb.width  <= 7680) screen_w = fb.width;
        if (fb.height > 0 && fb.height <= 4320) screen_h = fb.height;
    }

    int client_chan = az_channel_create();
    if (client_chan < 0) {
        de_log("[wallpaper] FATAL: az_channel_create failed");
        return -1;
    }

    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type            = AZ_WM_CREATE_WINDOW;
    req.client_chan      = (unsigned int)client_chan;
    req.create.x        = 0;
    req.create.y        = 0;
    req.create.w        = screen_w;
    req.create.h        = screen_h;
    req.create.title[0] = '\0';   /* blank title */

    if (az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&req) < 0) {
        de_log("[wallpaper] FATAL: channel_send failed");
        return -1;
    }

    az_wm_msg_t resp;
    unsigned int wid = 0;
    int shmem_id = -1;
    for (;;) {
        int r = az_channel_recv(client_chan, (az_ipc_msg_t *)&resp);
        if (r < 0) {
            de_log("[wallpaper] FATAL: recv failed");
            return -1;
        }
        if (resp.type == AZ_WM_WINDOW_CREATED) {
            wid      = resp.created.assigned_wid;
            shmem_id = (int)resp.created.shmem_id;
            break;
        }
    }

    if (az_shmem_map(shmem_id, WALLPAPER_MAP_ADDR) < 0) {
        de_log("[wallpaper] FATAL: shmem_map failed");
        return -1;
    }
    unsigned int *pixels = (unsigned int *)WALLPAPER_MAP_ADDR;

    /* Allocate background cache buffer */
    int bg_shmem = az_shmem_create(screen_w * screen_h * sizeof(unsigned int));
    if (bg_shmem >= 0 && az_shmem_map(bg_shmem, BG_CACHE_MAP_ADDR) == 0) {
        g_bg_cache = (unsigned int *)BG_CACHE_MAP_ADDR;
        wallpaper_render_base(g_bg_cache, screen_w, screen_h);
    }

    unsigned int phase = 0;
    wallpaper_render_frame(pixels, screen_w, screen_h, phase);

    /* Lock to bottom */
    az_wm_msg_t zmsg;
    memset(&zmsg, 0, sizeof(zmsg));
    zmsg.type = AZ_WM_SET_ZORDER_HINT;
    az_wm_zorder_payload_t *zpl = AZ_WM_MSG_ZORDER(&zmsg);
    zpl->wid  = wid;
    zpl->band = AZ_WM_ZORDER_BOTTOM;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&zmsg);

    /* Invalidate initial frame */
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);

    /* Register autonomous timer for smooth periodic animation (~200ms = 5 FPS) */
    az_set_timer(client_chan, 200, 0);

    de_log("[wallpaper] Entering autonomous animation & event loop.");

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) {
            de_log("[wallpaper] IPC channel disconnected, exiting loop.");
            break;
        }

        switch (msg.type) {
        case AZ_WM_TIMER_TICK:
            phase++;
            wallpaper_render_frame(pixels, screen_w, screen_h, phase);
            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);
            break;

        case AZ_WM_MOUSE_EVENT: {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            bool lclick = (msg.mouse.buttons & 1) && !(prev_btn & 1);
            bool rclick = (msg.mouse.buttons & 2) && !(prev_btn & 2);
            prev_btn = msg.mouse.buttons;

            bool redraw = false;

            if (g_context_menu_open) {
                int menu_w = 150;
                int item_h = 28;
                int old_h = g_context_menu_hover;
                if (mx >= g_context_menu_x && mx < g_context_menu_x + menu_w &&
                    my >= g_context_menu_y && my < g_context_menu_y + NUM_CTX_ITEMS * item_h + 8) {
                    g_context_menu_hover = (my - (g_context_menu_y + 4)) / item_h;
                    if (g_context_menu_hover < 0 || g_context_menu_hover >= NUM_CTX_ITEMS) g_context_menu_hover = -1;
                } else {
                    g_context_menu_hover = -1;
                }
                if (g_context_menu_hover != old_h) redraw = true;

                if (lclick) {
                    if (g_context_menu_hover >= 0) {
                        switch (g_context_menu_hover) {
                        case 0: wp_launch("/bin/terminal.elf"); break;
                        case 1: wp_launch("/bin/filemanager.elf"); break;
                        case 2: wp_launch("/bin/sysmon.elf"); break;
                        case 3: wp_launch("/bin/texteditor.elf"); break;
                        case 4: wp_launch("/bin/settings.elf"); break;
                        case 5:
                            g_wallpaper_theme = (g_wallpaper_theme + 1) % 4;
                            if (g_bg_cache) {
                                wallpaper_render_base(g_bg_cache, screen_w, screen_h);
                            }
                            break;
                        case 6: wp_launch("/bin/about.elf"); break;
                        }
                    }
                    g_context_menu_open = false;
                    redraw = true;
                }
            } else {
                if (rclick) {
                    g_context_menu_open = true;
                    g_context_menu_x = mx;
                    g_context_menu_y = my;
                    g_context_menu_hover = -1;
                    redraw = true;
                } else if (lclick) {
                    int clicked_icon = -1;
                    for (int i = 0; i < NUM_DESKTOP_ICONS; i++) {
                        if (mx >= g_desktop_icons[i].x && mx < g_desktop_icons[i].x + 52 &&
                            my >= g_desktop_icons[i].y && my < g_desktop_icons[i].y + 52) {
                            clicked_icon = i;
                            break;
                        }
                    }
                    if (clicked_icon >= 0) {
                        if (g_selected_icon == clicked_icon) {
                            wp_launch(g_desktop_icons[clicked_icon].path);
                            g_selected_icon = -1;
                        } else {
                            g_selected_icon = clicked_icon;
                        }
                        redraw = true;
                    } else {
                        if (g_selected_icon != -1) {
                            g_selected_icon = -1;
                            redraw = true;
                        }
                    }
                }
            }

            if (redraw) {
                wallpaper_render_frame(pixels, screen_w, screen_h, phase);
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);
            }
            break;
        }

        case AZ_WM_FOCUS_CHANGE:
            if (msg.focus.focused) {
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&zmsg);
            }
            break;

        default:
            break;
        }
    }

    sys_exit(0);
}
