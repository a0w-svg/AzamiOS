#include "settings.h"


#define TIME_SEC1_Y    86
#define TIME_PREV_Y    114
#define TIME_SEC2_Y    172
#define TIME_GRID_Y    200
#define TIME_CARD_W    300
#define TIME_CARD_H    44
#define TIME_CARD_GAP   8

void draw_time_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, TIME_SEC1_Y, (int)w - 40, "Live System Time & Calendar", UK_PEACH);

    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);

    char date_str[64];
    strftime(date_str, sizeof(date_str), "%A, %B %e, %Y  •  %T  %Z", &tm_info);

    uk_draw_panel(&g_win, px, TIME_PREV_Y, (int)w - 40, 44, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, TIME_PREV_Y + 6,  "Current Local Time & Date", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, TIME_PREV_Y + 24, date_str, UK_GREEN);

    uk_draw_section_header(&g_win, px, TIME_SEC2_Y, (int)w - 40, "Select System Timezone (/etc/timezone)", UK_MAUVE);

    for (int i = 0; i < 8; i++) {
        int col = i % 2;
        int row = i / 2;
        int card_x = px + col * (TIME_CARD_W + TIME_CARD_GAP + 12);
        int card_y = TIME_GRID_Y + row * (TIME_CARD_H + TIME_CARD_GAP);

        bool is_sel = (i == g_selected_tz_idx);
        unsigned int bg_col = is_sel ? UK_SURFACE1 : UK_SURFACE0;
        unsigned int border_col = is_sel ? UK_PEACH : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, card_x, card_y, TIME_CARD_W, TIME_CARD_H, 6, bg_col);
        uk_draw_rounded_rect_outline(&g_win, card_x, card_y, TIME_CARD_W, TIME_CARD_H, 6, border_col);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, card_x + 2, card_y + 2, 3, TIME_CARD_H - 4, 2, UK_PEACH);
        }

        uk_draw_text(&g_win, card_x + 12, card_y + 6, g_tz_settings_list[i].label, is_sel ? UK_TEXT : UK_SUBTEXT0);
        uk_draw_text(&g_win, card_x + 12, card_y + 24, g_tz_settings_list[i].offset_desc, UK_OVERLAY0);

        if (is_sel) {
            uk_draw_badge(&g_win, card_x + TIME_CARD_W - 46, card_y + 12, "Set", UK_SURFACE2, UK_PEACH);
        }
    }
}


void handle_time_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    for (int i = 0; i < 8; i++) {
                        int col = i % 2;
                        int row = i / 2;
                        int card_x = 20 + col * (TIME_CARD_W + TIME_CARD_GAP + 12);
                        int card_y = TIME_GRID_Y + row * (TIME_CARD_H + TIME_CARD_GAP);
                        if (mx >= card_x && mx < card_x + TIME_CARD_W &&
                            my >= card_y && my < card_y + TIME_CARD_H) {
                            apply_timezone(i);
                            draw_settings();
                            break;
                        }
                    }
}
