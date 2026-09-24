#include "settings.h"


/* ── Audio Tab ─────────────────────────────────────────────────────────────── */
#define AUDIO_SEC1_Y    90
#define AUDIO_DEV_Y     122
#define AUDIO_FMT_Y     170
#define AUDIO_SEC2_Y    224
#define AUDIO_SLIDER_Y  256
#define AUDIO_BTN_Y     298

void draw_audio_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, AUDIO_SEC1_Y, (int)w - 40, "Sound Hardware", UK_GREEN);

    uk_draw_panel(&g_win, px, AUDIO_DEV_Y, (int)w - 40, 40, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, AUDIO_DEV_Y + 5,  "Active Audio Controller", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, AUDIO_DEV_Y + 21, "Intel 82801AA AC97 Audio Device (/dev/dsp)", UK_TEXT);

    uk_draw_panel(&g_win, px, AUDIO_FMT_Y, (int)w - 40, 40, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, AUDIO_FMT_Y + 5,  "Sample Format", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, AUDIO_FMT_Y + 21, "44,100 Hz, 16-bit Stereo PCM (Dual-channel)", UK_TEXT);

    uk_draw_section_header(&g_win, px, AUDIO_SEC2_Y, (int)w - 40, "Master Volume Control", UK_YELLOW);

    /* Volume Slider Track */
    int slider_w = (int)w - 180;
    uk_fill_rounded_rect(&g_win, px, AUDIO_SLIDER_Y + 4, slider_w, 12, 6, UK_SURFACE1);
    int fill_w = (slider_w * g_volume_pct) / 100;
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, px, AUDIO_SLIDER_Y + 4, fill_w, 12, 6, UK_GREEN);
    }
    uk_fill_circle(&g_win, px + fill_w, AUDIO_SLIDER_Y + 10, 9, UK_TEXT);

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_volume_pct);
    uk_draw_text(&g_win, px + slider_w + 16, AUDIO_SLIDER_Y + 2, vol_str, UK_TEXT);

    /* Test Chime button */
    uk_draw_button(&g_win, px, AUDIO_BTN_Y, 130, 28, "Play Chime", UK_BTN_NORMAL);
}


void handle_audio_mouse(int mx, int my)
{
    unsigned int w = g_win.width, h = g_win.height;
    (void)w; (void)h;
    int slider_w = (int)w - 180;
                    if (mx >= 20 && mx <= 20 + slider_w && my >= AUDIO_SLIDER_Y - 6 && my <= AUDIO_SLIDER_Y + 24) {
                        int pct = ((mx - 20) * 100) / slider_w;
                        apply_volume(pct);
                        draw_settings();
                        return;
                    }
                    /* Play chime button */
                    if (mx >= 20 && mx <= 150 && my >= AUDIO_BTN_Y && my <= AUDIO_BTN_Y + 28) {
                        play_test_chime();
                        return;
                    }
}
