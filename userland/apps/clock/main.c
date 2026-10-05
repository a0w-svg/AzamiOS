/* ============================================================================
 * AzamiOS Desktop Environment — Clock & World Timekeeper (v2.0)
 * File: userland/apps/clock/main.c
 *
 * Features:
 *  • Tab 1: Local Clock with Analog Face & Digital Clock, Live Proper Date, Timezone
 *  • Tab 2: World Timekeeper with 6 Major International Timezones & Offsets
 *  • Tab 3: Precision Stopwatch & Lap Timer
 *  • 12-Hour / 24-Hour Format Switcher
 *  • Catppuccin Mocha aesthetic with smooth vector hands and cards
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/time.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       580
#define WIN_H       440
#define MAP_ADDR    ((void *)0x6C000000)

static uk_window_t g_win;

/* Tabs */
#define TAB_CLOCK     0
#define TAB_WORLD     1
#define TAB_STOPWATCH 2
static int g_tab = TAB_CLOCK;
static bool g_24h_mode = true;

/* Clock state */
static int g_hours = 0;
static int g_mins  = 0;
static int g_secs  = 0;
static char g_date_str[64];
static char g_zone_str[64];
static char g_offset_str[32];
static char g_clock_status[96];

/* World clocks */
typedef struct {
    const char *city;
    const char *country;
    const char *tz_env;
} world_city_t;

static const world_city_t g_world_cities[6] = {
    { "Warsaw / Central EU", "Poland / EU",    "Europe/Warsaw" },
    { "London",              "United Kingdom", "Europe/London" },
    { "New York",            "United States",  "America/New_York" },
    { "Los Angeles",         "United States",  "America/Los_Angeles" },
    { "Tokyo",               "Japan",          "Asia/Tokyo" },
    { "Sydney",              "Australia",      "Australia/Sydney" }
};

/* Stopwatch state */
static bool g_sw_running = false;
static unsigned long long g_sw_elapsed_ms = 0;
static unsigned long long g_sw_base_ms, g_sw_anchor_ms;

#define MAX_LAPS 8
static unsigned long long g_laps[MAX_LAPS];
static int g_lap_count = 0;

/* Sine & Cosine LUT for 360 degrees (in fixed point 1/1000) */
static const int g_sin_table[60] = {
      0,  104,  207,  309,  406,  500,  587,  669,  743,  809,
    866,  913,  951,  978,  994, 1000,  994,  978,  951,  913,
    866,  809,  743,  669,  587,  500,  406,  309,  207,  104,
      0, -104, -207, -309, -406, -500, -587, -669, -743, -809,
   -866, -913, -951, -978, -994,-1000, -994, -978, -951, -913,
   -866, -809, -743, -669, -587, -500, -406, -309, -207, -104
};

static const int g_cos_table[60] = {
   1000,  994,  978,  951,  913,  866,  809,  743,  669,  587,
    500,  406,  309,  207,  104,    0, -104, -207, -309, -406,
   -500, -587, -669, -743, -809, -866, -913, -951, -978, -994,
  -1000, -994, -978, -951, -913, -866, -809, -743, -669, -587,
   -500, -406, -309, -207, -104,    0,  104,  207,  309,  406,
    500,  587,  669,  743,  809,  866,  913,  951,  978,  994
};

static bool update_system_time(void)
{
    /* Follow Settings even when TZ was inherited at launch. */
    unsetenv("TZ");
    tzset();
    time_t t = time(NULL);
    struct tm tm_info;
    if (!localtime_r(&t, &tm_info)) return false;
    char date[64], offset[32], zone[64];
    strftime(date, sizeof(date), "%A, %B %e, %Y", &tm_info);
    strftime(offset, sizeof(offset), "%z", &tm_info);
    const char *zname = tm_info.tm_zone ? tm_info.tm_zone : "UTC";
    long off = tm_info.tm_gmtoff;
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    snprintf(zone, sizeof(zone), "%s (UTC%c%02d:%02d)", zname, sign,
             (int)(off / 3600), (int)((off % 3600) / 60));
    bool changed = g_hours != tm_info.tm_hour || g_mins != tm_info.tm_min ||
        g_secs != tm_info.tm_sec || strcmp(g_date_str, date) || strcmp(g_zone_str, zone);
    g_hours = tm_info.tm_hour; g_mins = tm_info.tm_min; g_secs = tm_info.tm_sec;
    strcpy(g_date_str, date); strcpy(g_offset_str, offset); strcpy(g_zone_str, zone);
    return changed;
}

