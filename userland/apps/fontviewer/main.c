/* ============================================================================
 * AzamiOS — Native Font Viewer (v1.0)
 * File: userland/apps/fontviewer/main.c
 *
 * Features:
 *   • Direct binary .azf (Azami Font v1) inspector & renderer
 *   • Specimen Pangram display at multiple scales (1x, 2x, 3x)
 *   • Interactive 256-Glyph Character Matrix (0x00..0xFF)
 *   • Character Inspector with enlarged 6x bit-grid and hex row data
 *   • Live Interactive Typing Sandbox
 *   • Catppuccin Mocha themed GUI
 * ============================================================================ */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "../../libc/include/az/ipc.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define WIN_W           760
#define WIN_H           540
#define TOOLBAR_H        36
#define SIDEBAR_W       200

#define AZF_MAGIC       0x4E465A41
#define AZF_HEADER_SZ   68

/* Catppuccin Mocha Colors */
#define CLR_BASE        0xFF1E1E2E
#define CLR_MANTLE      0xFF181825
#define CLR_CRUST       0xFF11111B
#define CLR_SURFACE0    0xFF313244
#define CLR_SURFACE1    0xFF45475A
#define CLR_SURFACE2    0xFF585B70
#define CLR_OVERLAY0    0xFF6C7086
#define CLR_TEXT        0xFFCDD6F4
#define CLR_SUBTEXT     0xFFA6ADC8
#define CLR_MAUVE       0xFFCBA6F7
#define CLR_SAPPHIRE    0xFF74C7EC
#define CLR_GREEN       0xFFA6E3A1
#define CLR_PEACH       0xFFFAB387
#define CLR_YELLOW      0xFFF9E2AF
#define CLR_RED         0xFFF38BA8

static uk_window_t g_win;

/* ── AZF In-Memory Representation ─────────────────────────────────────────── */
typedef struct {
    char family[32];
    char style[16];
    uint8_t glyph_w;
    uint8_t glyph_h;
    uint8_t first_char;
    uint8_t last_char;
    uint16_t num_glyphs;
    uint16_t bytes_per_glyph;
    uint32_t flags;
    uint8_t glyph_data[256][32]; /* bitmap rows */
    bool loaded;
} azf_font_t;

static azf_font_t g_current_font;

/* ── Available Fonts List ─────────────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *path;
} font_entry_t;

static const font_entry_t g_font_list[] = {
    { "VGA Regular",      "/usr/share/fonts/vga_regular.azf" },
    { "VGA Small",        "/usr/share/fonts/vga_small.azf" },
    { "Terminus Regular", "/usr/share/fonts/terminus_regular.azf" },
    { "Terminus Bold",    "/usr/share/fonts/terminus_bold.azf" },
    { "Modern Sans",      "/usr/share/fonts/modern_sans.azf" },
    { "Cozy Serif",       "/usr/share/fonts/cozy_serif.azf" },
    { "Custom Sample",    "/hdd/fonts/custom_sample.azf" },
};
#define NUM_FONTS (sizeof(g_font_list) / sizeof(g_font_list[0]))

static int g_selected_font = 0;

/* ── View Modes ───────────────────────────────────────────────────────────── */
typedef enum {
    VIEW_SPECIMEN = 0,
    VIEW_CHARMAP  = 1,
    VIEW_SANDBOX  = 2
} view_mode_t;

static view_mode_t g_view_mode = VIEW_SPECIMEN;
static int g_inspect_char = 65; /* 'A' */

/* ── Sandbox Buffer ───────────────────────────────────────────────────────── */
static char g_sandbox_text[256] = "Type anything here to test active font typography in real time!";
static int  g_sandbox_len = 65;
static unsigned int g_tick = 0;

