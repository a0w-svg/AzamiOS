/* ============================================================================
 * AzamiOS — Settings Panel (v3.0 with Live Theme, Audio & System Controls)
 * File: userland/apps/settings/main.c
 *
 * Features:
 *  • Display Configuration (Resolution, VSync, Compositing toggles)
 *  • Audio Control (Master Volume slider, Intel AC97 test chime)
 *  • Theme Switcher (5 Themes: Mocha, Latte, Nord, Cyberpunk, OLED)
 *  • System Information (Kernel, Hardware, Memory, Storage)
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       580
#define WIN_H       450
#define MAP_ADDR    ((void *)0x69000000)

static uk_window_t g_win;

/* ── Tabs ────────────────────────────────────────────────────────────────────── */
#define NTABS  5
static const char *g_tab_labels[NTABS] = { "Display", "Audio", "Theme", "Time", "System" };
static int g_active_tab = 0;

/* ── Audio state ─────────────────────────────────────────────────────────────── */
static int g_volume_pct = 75; /* 0..100 */

#define SOUND_PCM_WRITE_VOLUME 0x40045004

static void apply_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volume_pct = pct;

    int fd = sys_open("/dev/dsp", 0, 0);
    if (fd >= 0) {
        unsigned int vol = (unsigned int)pct | ((unsigned int)pct << 8);
        syscall3(SYS_ioctl, fd, SOUND_PCM_WRITE_VOLUME, (long)&vol);
        sys_close(fd);
    }
}

static void play_test_chime(void)
{
    int fd = sys_open("/dev/dsp", 0, 0);
    if (fd < 0) return;

    short buffer[1024];
    int notes[3] = { 523, 659, 784 }; /* C5, E5, G5 major triad */

    for (int n = 0; n < 3; n++) {
        int freq = notes[n];
        int samples_per_cycle = 44100 / freq;
        int half = samples_per_cycle / 2;
        int t = 0;

        for (int chunk = 0; chunk < 4; chunk++) {
            for (int i = 0; i < 1024; i++) {
                buffer[i] = ((t % samples_per_cycle) < half) ? 12000 : -12000;
                t++;
            }
            sys_write(fd, buffer, sizeof(buffer));
        }
    }
    sys_close(fd);
}

/* ── Theme Presets (5 Themes) ─────────────────────────────────────────────── */
typedef struct {
    const char *name;
    const char *desc;
    unsigned int bg;
    unsigned int accent;
    unsigned int text;
} theme_entry_t;

static const theme_entry_t g_themes[AZ_THEME_COUNT] = {
    [AZ_THEME_MOCHA]     = { "Catppuccin Mocha", "Dark Pastel",  0xFF1E1E2E, 0xFFCBA6F7, 0xFFCDD6F4 },
    [AZ_THEME_LATTE]     = { "Catppuccin Latte", "Light Minimal", 0xFFEFF1F5, 0xFF8839EF, 0xFF4C4F69 },
    [AZ_THEME_NORD]      = { "Nord Arctic",      "Polar Frost",  0xFF2E3440, 0xFF88C0D0, 0xFFECEFF4 },
    [AZ_THEME_CYBERPUNK] = { "Cyberpunk Neon",   "Neon High-Con",0xFF0D0D18, 0xFF00FFCC, 0xFFF0F6FC },
    [AZ_THEME_OLED]      = { "OLED Pure Dark",   "True Black",   0xFF050505, 0xFF3B82F6, 0xFFFFFFFF },
};
static int g_theme_selected = 0;

static void apply_theme(int theme_id)
{
    if (theme_id < 0 || theme_id >= AZ_THEME_COUNT) return;
    g_theme_selected = theme_id;

    /* Broadcast to Display Server */
    az_wm_msg_t tmsg;
    memset(&tmsg, 0, sizeof(tmsg));
    tmsg.type = AZ_WM_SET_THEME;
    AZ_WM_MSG_THEME(&tmsg)->theme_id = (unsigned int)theme_id;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&tmsg);

    /* Persist to /etc/desktop.conf */
    int fd = sys_open("/etc/desktop.conf", 0x42 /* O_CREAT|O_WRONLY */, 0644);
    if (fd >= 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "[theme]\ntheme_id=%d\nname=%s\nwallpaper=/usr/share/wallpapers/default.raw\n\n[display]\nvsync=1\ncompositing=1\ncursor_aa=1\nfps=60\n\n[panel]\nposition=bottom\nheight=32\nautohide=0\nshow_clock=1\n",
                 theme_id, g_themes[theme_id].name);
        sys_write(fd, buf, strlen(buf));
        sys_close(fd);
    }
    /* Legacy /etc/theme.conf support */
    int lfd = sys_open("/etc/theme.conf", 0x42, 0644);
    if (lfd >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\n", theme_id);
        sys_write(lfd, buf, strlen(buf));
        sys_close(lfd);
    }
}

