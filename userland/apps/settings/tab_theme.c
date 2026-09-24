#include "settings.h"


/* ── Theme Tab ─────────────────────────────────────────────────────────────── */
#define THEME_SEC_Y    90
#define THEME_GRID_Y   122
#define THEME_CARD_W   300
#define THEME_CARD_H   60
#define THEME_CARD_GAP 16

void draw_theme_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, THEME_SEC_Y, (int)w - 40, "Desktop Themes & Color Palettes", UK_MAUVE);

    int theme_count = az_theme_count();
    for (int i = 0; i < theme_count; i++) {
        const az_theme_t *th = az_theme_get(i);
        int col = i % 2;
        int row = i / 2;
        int card_x = px + col * (THEME_CARD_W + THEME_CARD_GAP);
        int card_y = THEME_GRID_Y + row * (THEME_CARD_H + THEME_CARD_GAP);

        bool is_sel = (i == g_theme_selected);
        unsigned int border_col = is_sel ? UK_MAUVE : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, card_x, card_y, THEME_CARD_W, THEME_CARD_H, 8, th->base);
        uk_draw_rounded_rect_outline(&g_win, card_x, card_y, THEME_CARD_W, THEME_CARD_H, 8, border_col);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, card_x + 2, card_y + 2, 4, THEME_CARD_H - 4, 2, UK_MAUVE);
        }

        /* Swatch dots */
        uk_fill_circle(&g_win, card_x + 22, card_y + 24, 8, th->accent);
        uk_fill_circle(&g_win, card_x + 42, card_y + 24, 8, th->text);
        uk_fill_circle(&g_win, card_x + 62, card_y + 24, 8, th->base);

        /* Title & Desc */
        uk_draw_text(&g_win, card_x + 82, card_y + 14, th->name, is_sel ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, card_x + 82, card_y + 32, theme_brightness_label(th), UK_OVERLAY0);

        if (is_sel) {
            uk_draw_badge(&g_win, card_x + THEME_CARD_W - 54, card_y + 18, "Active", UK_SURFACE1, UK_MAUVE);
        }
    }
}


void handle_theme_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    int theme_count = az_theme_count();
                    for (int i = 0; i < theme_count; i++) {
                        int col = i % 2;
                        int row = i / 2;
                        int card_x = 20 + col * (THEME_CARD_W + THEME_CARD_GAP);
                        int card_y = THEME_GRID_Y + row * (THEME_CARD_H + THEME_CARD_GAP);
                        if (mx >= card_x && mx < card_x + THEME_CARD_W &&
                            my >= card_y && my < card_y + THEME_CARD_H) {
                            apply_theme(i);
                            draw_settings();
                            break;
                        }
                    }
}