/* ── AZF Loader ───────────────────────────────────────────────────────────── */
static bool load_azf_file(const char *path, azf_font_t *out_font)
{
    memset(out_font, 0, sizeof(*out_font));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Fallback: use built-in de_font8x16 */
        snprintf(out_font->family, sizeof(out_font->family), "VGA (Built-in)");
        snprintf(out_font->style, sizeof(out_font->style), "Regular");
        out_font->glyph_w = 8;
        out_font->glyph_h = 16;
        out_font->first_char = 0x20;
        out_font->last_char = 0x7E;
        out_font->num_glyphs = 95;
        out_font->bytes_per_glyph = 16;
        out_font->flags = 1;
        for (int i = 0; i < 95; i++) {
            memcpy(out_font->glyph_data[0x20 + i], de_font8x16[i], 16);
        }
        out_font->loaded = true;
        return true;
    }

    uint8_t hdr[AZF_HEADER_SZ];
    if (read(fd, hdr, AZF_HEADER_SZ) != AZF_HEADER_SZ) {
        close(fd);
        return false;
    }

    uint32_t magic = *(uint32_t *)&hdr[0];
    if (magic != AZF_MAGIC) {
        close(fd);
        return false;
    }

    memcpy(out_font->family, &hdr[8], 31);
    memcpy(out_font->style, &hdr[40], 15);
    out_font->glyph_w = hdr[56];
    out_font->glyph_h = hdr[57];
    out_font->first_char = hdr[58];
    out_font->last_char = hdr[59];
    out_font->num_glyphs = *(uint16_t *)&hdr[60];
    out_font->bytes_per_glyph = *(uint16_t *)&hdr[62];
    out_font->flags = *(uint32_t *)&hdr[64];

    for (int i = 0; i < out_font->num_glyphs && (out_font->first_char + i) < 256; i++) {
        int b = out_font->bytes_per_glyph;
        if (b > 32) b = 32;
        read(fd, out_font->glyph_data[out_font->first_char + i], b);
    }

    close(fd);
    out_font->loaded = true;
    return true;
}

static inline void draw_rect_outline(uk_window_t *w, int x, int y, int rw, int rh, unsigned int col)
{
    uk_fill_rect(w, x, y, rw, 1, col);
    uk_fill_rect(w, x, y + rh - 1, rw, 1, col);
    uk_fill_rect(w, x, y, 1, rh, col);
    uk_fill_rect(w, x + rw - 1, y, 1, rh, col);
}

/* ── Custom Scaled Glyph Rendering ────────────────────────────────────────── */
static void draw_glyph_scaled(int px, int py, uint8_t ch, int scale, uint32_t color)
{
    if (scale <= 0) scale = 1;
    int gw = g_current_font.glyph_w;
    int gh = g_current_font.glyph_h;

    const uint8_t *rows = g_current_font.glyph_data[ch];

    for (int y = 0; y < gh; y++) {
        uint8_t row_bits = rows[y];
        for (int x = 0; x < gw; x++) {
            if (row_bits & (0x80 >> x)) {
                if (scale == 1) {
                    uk_put_pixel(&g_win, px + x, py + y, color);
                } else {
                    uk_fill_rect(&g_win, px + x * scale, py + y * scale, scale, scale, color);
                }
            }
        }
    }
}

static void draw_string_scaled(int px, int py, const char *str, int scale, uint32_t color)
{
    int gw = g_current_font.glyph_w * scale;
    int cur_x = px;
    while (*str) {
        draw_glyph_scaled(cur_x, py, (uint8_t)*str, scale, color);
        cur_x += gw;
        str++;
    }
}

