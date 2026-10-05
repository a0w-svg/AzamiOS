#include "settings.h"


/* ── Display Tab ───────────────────────────────────────────────────────────── */
#define DISP_SEC1_Y   90
#define DISP_PANEL_Y  122
#define DISP_SEC2_Y   176
#define DISP_TOG1_Y   208
#define DISP_TOG2_Y   244
#define DISP_TOG3_Y   280

static az_font_info_t s_font_list[16];
static int s_font_count = -1;
static char s_active_font_path[128];

static void init_fonts(void)
{
    if (s_font_count >= 0) return;
    s_font_count = az_font_scan_dirs(s_font_list, 16);
    az_font_t *font = az_font_load_default();
    if (font) {
        strncpy(s_active_font_path, font->path, sizeof(s_active_font_path) - 1);
        az_font_free(font);
    }
}

int display_content_height(void)
{
    init_fonts();
    int bottom = 340 + ((s_font_count + 2) / 3) * 64 + 12;
    return bottom < 464 ? 464 : bottom;
}

void draw_display_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, DISP_SEC1_Y, (int)w - 40, "Framebuffer Display", UK_BLUE);

    az_fb_info_t fb;
    char res[64];
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        snprintf(res, sizeof(res), "%ux%u @ %u bpp", fb.width, fb.height, fb.bpp);
    } else {
        snprintf(res, sizeof(res), "Framebuffer information unavailable");
    }

    uk_draw_panel(&g_win, px, DISP_PANEL_Y, (int)w - 40, 42, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, DISP_PANEL_Y + 6,  "Hardware Resolution", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, DISP_PANEL_Y + 22, res, UK_TEXT);

    uk_draw_section_header(&g_win, px, DISP_SEC2_Y, (int)w - 40, "Compositor & Rendering", UK_MAUVE);

    draw_toggle(px, DISP_TOG1_Y, g_vsync,     "VSync Double Page Flipping");
    uk_draw_text(&g_win, px, DISP_TOG2_Y, "Alpha blending: provided by the desktop compositor", UK_SUBTEXT0);
    uk_draw_text(&g_win, px, DISP_TOG3_Y, "Cursor smoothing: controlled by the display backend", UK_SUBTEXT0);
    uk_draw_text_small(&g_win, px + 350, DISP_TOG1_Y + 3, "Requires desktop restart", UK_OVERLAY0);

    /* Font Subsystem Section */
    #define FONT_SEC_Y     312
    #define FONT_GRID_Y    340
    #define FONT_CARD_W    216
    #define FONT_CARD_H    54
    #define FONT_CARD_GAP  16

    init_fonts();

    uk_draw_section_header(&g_win, px, FONT_SEC_Y, (int)w - 40, "Typography & Fonts (/usr/share/fonts, /hdd/fonts)", UK_SAPPHIRE);

    int max_disp = s_font_count;
    for (int i = 0; i < max_disp; i++) {
        int col = i % 3;
        int row = i / 3;
        int card_x = px + col * (FONT_CARD_W + FONT_CARD_GAP);
        int card_y = FONT_GRID_Y + row * (FONT_CARD_H + 10);

        bool is_sel = strcmp(s_font_list[i].path, s_active_font_path) == 0;
        unsigned int border_col = is_sel ? UK_SAPPHIRE : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, card_x, card_y, FONT_CARD_W, FONT_CARD_H, 6, UK_SURFACE0);
        uk_draw_rounded_rect_outline(&g_win, card_x, card_y, FONT_CARD_W, FONT_CARD_H, 6, border_col);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, card_x + 2, card_y + 2, 4, FONT_CARD_H - 4, 2, UK_SAPPHIRE);
        }

        char title_buf[32];
        snprintf(title_buf, sizeof(title_buf), "%s %s", s_font_list[i].name, s_font_list[i].style);
        uk_draw_text(&g_win, card_x + 10, card_y + 8, title_buf, is_sel ? UK_TEXT : UK_SUBTEXT1);

        char size_buf[32];
        snprintf(size_buf, sizeof(size_buf), "%dx%d %s", s_font_list[i].glyph_w, s_font_list[i].glyph_h,
                 strstr(s_font_list[i].path, "/hdd/") ? "[HDD]" : "[SYS]");
        uk_draw_text_small(&g_win, card_x + 10, card_y + 28, size_buf, UK_OVERLAY0);

        if (is_sel) {
            uk_draw_badge(&g_win, card_x + FONT_CARD_W - 54, card_y + 14, "Active", UK_SURFACE1, UK_SAPPHIRE);
        }
    }
}


void handle_display_mouse(int mx, int my)
{
    if (hit_toggle(20, DISP_TOG1_Y, mx, my)) {
        g_vsync ^= 1;
        save_display_settings();
        draw_settings();
        return;
    }
    init_fonts();
    for (int i = 0; i < s_font_count; i++) {
        int x = 20 + (i % 3) * 232, y = 340 + (i / 3) * 64;
        if (mx < x || mx >= x + 216 || my < y || my >= y + 54) continue;
        az_font_t *font = az_font_load(s_font_list[i].path);
        if (!font || az_font_set_default_font(s_font_list[i].path) < 0) {
            if (font) az_font_free(font);
            snprintf(g_settings_status, sizeof(g_settings_status), "Font selection failed: could not load or save font.");
        } else {
            strncpy(s_active_font_path, s_font_list[i].path, sizeof(s_active_font_path) - 1);
            if (g_win.font) az_font_free(g_win.font);
            g_win.font = font;
            snprintf(g_settings_status, sizeof(g_settings_status), "Font applied here. Taskbar updates within one second; other apps reload on restart.");
        }
        draw_settings();
        return;
    }
}
