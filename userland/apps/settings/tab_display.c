#include "settings.h"


/* ── Display Tab ───────────────────────────────────────────────────────────── */
#define DISP_SEC1_Y   90
#define DISP_PANEL_Y  122
#define DISP_SEC2_Y   176
#define DISP_TOG1_Y   208
#define DISP_TOG2_Y   244
#define DISP_TOG3_Y   280

void draw_display_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, DISP_SEC1_Y, (int)w - 40, "Framebuffer Display", UK_BLUE);

    az_fb_info_t fb;
    char res[64];
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        snprintf(res, sizeof(res), "%ux%u @ 32 bpp (Bochs BGA)", fb.width, fb.height);
    } else {
        snprintf(res, sizeof(res), "1280x800 @ 32 bpp (Bochs BGA)");
    }

    uk_draw_panel(&g_win, px, DISP_PANEL_Y, (int)w - 40, 42, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, DISP_PANEL_Y + 6,  "Hardware Resolution", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, DISP_PANEL_Y + 22, res, UK_TEXT);

    uk_draw_section_header(&g_win, px, DISP_SEC2_Y, (int)w - 40, "Compositor & Rendering", UK_MAUVE);

    draw_toggle(px, DISP_TOG1_Y, g_vsync,     "VSync Double Page Flipping");
    draw_toggle(px, DISP_TOG2_Y, g_composit,  "Compositor Alpha Blending");
    draw_toggle(px, DISP_TOG3_Y, g_cursor_aa, "Hardware Cursor Anti-Aliasing");

    /* Font Subsystem Section */
    #define FONT_SEC_Y     312
    #define FONT_GRID_Y    340
    #define FONT_CARD_W    216
    #define FONT_CARD_H    54
    #define FONT_CARD_GAP  16

    static az_font_info_t s_font_list[16];
    static int s_font_count = -1;
    static char s_active_font_path[128] = {0};

    if (s_font_count < 0) {
        s_font_count = az_font_scan_dirs(s_font_list, 16);
        az_font_t *cur = az_font_load_default();
        if (cur) {
            strncpy(s_active_font_path, cur->path, sizeof(s_active_font_path) - 1);
            az_font_free(cur);
        }
    }

    uk_draw_section_header(&g_win, px, FONT_SEC_Y, (int)w - 40, "Typography & Fonts (/usr/share/fonts, /hdd/fonts)", UK_SAPPHIRE);

    int max_disp = (s_font_count > 6) ? 6 : s_font_count;
    for (int i = 0; i < max_disp; i++) {
        int col = i % 3;
        int row = i / 3;
        int card_x = px + col * (FONT_CARD_W + FONT_CARD_GAP);
        int card_y = FONT_GRID_Y + row * (FONT_CARD_H + 10);

        bool is_sel = (strcmp(s_font_list[i].path, s_active_font_path) == 0 ||
                       strstr(s_active_font_path, s_font_list[i].name) != NULL);
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
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    if (hit_toggle(20, DISP_TOG1_Y, mx, my)) { g_vsync    ^= 1; save_display_settings(); draw_settings(); return; }
                    if (hit_toggle(20, DISP_TOG2_Y, mx, my)) { g_composit ^= 1; save_display_settings(); draw_settings(); return; }
                    if (hit_toggle(20, DISP_TOG3_Y, mx, my)) { g_cursor_aa^= 1; save_display_settings(); draw_settings(); return; }

                    /* Font card clicks */
                    az_font_info_t flist[16];
                    int fcnt = az_font_scan_dirs(flist, 16);
                    int max_d = (fcnt > 6) ? 6 : fcnt;
                    for (int i = 0; i < max_d; i++) {
                        int col = i % 3;
                        int row = i / 3;
                        int card_x = 20 + col * (216 + 16);
                        int card_y = 340 + row * (54 + 10);
                        if (mx >= card_x && mx < card_x + 216 &&
                            my >= card_y && my < card_y + 54) {
                            az_font_set_default_font(flist[i].path);
                            draw_settings();
                            break;
                        }
                    }
}
