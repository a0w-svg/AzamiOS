/* ============================================================================
 * AzamiOS — Desktop Rendering Implementation
 * File: user/apps/azwm/desktop.c
 *
 * Provides the visual desktop environment: gradient background, taskbar
 * with window buttons, mouse cursor sprite, and bitmap font rendering.
 * ============================================================================ */

#include "desktop.h"
#include "de_font.h"

/* ── Character & String drawing (delegated to de_font.h) ─────────────────── */

void desktop_draw_char_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, char c, unsigned int color)
{
    de_font_draw_char(buf, pitch_px, w, h, x, y, c, color);
}

void desktop_draw_text_at(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          int x, int y, const char *text, unsigned int color)
{
    de_font_draw_str(buf, pitch_px, w, h, x, y, text, color);
}

/* ── Desktop background (dark Catppuccin gradient) ────────────────────────── */

void desktop_draw_background(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px)
{
    for (unsigned int y = 0; y < h; y++) {
        unsigned int *line = &buf[y * pitch_px];
        unsigned int r_base = 30 + ((y * 10) / h);
        unsigned int g_base = 30 + ((y * 20) / h);
        unsigned int b_base = 46 + ((y * 40) / h);
        for (unsigned int x = 0; x < w; x++) {
            unsigned int r = r_base + ((x * 20) / w);
            unsigned int g = g_base + ((x * 10) / w);
            unsigned int b = b_base;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            line[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    /* Draw the AzamiOS logo text in the center */
    const char *logo = "AzamiOS";
    int logo_len = 7;
    int logo_y = (int)(h / 2) - 40;
    int logo_start_x = (int)(w / 2) - (logo_len * 16 / 2);

    /* Main text — large (draw each char 2x) with drop shadow */
    for (int i = 0; logo[i]; i++) {
        de_font_draw_char_2x(buf, pitch_px, w, h, logo_start_x + i * 16 + 2, logo_y + 2, logo[i], 0xFF11111B);
        de_font_draw_char_2x(buf, pitch_px, w, h, logo_start_x + i * 16,     logo_y,     logo[i], 0xFF89B4FA);
    }

    /* Version subtitle */
    const char *ver = "v7.0 Microkernel - Graphical Desktop";
    int ver_len = 0;
    while (ver[ver_len]) ver_len++;
    int ver_x = (int)(w / 2) - (ver_len * 8 / 2);
    desktop_draw_text_at(buf, w, h, pitch_px, ver_x + 1, logo_y + 37, ver, 0xFF11111B);
    desktop_draw_text_at(buf, w, h, pitch_px, ver_x,     logo_y + 36, ver, 0xFF9399B2);
}

/* ── Taskbar (bottom of screen) ───────────────────────────────────────────── */

#define TASKBAR_HEIGHT  32

void desktop_draw_taskbar(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                          az_window_t *windows, unsigned int max_windows)
{
    int tb_y = (int)h - TASKBAR_HEIGHT;

    /* Taskbar background — semi-transparent dark */
    for (int y = tb_y; y < (int)h; y++) {
        for (unsigned int x = 0; x < w; x++) {
            buf[(unsigned int)y * pitch_px + x] = 0xFF11111B;
        }
    }

    /* Top border line */
    for (unsigned int x = 0; x < w; x++) {
        buf[(unsigned int)tb_y * pitch_px + x] = 0xFF313244;
    }

    /* AzamiOS button on the left */
    for (int y = tb_y + 2; y < (int)h - 2; y++) {
        for (int x = 4; x < 80; x++) {
            buf[(unsigned int)y * pitch_px + (unsigned int)x] = 0xFF1E1E2E;
        }
    }
    desktop_draw_text_at(buf, w, h, pitch_px, 12, tb_y + 8, "AzamiOS", 0xFFCBA6F7);

    /* Window buttons — skip system windows with blank titles (e.g. wallpaper) */
    int btn_x = 90;
    for (unsigned int i = 0; i < max_windows; i++) {
        if (windows[i].wid == 0) continue;
        if (windows[i].title[0] == '\0') continue; /* skip titleless system windows */

        unsigned int btn_bg = windows[i].focused ? 0xFF313244 : 0xFF1E1E2E;
        unsigned int btn_fg = windows[i].focused ? 0xFFCDD6F4 : 0xFF6C7086;

        for (int y = tb_y + 4; y < (int)h - 4; y++) {
            for (int x = btn_x; x < btn_x + 120 && x < (int)w - 4; x++) {
                buf[(unsigned int)y * pitch_px + (unsigned int)x] = btn_bg;
            }
        }
        if (windows[i].focused) {
            for (int y = (int)h - 6; y < (int)h - 4; y++) {
                for (int x = btn_x; x < btn_x + 120 && x < (int)w - 4; x++) {
                    buf[(unsigned int)y * pitch_px + (unsigned int)x] = 0xFFCBA6F7;
                }
            }
        }

        desktop_draw_text_at(buf, w, h, pitch_px, btn_x + 8, tb_y + 8,
                             windows[i].title, btn_fg);

        btn_x += 124;
    }
}

/* ── Mouse cursor (pre-rendered 14×21 sprite with black border & white body) ─ */

#define C_TRN 0x00000000U
#define C_OUT 0xFF000000U
#define C_WHT 0xFFFFFFFFU

static const unsigned int g_cursor_sprite[21][14] = {
    { C_OUT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_WHT, C_WHT, C_WHT, C_OUT, C_OUT, C_OUT, C_OUT, C_OUT, C_OUT, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_WHT, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_WHT, C_OUT, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_OUT, C_TRN, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_OUT, C_WHT, C_WHT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_OUT, C_OUT, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
    { C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN, C_TRN },
};

void desktop_draw_cursor(unsigned int *buf, unsigned int w, unsigned int h, unsigned int pitch_px,
                         int cx, int cy)
{
    for (int row = 0; row < 21; row++) {
        int py = cy + row;
        if (py < 0 || py >= (int)h) continue;
        for (int col = 0; col < 14; col++) {
            int px = cx + col;
            if (px < 0 || px >= (int)w) continue;
            unsigned int pixel = g_cursor_sprite[row][col];
            if (pixel != C_TRN) {
                buf[(unsigned int)py * pitch_px + (unsigned int)px] = pixel;
            }
        }
    }
}