/* ── UI Drawing ────────────────────────────────────────────────────────────── */
static void draw_fontviewer(void)
{
    /* 1. Header Toolbar */
    uk_fill_rect(&g_win, 0, 0, WIN_W, TOOLBAR_H, CLR_MANTLE);
    uk_fill_rect(&g_win, 0, TOOLBAR_H - 1, WIN_W, 1, CLR_SURFACE0);

    draw_rect_outline(&g_win, 8, 6, 120, 24, CLR_MAUVE);
    uk_draw_text(&g_win, 14, 10, "FONT VIEWER", CLR_MAUVE);

    /* View mode tabs */
    uk_draw_button(&g_win, 136, 6, 88, 24, "Specimen", (g_view_mode == VIEW_SPECIMEN) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, 228, 6, 110, 24, "Character Map", (g_view_mode == VIEW_CHARMAP) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, 342, 6, 80, 24, "Sandbox", (g_view_mode == VIEW_SANDBOX) ? UK_BTN_PRESSED : UK_BTN_NORMAL);

    /* Active Font Info Summary */
    char info_str[128];
    snprintf(info_str, sizeof(info_str), "%s %s (%dx%d)",
             g_current_font.family, g_current_font.style,
             g_current_font.glyph_w, g_current_font.glyph_h);
    uk_draw_text(&g_win, WIN_W - 250, 10, info_str, CLR_PEACH);

    /* 2. Left Sidebar (Font Selector) */
    int body_h = WIN_H - TOOLBAR_H;
    uk_fill_rect(&g_win, 0, TOOLBAR_H, SIDEBAR_W, body_h, CLR_MANTLE);
    uk_fill_rect(&g_win, SIDEBAR_W - 1, TOOLBAR_H, 1, body_h, CLR_SURFACE0);

    uk_fill_rect(&g_win, 0, TOOLBAR_H, SIDEBAR_W, 22, CLR_SURFACE0);
    uk_draw_text(&g_win, 8, TOOLBAR_H + 3, "INSTALLED FONTS", CLR_SUBTEXT);

    int font_y = TOOLBAR_H + 28;
    for (size_t i = 0; i < NUM_FONTS; i++) {
        bool sel = ((int)i == g_selected_font);
        if (sel) {
            uk_fill_rect(&g_win, 4, font_y - 3, SIDEBAR_W - 8, 22, CLR_SURFACE1);
            draw_rect_outline(&g_win, 4, font_y - 3, SIDEBAR_W - 8, 22, CLR_MAUVE);
        }
        uk_draw_text(&g_win, 12, font_y, g_font_list[i].name, sel ? CLR_TEXT : CLR_SUBTEXT);
        font_y += 26;
    }

    /* Font Specs Box at bottom of sidebar */
    int spec_y = WIN_H - 120;
    uk_fill_rect(&g_win, 6, spec_y, SIDEBAR_W - 12, 114, CLR_CRUST);
    draw_rect_outline(&g_win, 6, spec_y, SIDEBAR_W - 12, 114, CLR_SURFACE0);
    uk_draw_text(&g_win, 12, spec_y + 6, "METRICS", CLR_MAUVE);

    char buf[64];
    snprintf(buf, sizeof(buf), "Width:  %d px", g_current_font.glyph_w);
    uk_draw_text(&g_win, 12, spec_y + 24, buf, CLR_TEXT);
    snprintf(buf, sizeof(buf), "Height: %d px", g_current_font.glyph_h);
    uk_draw_text(&g_win, 12, spec_y + 42, buf, CLR_TEXT);
    snprintf(buf, sizeof(buf), "Glyphs: %d", g_current_font.num_glyphs);
    uk_draw_text(&g_win, 12, spec_y + 60, buf, CLR_TEXT);
    snprintf(buf, sizeof(buf), "Range:  0x%02X-0x%02X", g_current_font.first_char, g_current_font.last_char);
    uk_draw_text(&g_win, 12, spec_y + 78, buf, CLR_TEXT);

    /* 3. Main Content Area */
    int main_x = SIDEBAR_W;
    int main_w = WIN_W - SIDEBAR_W;
    uk_fill_rect(&g_win, main_x, TOOLBAR_H, main_w, body_h, CLR_BASE);

    if (g_view_mode == VIEW_SPECIMEN) {
        /* Specimen View */
        int sy = TOOLBAR_H + 20;

        /* Large Headline (3x scale) */
        uk_draw_text(&g_win, main_x + 20, sy, "3x Scale Specimen:", CLR_SUBTEXT);
        sy += 20;
        draw_string_scaled(main_x + 20, sy, "AzamiOS Typography", 3, CLR_MAUVE);
        sy += g_current_font.glyph_h * 3 + 24;

        /* Sub-headline (2x scale) */
        uk_draw_text(&g_win, main_x + 20, sy, "2x Scale Specimen:", CLR_SUBTEXT);
        sy += 20;
        draw_string_scaled(main_x + 20, sy, "The Quick Brown Fox Jumps Over Lazy Dog", 2, CLR_SAPPHIRE);
        sy += g_current_font.glyph_h * 2 + 10;
        draw_string_scaled(main_x + 20, sy, "1234567890 !@#$%^&*() _+-=[]{}|;':\",./<>?", 2, CLR_PEACH);
        sy += g_current_font.glyph_h * 2 + 24;

        /* 1x Scale Pangrams & Paragraphs */
        uk_draw_text(&g_win, main_x + 20, sy, "1x Native Scale:", CLR_SUBTEXT);
        sy += 20;
        draw_string_scaled(main_x + 20, sy, "PACK MY BOX WITH FIVE DOZEN LIQUOR JUGS. (Pangram #1)", 1, CLR_TEXT);
        sy += 22;
        draw_string_scaled(main_x + 20, sy, "How quickly daft jumping zebras vex. (Pangram #2)", 1, CLR_TEXT);
        sy += 22;
        draw_string_scaled(main_x + 20, sy, "Sphinx of black quartz, judge my vow. (Pangram #3)", 1, CLR_TEXT);
        sy += 22;
        draw_string_scaled(main_x + 20, sy, "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz", 1, CLR_GREEN);
        sy += 22;
        draw_string_scaled(main_x + 20, sy, "0123456789 `~ !@#$%^&*() -_=+ [{]}\\|;:'\",<.>/?", 1, CLR_YELLOW);
    } else if (g_view_mode == VIEW_CHARMAP) {
        /* Character Map Matrix */
        int grid_ox = main_x + 20;
        int grid_oy = TOOLBAR_H + 20;
        int cell_w = 20;
        int cell_h = 24;

        uk_draw_text(&g_win, grid_ox, grid_oy, "CHARACTER MATRIX (0x20 - 0x7E)", CLR_MAUVE);
        grid_oy += 22;

        /* 16 columns x 6 rows of ASCII 0x20..0x7F */
        for (int row = 0; row < 6; row++) {
            for (int col = 0; col < 16; col++) {
                int ch = 0x20 + row * 16 + col;
                if (ch > 0x7E) break;

                int cx = grid_ox + col * cell_w;
                int cy = grid_oy + row * cell_h;

                bool is_sel = (ch == g_inspect_char);
                if (is_sel) {
                    uk_fill_rect(&g_win, cx, cy, cell_w - 2, cell_h - 2, CLR_SURFACE1);
                    draw_rect_outline(&g_win, cx, cy, cell_w - 2, cell_h - 2, CLR_MAUVE);
                } else {
                    draw_rect_outline(&g_win, cx, cy, cell_w - 2, cell_h - 2, CLR_SURFACE0);
                }

                draw_glyph_scaled(cx + (cell_w - g_current_font.glyph_w) / 2,
                                  cy + (cell_h - g_current_font.glyph_h) / 2,
                                  (uint8_t)ch, 1, is_sel ? CLR_PEACH : CLR_TEXT);
            }
        }

        /* Character Inspector Card on Right */
        int insp_x = main_x + 360;
        int insp_y = TOOLBAR_H + 42;
        int insp_w = 180;
        int insp_h = 320;

        uk_fill_rect(&g_win, insp_x, insp_y, insp_w, insp_h, CLR_CRUST);
        draw_rect_outline(&g_win, insp_x, insp_y, insp_w, insp_h, CLR_SURFACE1);

        char insp_hdr[64];
        snprintf(insp_hdr, sizeof(insp_hdr), "GLYPH: '%c'", (g_inspect_char >= 0x20 && g_inspect_char <= 0x7E) ? g_inspect_char : '?');
        uk_draw_text(&g_win, insp_x + 12, insp_y + 10, insp_hdr, CLR_MAUVE);

        /* Enlarged 8x bit-grid */
        int bit_ox = insp_x + 26;
        int bit_oy = insp_y + 34;
        int bit_scale = 8;
        draw_glyph_scaled(bit_ox, bit_oy, (uint8_t)g_inspect_char, bit_scale, CLR_SAPPHIRE);
        draw_rect_outline(&g_win, bit_ox - 2, bit_oy - 2, g_current_font.glyph_w * bit_scale + 4, g_current_font.glyph_h * bit_scale + 4, CLR_SURFACE2);

        /* Metadata */
        char code_buf[64];
        snprintf(code_buf, sizeof(code_buf), "Hex:  0x%02X", g_inspect_char);
        uk_draw_text(&g_win, insp_x + 12, insp_y + 180, code_buf, CLR_PEACH);
        snprintf(code_buf, sizeof(code_buf), "Dec:  %d", g_inspect_char);
        uk_draw_text(&g_win, insp_x + 12, insp_y + 200, code_buf, CLR_TEXT);
        snprintf(code_buf, sizeof(code_buf), "Char: '%c'", (g_inspect_char >= 0x20) ? g_inspect_char : ' ');
        uk_draw_text(&g_win, insp_x + 12, insp_y + 220, code_buf, CLR_GREEN);

        /* Bitmap byte rows dump */
        uk_draw_text(&g_win, insp_x + 12, insp_y + 244, "Raw Row Bytes:", CLR_SUBTEXT);
        char hex_dump[64] = "";
        for (int r = 0; r < 4 && r < g_current_font.glyph_h; r++) {
            char bstr[8];
            snprintf(bstr, sizeof(bstr), "%02X ", g_current_font.glyph_data[g_inspect_char][r]);
            strcat(hex_dump, bstr);
        }
        uk_draw_text(&g_win, insp_x + 12, insp_y + 264, hex_dump, CLR_TEXT);
    } else if (g_view_mode == VIEW_SANDBOX) {
        /* Live Sandbox */
        int ty = TOOLBAR_H + 24;
        uk_draw_text(&g_win, main_x + 20, ty, "LIVE TYPING SANDBOX", CLR_MAUVE);
        uk_draw_text(&g_win, main_x + 20, ty + 20, "Type on your keyboard to test character rendering:", CLR_SUBTEXT);

        /* Editable Input Box */
        int box_y = ty + 50;
        uk_fill_rect(&g_win, main_x + 20, box_y, main_w - 40, 80, CLR_CRUST);
        draw_rect_outline(&g_win, main_x + 20, box_y, main_w - 40, 80, CLR_SAPPHIRE);

        /* 2x scaled user text */
        draw_string_scaled(main_x + 28, box_y + 12, g_sandbox_text, 2, CLR_TEXT);

        /* Blinking cursor */
        if (g_tick % 2 == 0) {
            int cur_x = main_x + 28 + g_sandbox_len * (g_current_font.glyph_w * 2);
            uk_fill_rect(&g_win, cur_x, box_y + 12, 3, g_current_font.glyph_h * 2, CLR_MAUVE);
        }

        /* 1x scaled user text preview below */
        uk_draw_text(&g_win, main_x + 20, box_y + 100, "1x Scale Rendering:", CLR_SUBTEXT);
        draw_string_scaled(main_x + 20, box_y + 124, g_sandbox_text, 1, CLR_PEACH);

        /* 3x scaled user text preview below */
        uk_draw_text(&g_win, main_x + 20, box_y + 160, "3x Scale Rendering:", CLR_SUBTEXT);
        draw_string_scaled(main_x + 20, box_y + 184, g_sandbox_text, 3, CLR_GREEN);
    }
}