static void load_desktop_config(void)
{
    int fd = sys_open("/etc/desktop.conf", 0, 0);
    if (fd < 0) fd = sys_open("/etc/theme.conf", 0, 0);
    if (fd >= 0) {
        char buf[256];
        ssize_t n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *tid = strstr(buf, "theme_id=");
            if (tid) {
                int id = atoi(tid + 9);
                if (id >= 0 && id < AZ_THEME_COUNT) g_theme_selected = id;
            } else if (buf[0] >= '0' && buf[0] <= '9') {
                int id = atoi(buf);
                if (id >= 0 && id < AZ_THEME_COUNT) g_theme_selected = id;
            }
        }
    }
}

/* ── Toggle switches ─────────────────────────────────────────────────────────── */
static int g_vsync    = 1;
static int g_composit = 1;
static int g_cursor_aa= 1;

static void draw_toggle(int x, int y, int on, const char *label)
{
    unsigned int track_col = on ? UK_MAUVE : UK_SURFACE1;
    uk_fill_rounded_rect(&g_win, x, y, 40, 20, 10, track_col);
    int knob_x = on ? x + 22 : x + 2;
    uk_fill_circle(&g_win, knob_x + 8, y + 10, 8, UK_TEXT);
    uk_draw_text(&g_win, x + 48, y + 2, label, UK_TEXT);
}

static int hit_toggle(int tx, int ty, int mx, int my)
{
    return (mx >= tx && mx < tx + 240 && my >= ty && my < ty + 24);
}

/* ── Display Tab ───────────────────────────────────────────────────────────── */
#define DISP_SEC1_Y   90
#define DISP_PANEL_Y  122
#define DISP_SEC2_Y   176
#define DISP_TOG1_Y   208
#define DISP_TOG2_Y   244
#define DISP_TOG3_Y   280

static void draw_display_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, DISP_SEC1_Y, (int)w - 40, "Framebuffer Display", UK_BLUE);

    az_fb_info_t fb;
    char res[32];
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
}

/* ── Audio Tab ─────────────────────────────────────────────────────────────── */
#define AUDIO_SEC1_Y    90
#define AUDIO_DEV_Y     122
#define AUDIO_FMT_Y     170
#define AUDIO_SEC2_Y    224
#define AUDIO_SLIDER_Y  256
#define AUDIO_BTN_Y     298

static void draw_audio_tab(void)
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

/* ── Theme Tab ─────────────────────────────────────────────────────────────── */
#define THEME_SEC_Y    90
#define THEME_GRID_Y   122
#define THEME_CARD_W   260
#define THEME_CARD_H   60
#define THEME_CARD_GAP 12

static void draw_theme_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, THEME_SEC_Y, (int)w - 40, "Desktop Themes & Color Palettes", UK_MAUVE);

    for (int i = 0; i < AZ_THEME_COUNT; i++) {
        int col = i % 2;
        int row = i / 2;
        int card_x = px + col * (THEME_CARD_W + THEME_CARD_GAP);
        int card_y = THEME_GRID_Y + row * (THEME_CARD_H + THEME_CARD_GAP);

        bool is_sel = (i == g_theme_selected);
        unsigned int border_col = is_sel ? UK_MAUVE : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, card_x, card_y, THEME_CARD_W, THEME_CARD_H, 8, g_themes[i].bg);
        uk_draw_rounded_rect_outline(&g_win, card_x, card_y, THEME_CARD_W, THEME_CARD_H, 8, border_col);

        if (is_sel) {
            uk_fill_rounded_rect(&g_win, card_x + 2, card_y + 2, 4, THEME_CARD_H - 4, 2, UK_MAUVE);
        }

        /* Swatch dots */
        uk_fill_circle(&g_win, card_x + 22, card_y + 24, 8, g_themes[i].accent);
        uk_fill_circle(&g_win, card_x + 42, card_y + 24, 8, g_themes[i].text);
        uk_fill_circle(&g_win, card_x + 62, card_y + 24, 8, g_themes[i].bg);

        /* Title & Desc */
        uk_draw_text(&g_win, card_x + 82, card_y + 14, g_themes[i].name, is_sel ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, card_x + 82, card_y + 32, g_themes[i].desc, UK_OVERLAY0);

        if (is_sel) {
            uk_draw_badge(&g_win, card_x + THEME_CARD_W - 48, card_y + 18, "Active", UK_SURFACE1, UK_MAUVE);
        }
    }
}