static bool world_city_time(int city, time_t now, struct tm *out, char *badge, size_t size)
{
    if (city < 0 || city >= 6 || setenv("TZ", g_world_cities[city].tz_env, 1) < 0) return false;
    tzset();
    bool ok = localtime_r(&now, out) != NULL;
    if (ok) strftime(badge, size, "%Z (%z)", out);
    /* tm_zone points at libc's mutable timezone cache: preserve the label
     * above before restoring the system timezone for the local clock. */
    out->tm_zone = NULL;
    unsetenv("TZ");
    tzset();
    return ok;
}

static bool stopwatch_now(unsigned long long *now)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0 || ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
        snprintf(g_clock_status, sizeof(g_clock_status), "Stopwatch: monotonic clock unavailable.");
        return false;
    }
    *now = (unsigned long long)ts.tv_sec * 1000 + (unsigned long long)ts.tv_nsec / 1000000;
    g_clock_status[0] = '\0';
    return true;
}

static bool stopwatch_update(void)
{
    if (!g_sw_running) return true;
    unsigned long long now;
    if (!stopwatch_now(&now)) return false;
    if (now < g_sw_anchor_ms) {
        snprintf(g_clock_status, sizeof(g_clock_status), "Stopwatch: monotonic clock moved backwards.");
        return false;
    }
    g_sw_elapsed_ms = g_sw_base_ms + now - g_sw_anchor_ms;
    return true;
}

static void stopwatch_toggle(void)
{
    if (g_sw_running) {
        if (!stopwatch_update()) return;
        g_sw_base_ms = g_sw_elapsed_ms;
        g_sw_running = false;
    } else {
        unsigned long long now;
        if (!stopwatch_now(&now)) return;
        g_sw_base_ms = g_sw_elapsed_ms;
        g_sw_anchor_ms = now;
        g_sw_running = true;
    }
}

static void stopwatch_lap(void)
{
    if (g_sw_running && g_lap_count < MAX_LAPS && stopwatch_update())
        g_laps[g_lap_count++] = g_sw_elapsed_ms;
}

static void stopwatch_reset(void)
{
    g_sw_running = false;
    g_sw_elapsed_ms = g_sw_base_ms = g_sw_anchor_ms = 0;
    g_lap_count = 0;
    g_clock_status[0] = '\0';
}

static void draw_analog_clock(int cx, int cy, int radius, int h, int m, int s)
{
    /* Outer bezel */
    uk_fill_circle(&g_win, cx, cy, radius, UK_MANTLE);
    uk_fill_circle(&g_win, cx, cy, radius - 4, UK_BASE);

    /* 12 Hour Ticks */
    for (int i = 0; i < 12; i++) {
        int idx = (i * 5) % 60;
        int sx = cx + (g_sin_table[idx] * (radius - 12)) / 1000;
        int sy = cy - (g_cos_table[idx] * (radius - 12)) / 1000;
        uk_fill_circle(&g_win, sx, sy, (i % 3 == 0) ? 3 : 2, (i % 3 == 0) ? UK_MAUVE : UK_OVERLAY0);
    }

    /* Hour Hand */
    int h_idx = ((h % 12) * 5 + m / 12) % 60;
    int hx = cx + (g_sin_table[h_idx] * (radius * 50 / 100)) / 1000;
    int hy = cy - (g_cos_table[h_idx] * (radius * 50 / 100)) / 1000;
    uk_draw_line_aa(&g_win, cx, cy, hx, hy, UK_MAUVE);

    /* Minute Hand */
    int m_idx = m % 60;
    int mx = cx + (g_sin_table[m_idx] * (radius * 75 / 100)) / 1000;
    int my = cy - (g_cos_table[m_idx] * (radius * 75 / 100)) / 1000;
    uk_draw_line_aa(&g_win, cx, cy, mx, my, UK_BLUE);

    /* Second Hand */
    int s_idx = s % 60;
    int sx = cx + (g_sin_table[s_idx] * (radius * 85 / 100)) / 1000;
    int sy = cy - (g_cos_table[s_idx] * (radius * 85 / 100)) / 1000;
    uk_draw_line_aa(&g_win, cx, cy, sx, sy, UK_RED);

    /* Center pivot cap */
    uk_fill_circle(&g_win, cx, cy, 5, UK_RED);
    uk_fill_circle(&g_win, cx, cy, 2, UK_BASE);
}