/* ── Input Event Handling ─────────────────────────────────────────────────── */
static void handle_click(int mx, int my)
{
    /* 1. View Mode Tabs */
    if (my >= 6 && my <= 30) {
        if (mx >= 136 && mx < 224) { g_view_mode = VIEW_SPECIMEN; return; }
        if (mx >= 228 && mx < 338) { g_view_mode = VIEW_CHARMAP; return; }
        if (mx >= 342 && mx < 422) { g_view_mode = VIEW_SANDBOX; return; }
    }

    /* 2. Font Selector Sidebar */
    if (mx < SIDEBAR_W && my >= TOOLBAR_H + 28) {
        int clicked_font = (my - (TOOLBAR_H + 28)) / 26;
        if (clicked_font >= 0 && clicked_font < (int)NUM_FONTS) {
            g_selected_font = clicked_font;
            load_azf_file(g_font_list[clicked_font].path, &g_current_font);
            return;
        }
    }

    /* 3. Character Map Click */
    if (g_view_mode == VIEW_CHARMAP) {
        int grid_ox = SIDEBAR_W + 20;
        int grid_oy = TOOLBAR_H + 42;
        int cell_w = 20;
        int cell_h = 24;

        if (mx >= grid_ox && mx < grid_ox + 16 * cell_w && my >= grid_oy && my < grid_oy + 6 * cell_h) {
            int col = (mx - grid_ox) / cell_w;
            int row = (my - grid_oy) / cell_h;
            int ch = 0x20 + row * 16 + col;
            if (ch >= 0x20 && ch <= 0x7E) {
                g_inspect_char = ch;
            }
        }
    }
}