/* ── Time & Date Tab ───────────────────────────────────────────────────────── */
typedef struct {
    const char *label;
    const char *tz_id;
    const char *offset_desc;
} tz_setting_item_t;

static const tz_setting_item_t g_tz_settings_list[8] = {
    { "Universal Time",       "UTC",                 "UTC+00:00 (Standard)" },
    { "London / Dublin",      "Europe/London",       "GMT/BST (UTC+01:00)" },
    { "Warsaw / Central EU",  "Europe/Warsaw",       "CET/CEST (UTC+02:00)" },
    { "Athens / Helsinki",    "Europe/Athens",       "EET/EEST (UTC+03:00)" },
    { "New York / Toronto",   "America/New_York",    "EST/EDT (UTC-04:00)" },
    { "Chicago / Dallas",     "America/Chicago",     "CST/CDT (UTC-05:00)" },
    { "Los Angeles / SF",     "America/Los_Angeles", "PST/PDT (UTC-07:00)" },
    { "Tokyo / Seoul",        "Asia/Tokyo",          "JST/KST (UTC+09:00)" }
};

#include "../shared/sys_config.h"

static int g_selected_tz_idx = 2; /* Default: Europe/Warsaw / Central EU */

static void init_timezone_setting(void)
{
    char buf[64] = "";
    if (az_config_read("timezone", buf, sizeof(buf)) > 0) {
        for (int i = 0; i < 8; i++) {
            if (strcmp(buf, g_tz_settings_list[i].tz_id) == 0 ||
                (strcmp(buf, "Europe/Paris") == 0 && i == 2) ||
                (strcmp(buf, "Europe/Berlin") == 0 && i == 2)) {
                g_selected_tz_idx = i;
                return;
            }
        }
    }
}

static void apply_timezone(int idx)
{
    if (idx < 0 || idx >= 8) return;
    g_selected_tz_idx = idx;

    /* Persist to /hdd/etc/timezone and /etc/timezone */
    const char *tz = g_tz_settings_list[idx].tz_id;
    az_config_write("timezone", tz, strlen(tz));

    /* Update environment and tzset */
    tzset();
}

#define TIME_SEC1_Y    86
#define TIME_PREV_Y    114
#define TIME_SEC2_Y    172
#define TIME_GRID_Y    200
#define TIME_CARD_W    260
#define TIME_CARD_H    44
#define TIME_CARD_GAP   8

static void draw_time_tab(void)
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

/* ── System Tab ────────────────────────────────────────────────────────────── */
#define SYS_SEC_Y  90
#define SYS_GRID_Y 122

static void draw_system_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, SYS_SEC_Y, (int)w - 40, "Kernel & System Architecture", UK_TEAL);

    struct sysinfo si;
    sysinfo(&si);
    unsigned long total_mb = (si.totalram * si.mem_unit) / (1024 * 1024);
    unsigned long free_mb  = (si.freeram * si.mem_unit) / (1024 * 1024);

    char mem_buf[64];
    snprintf(mem_buf, sizeof(mem_buf), "%lu MB Total (%lu MB Free)", total_mb, free_mb);

    static const char *sys_info[][2] = {
        { "Operating System", "AzamiOS v7.0.0 (x86_64 Microkernel)" },
        { "SMP CPU Cores",    "4 Cores (Preemptive CFS Scheduling)" },
        { "Memory Model",     "Buddy PMM + 4-Level VMM (PML4)" },
        { "System Memory",    "" },
        { "Storage System",   "Persistent SATA AHCI (/hdd) + Ext2" },
        { "Window Server",    "azwm Compositor (Zero-Copy SHMEM)" },
        { "Audio Controller", "Intel AC97 PCI (/dev/dsp)" },
    };

    int py = SYS_GRID_Y;
    for (int i = 0; i < 7; i++) {
        uk_draw_panel(&g_win, px, py, (int)w - 40, 24, UK_SURFACE0);
        uk_draw_text(&g_win, px + 10, py + 4, sys_info[i][0], UK_SUBTEXT0);
        if (i == 3) {
            uk_draw_text(&g_win, px + 160, py + 4, mem_buf, UK_GREEN);
        } else {
            uk_draw_text(&g_win, px + 160, py + 4, sys_info[i][1], UK_TEXT);
        }
        py += 28;
    }
}