static void draw_clock_app(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Top Header Bar ──────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_SAPPHIRE);
    uk_draw_text(&g_win, 16, 7, "AzamiOS Timekeeper", UK_TEXT);
    uk_draw_text(&g_win, 16, 23, "System Time, Timezones & Stopwatch • v2.0", UK_OVERLAY0);

    /* Tab switcher */
    int tab_w = 78;
    uk_draw_button(&g_win, (int)w - 260, 8, tab_w, 26, "Clock", (g_tab == TAB_CLOCK) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, (int)w - 176, 8, tab_w, 26, "World", (g_tab == TAB_WORLD) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_draw_button(&g_win, (int)w - 92,  8, tab_w, 26, "Timer", (g_tab == TAB_STOPWATCH) ? UK_BTN_PRESSED : UK_BTN_NORMAL);
    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    if (g_tab == TAB_CLOCK) {
        /* Left Pane: Analog Clock */
        int clock_cx = 145;
        int clock_cy = 240;
        int clock_r  = 115;
        draw_analog_clock(clock_cx, clock_cy, clock_r, g_hours, g_mins, g_secs);

        /* Right Pane: Digital Readout & Cards */
        int card_x = 295;
        int card_y = 56;
        int card_w = (int)w - card_x - 20;

        /* Digital Clock Glass Card */
        uk_draw_card_glass(&g_win, card_x, card_y, card_w, 145, UK_MANTLE, "DIGITAL TIME");

        char time_str[32];
        int display_h = g_hours;
        if (!g_24h_mode) {
            display_h = g_hours % 12;
            if (display_h == 0) display_h = 12;
        }
        snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d", display_h, g_mins, g_secs);
        uk_draw_text_2x(&g_win, card_x + 24, card_y + 50, time_str, UK_GREEN);

        char ampm[16];
        if (g_24h_mode) {
            snprintf(ampm, sizeof(ampm), "24H");
        } else {
            snprintf(ampm, sizeof(ampm), "%s", (g_hours >= 12) ? "PM" : "AM");
        }
        uk_draw_badge(&g_win, card_x + card_w - 55, card_y + 54, ampm, UK_SURFACE1, UK_MAUVE);

        /* Format Mode Switch Button */
        uk_draw_button(&g_win, card_x + 24, card_y + 102, 110, 24, g_24h_mode ? "Use 12-Hour" : "Use 24-Hour", UK_BTN_NORMAL);

        /* Date & Timezone Glass Card */
        uk_draw_card_glass(&g_win, card_x, card_y + 160, card_w, 195, UK_MANTLE, "CALENDAR & LOCATION");
        uk_draw_text(&g_win, card_x + 18, card_y + 195, "Date:", UK_SUBTEXT0);
        uk_draw_text(&g_win, card_x + 65, card_y + 195, g_date_str, UK_TEXT);

        uk_draw_text(&g_win, card_x + 18, card_y + 230, "Zone:", UK_SUBTEXT0);
        uk_draw_text(&g_win, card_x + 65, card_y + 230, g_zone_str, UK_PEACH);

        uk_draw_text(&g_win, card_x + 18, card_y + 265, "Offset:", UK_SUBTEXT0);
        uk_draw_text(&g_win, card_x + 65, card_y + 265, g_offset_str, UK_TEAL);

        uk_draw_text(&g_win, card_x + 18, card_y + 300, "System:", UK_SUBTEXT0);
        uk_draw_text(&g_win, card_x + 65, card_y + 300, "System realtime clock", UK_LAVENDER);

    } else if (g_tab == TAB_WORLD) {
        /* World Clocks Grid */
        time_t now = time(NULL);

        int cols = 2;
        int card_w = ((int)w - 40 - 15) / cols;
        int card_h = 105;

        for (int i = 0; i < 6; i++) {
            int row = i / cols;
            int col = i % cols;
            int cx = 20 + col * (card_w + 15);
            int cy = 56 + row * (card_h + 12);

            uk_fill_rounded_rect(&g_win, cx, cy, card_w, card_h, 8, UK_MANTLE);
            uk_draw_rounded_rect_outline(&g_win, cx, cy, card_w, card_h, 8, UK_SURFACE1);

            /* Mini analog clock */
            struct tm city_tm;
            char bstr[32];
            if (!world_city_time(i, now, &city_tm, bstr, sizeof(bstr))) {
                uk_draw_text(&g_win, cx + 10, cy + 40, "City time unavailable", UK_RED);
                continue;
            }
            int city_h = city_tm.tm_hour;
            int city_m = city_tm.tm_min;
            int city_s = city_tm.tm_sec;
            draw_analog_clock(cx + 42, cy + 52, 34, city_h, city_m, city_s);

            /* City & Country */
            uk_draw_text(&g_win, cx + 88, cy + 16, g_world_cities[i].city, UK_TEXT);
            uk_draw_text(&g_win, cx + 88, cy + 34, g_world_cities[i].country, UK_OVERLAY0);

            /* Digital time readout */
            char dstr[32];
            int dh = city_h;
            const char *d_ampm = (city_h >= 12) ? "PM" : "AM";
            if (!g_24h_mode) {
                dh = city_h % 12;
                if (dh == 0) dh = 12;
                snprintf(dstr, sizeof(dstr), "%02d:%02d %s", dh, city_m, d_ampm);
            } else {
                snprintf(dstr, sizeof(dstr), "%02d:%02d:%02d", dh, city_m, city_s);
            }
            uk_draw_text(&g_win, cx + 88, cy + 54, dstr, UK_GREEN);

            /* Badge */
            uk_draw_badge(&g_win, cx + 88, cy + 76, bstr, UK_SURFACE0, UK_MAUVE);
        }

    } else {
        /* Stopwatch View */
        int sw_box_y = 56;
        uk_draw_card_glass(&g_win, 20, sw_box_y, (int)w - 40, 140, UK_MANTLE, "PRECISION STOPWATCH");

        unsigned long long ms = g_sw_elapsed_ms;
        unsigned int s = (unsigned int)(ms / 1000);
        unsigned int m = s / 60;
        s %= 60;
        unsigned int f = (unsigned int)((ms % 1000) / 10);

        char sw_buf[32];
        snprintf(sw_buf, sizeof(sw_buf), "%02u:%02u.%02u", m, s, f);
        uk_draw_text_2x(&g_win, (int)w / 2 - 80, sw_box_y + 55, sw_buf, g_sw_running ? UK_YELLOW : UK_TEXT);

        /* Stopwatch Action Buttons */
        int btn_y = sw_box_y + 155;
        uk_draw_button(&g_win, 40,  btn_y, 145, 36, g_sw_running ? "Pause" : "Start", g_sw_running ? UK_BTN_HOVER : UK_BTN_PRESSED);
        uk_draw_button(&g_win, 215, btn_y, 145, 36, "Lap", g_sw_running && g_lap_count < MAX_LAPS ? UK_BTN_NORMAL : UK_BTN_DISABLED);
        uk_draw_button(&g_win, 390, btn_y, 145, 36, "Reset", UK_BTN_NORMAL);

        /* Laps Table Card */
        int lap_y = btn_y + 50;
        uk_draw_card_glass(&g_win, 20, lap_y, (int)w - 40, (int)h - lap_y - 15, UK_MANTLE, "LAP TIMES");

        if (g_lap_count == 0) {
            uk_draw_text(&g_win, 40, lap_y + 40, "No recorded laps. Press [Lap] while running.", UK_OVERLAY0);
        } else {
            for (int i = 0; i < g_lap_count && i < MAX_LAPS; i++) {
                int ly = lap_y + 35 + i * 22;
                char lap_label[32];
                snprintf(lap_label, sizeof(lap_label), "Lap %d:", i + 1);
                uk_draw_text(&g_win, 40, ly, lap_label, UK_SUBTEXT0);

                unsigned long long lms = g_laps[i];
                unsigned int ls = (unsigned int)(lms / 1000);
                unsigned int lm = ls / 60;
                ls %= 60;
                unsigned int lf = (unsigned int)((lms % 1000) / 10);

                char lap_time[32];
                snprintf(lap_time, sizeof(lap_time), "%02u:%02u.%02u", lm, ls, lf);
                uk_draw_text(&g_win, 120, ly, lap_time, UK_TEXT);
            }
        }
    }

    if (g_clock_status[0]) {
        uk_fill_rect(&g_win, 0, (int)h - 24, (int)w, 24, UK_MANTLE);
        uk_draw_text_clip(&g_win, 10, (int)h - 20, g_clock_status, UK_RED, (int)w - 20);
    }
    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    tzset();

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Clock & World Time",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    /* Register autonomous timer: 100ms for stopwatch tick */
    az_set_timer(g_win.client_chan, 100, 0);

    update_system_time();
    draw_clock_app();

    unsigned int prev_btn = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) break;

        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g_win, &msg)) break;
            draw_clock_app();
            continue;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            /* Preference refresh stays at one second. Stopwatch duration
             * comes from elapsed monotonic time, even after delayed IPC ticks. */
            static unsigned int ticks;
            bool changed = false;
            if (++ticks >= 10) {
                ticks = 0;
                changed = update_system_time();
            }
            if (g_sw_running) stopwatch_update();
            if (changed || (g_sw_running && g_tab == TAB_STOPWATCH)) draw_clock_app();
        } else if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btn = msg.mouse.buttons;

            bool lclick = (btn & 1) && !(prev_btn & 1);
            prev_btn = btn;

            if (lclick) {
                /* Tab clicks */
                if (my >= 8 && my <= 34) {
                    if (mx >= (int)g_win.width - 260 && mx < (int)g_win.width - 182) {
                        g_tab = TAB_CLOCK;
                        draw_clock_app();
                        continue;
                    } else if (mx >= (int)g_win.width - 176 && mx < (int)g_win.width - 98) {
                        g_tab = TAB_WORLD;
                        draw_clock_app();
                        continue;
                    } else if (mx >= (int)g_win.width - 92 && mx < (int)g_win.width - 14) {
                        g_tab = TAB_STOPWATCH;
                        draw_clock_app();
                        continue;
                    }
                }

                if (g_tab == TAB_CLOCK) {
                    int card_x = 295;
                    int card_y = 56;
                    /* Toggle 12h/24h button */
                    if (mx >= card_x + 24 && mx <= card_x + 134 && my >= card_y + 102 && my <= card_y + 126) {
                        g_24h_mode = !g_24h_mode;
                        draw_clock_app();
                    }
                } else if (g_tab == TAB_STOPWATCH) {
                    int btn_y = 56 + 155;
                    if (my >= btn_y && my <= btn_y + 36) {
                        if (mx >= 40 && mx <= 185) {
                            /* Start / Pause Toggle */
                            stopwatch_toggle();
                            draw_clock_app();
                        } else if (mx >= 215 && mx <= 360) {
                            /* Lap */
                            stopwatch_lap();
                            draw_clock_app();
                        } else if (mx >= 390 && mx <= 535) {
                            /* Reset */
                            stopwatch_reset();
                            draw_clock_app();
                        }
                    }
                }
            }
        }
    }

    return 0;
}