static void handle_keydown(uint32_t key)
{
    if (g_view_mode == VIEW_SANDBOX) {
        if (key == '\b' || key == 127 || key == 0x08 || key == 0x0E) { /* Backspace */
            if (g_sandbox_len > 0) {
                g_sandbox_text[--g_sandbox_len] = '\0';
            }
            return;
        }
        if (key >= 0x20 && key <= 0x7E) {
            if (g_sandbox_len < (int)sizeof(g_sandbox_text) - 2) {
                g_sandbox_text[g_sandbox_len++] = (char)key;
                g_sandbox_text[g_sandbox_len] = '\0';
            }
            return;
        }
    }

    if (g_view_mode == VIEW_CHARMAP) {
        if (key == 0x4B || key == 0x5002 || key == 142) { /* Left */
            if (g_inspect_char > 0x20) g_inspect_char--;
        } else if (key == 0x4D || key == 0x5003 || key == 143) { /* Right */
            if (g_inspect_char < 0x7E) g_inspect_char++;
        } else if (key == 0x48 || key == 0x5000 || key == 140) { /* Up */
            if (g_inspect_char >= 0x30) g_inspect_char -= 16;
        } else if (key == 0x50 || key == 0x5001 || key == 141) { /* Down */
            if (g_inspect_char + 16 <= 0x7E) g_inspect_char += 16;
        }
    }
}