static void draw_settings(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* Header */
    uk_gradient_h(&g_win, 0, 0, (int)w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_MAUVE);
    uk_draw_text(&g_win, 16, 6,  "AzamiOS Settings", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "System Preferences, Timezones, Themes & Hardware", UK_OVERLAY0);
    uk_hline(&g_win, 0, 44, (int)w, UK_SURFACE1);

    /* Tab bar */
    uk_draw_tab_bar(&g_win, 0, 44, (int)w / NTABS, 36,
                    g_tab_labels, NTABS, g_active_tab);
    uk_hline(&g_win, 0, 80, (int)w, UK_SURFACE1);

    /* Tab content */
    switch (g_active_tab) {
    case 0: draw_display_tab(); break;
    case 1: draw_audio_tab();   break;
    case 2: draw_theme_tab();   break;
    case 3: draw_time_tab();    break;
    case 4: draw_system_tab();  break;
    }

    /* Footer */
    uk_fill_rect(&g_win, 0, (int)h - 38, (int)w, 38, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 38, (int)w, UK_SURFACE1);
    uk_draw_text(&g_win, 16, (int)h - 26, "Settings changes take effect immediately.", UK_OVERLAY0);
    uk_draw_button(&g_win, (int)w - 110, (int)h - 32, 96, 26, "Close", UK_BTN_NORMAL);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    tzset();
    init_timezone_setting();
    load_desktop_config();

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Settings",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    draw_settings();

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = (int)msg.mouse.abs_x;
            int my = (int)msg.mouse.abs_y;
            int lclick = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) != 0;

            if (lclick) {
                /* Tab clicks */
                unsigned int w = g_win.width;
                int tab_w = (int)w / NTABS;
                if (my >= 44 && my < 80) {
                    int t = mx / tab_w;
                    if (t >= 0 && t < NTABS) {
                        g_active_tab = t;
                        draw_settings();
                        continue;
                    }
                }

                /* Display tab toggles */
                if (g_active_tab == 0) {
                    if (hit_toggle(20, DISP_TOG1_Y, mx, my)) { g_vsync    ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, DISP_TOG2_Y, mx, my)) { g_composit ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, DISP_TOG3_Y, mx, my)) { g_cursor_aa^= 1; draw_settings(); continue; }
                }

                /* Audio tab */
                if (g_active_tab == 1) {
                    int slider_w = (int)w - 180;
                    if (mx >= 20 && mx <= 20 + slider_w && my >= AUDIO_SLIDER_Y - 6 && my <= AUDIO_SLIDER_Y + 24) {
                        int pct = ((mx - 20) * 100) / slider_w;
                        apply_volume(pct);
                        draw_settings();
                        continue;
                    }
                    /* Play chime button */
                    if (mx >= 20 && mx <= 150 && my >= AUDIO_BTN_Y && my <= AUDIO_BTN_Y + 28) {
                        play_test_chime();
                        continue;
                    }
                }

                /* Theme tab card clicks */
                if (g_active_tab == 2) {
                    for (int i = 0; i < AZ_THEME_COUNT; i++) {
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

                /* Time & Date tab timezone clicks */
                if (g_active_tab == 3) {
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

                /* Footer Close button */
                unsigned int h = g_win.height;
                if (mx >= (int)w - 110 && mx <= (int)w - 14 &&
                    my >= (int)h - 32 && my <= (int)h - 6) {
                    az_wm_msg_t cmsg;
                    memset(&cmsg, 0, sizeof(cmsg));
                    cmsg.type = AZ_WM_DESTROY_WINDOW;
                    cmsg.wid = g_win.wid;
                    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&cmsg);
                    sys_exit(0);
                }
            }
        }
    }
    sys_exit(0);
}