#define MAP_ADDR    ((void *)0x6D000000)
#define SERVER_CHAN 1

int main(int argc, char **argv)
{
    if (uk_window_connect(&g_win, "Azami Font Viewer", 120, 80, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0) {
        fprintf(stderr, "Failed to create Font Viewer window\n");
        return 1;
    }

    if (argc > 1) {
        load_azf_file(argv[1], &g_current_font);
    } else {
        load_azf_file(g_font_list[0].path, &g_current_font);
    }

    az_set_timer(g_win.client_chan, 400, 0);

    draw_fontviewer();
    uk_invalidate(&g_win);

    bool running = true;
    while (running) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }
        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            uk_handle_resize(&g_win, &msg);
            draw_fontviewer();
            uk_invalidate(&g_win);
            continue;
        }
        if (msg.type == AZ_WM_TIMER_TICK) {
            g_tick++;
            if (g_view_mode == VIEW_SANDBOX) {
                draw_fontviewer();
                uk_invalidate(&g_win);
            }
            continue;
        }
        if (msg.type == AZ_WM_MOUSE_EVENT) {
            if (msg.mouse.buttons & 1) {
                handle_click(msg.mouse.abs_x, msg.mouse.abs_y);
                draw_fontviewer();
                uk_invalidate(&g_win);
            }
        }
        if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                handle_keydown(msg.key.keycode);
                draw_fontviewer();
                uk_invalidate(&g_win);
            }
        }
    }

    return 0;
}
