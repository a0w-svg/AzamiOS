/* ============================================================================
 * AzamiOS — Settings Panel (v3.5 with Security, Network, Live Theme & Audio)
 * File: userland/apps/settings/main.c
 *
 * Features:
 *  • Display Configuration (Resolution, VSync, Compositing toggles)
 *  • Audio Control (Master Volume slider, Intel AC97 test chime)
 *  • Theme Switcher (5 Themes: Mocha, Latte, Nord, Cyberpunk, OLED)
 *  • Time & Date Configuration (Timezones, Live clock)
 *  • Network Configuration (eth0 live stats, IP, gateway, DNS, packets)
 *  • Security & Kernel Mitigations (Interactive sysctl toggles: dmesg_restrict,
 *    kptr_restrict, mmap_min_addr, yama_ptrace_scope, protected hardlinks/symlinks)
 *  • System Architecture & Memory Inspector
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/time.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../../libc/include/sys/statvfs.h"
#include "../../libc/include/sys/reboot.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/ioctl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#if __has_include(<azami/font.h>)
#include <azami/font.h>
#elif __has_include("../shared/az_font.h")
#include "../shared/az_font.h"
#elif __has_include("../../libc/include/azami/font.h")
#include "../../libc/include/azami/font.h"
#endif
#include "../shared/ui_kit.h"
#include "../shared/sys_config.h"

#define SERVER_CHAN  1
#define WIN_W       720
#define WIN_H       510
#define MAP_ADDR    ((void *)0x69000000)

static uk_window_t g_win;

/* ── Tabs ────────────────────────────────────────────────────────────────────── */
#define NTABS  9
static const char *g_tab_labels[NTABS] = {
    "Display", "Audio", "Theme", "Time", "Network", "Power", "Disks", "Security", "System"
};
static int g_active_tab = 0; /* Standard default: Display tab */

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
    [AZ_THEME_MOCHA]     = { "Catppuccin Mocha", "Dark Pastel",   0xFF1E1E2E, 0xFFCBA6F7, 0xFFCDD6F4 },
    [AZ_THEME_LATTE]     = { "Catppuccin Latte", "Light Minimal", 0xFFEFF1F5, 0xFF8839EF, 0xFF4C4F69 },
    [AZ_THEME_NORD]      = { "Nord Arctic",      "Polar Frost",   0xFF2E3440, 0xFF88C0D0, 0xFFECEFF4 },
    [AZ_THEME_CYBERPUNK] = { "Cyberpunk Neon",   "Neon High-Con", 0xFF0D0D18, 0xFF00FFCC, 0xFFF0F6FC },
    [AZ_THEME_OLED]      = { "OLED Pure Dark",   "True Black",    0xFF050505, 0xFF3B82F6, 0xFFFFFFFF },
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
    return (mx >= tx && mx < tx + 340 && my >= ty && my < ty + 24);
}

static int hit_toggle_wide(int tx, int ty, int mx, int my, int width)
{
    return (mx >= tx && mx < tx + width && my >= ty && my < ty + 24);
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
#define THEME_CARD_W   300
#define THEME_CARD_H   60
#define THEME_CARD_GAP 16

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
            uk_draw_badge(&g_win, card_x + THEME_CARD_W - 54, card_y + 18, "Active", UK_SURFACE1, UK_MAUVE);
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

    const char *tz = g_tz_settings_list[idx].tz_id;
    az_config_write("timezone", tz, strlen(tz));
    tzset();
}

#define TIME_SEC1_Y    86
#define TIME_PREV_Y    114
#define TIME_SEC2_Y    172
#define TIME_GRID_Y    200
#define TIME_CARD_W    300
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

/* ── Network Tab State & Static Configuration ──────────────────────────────── */
static int  g_net_dhcp = 1; /* 1 = DHCP (Automatic), 0 = Static Configuration */
static char g_net_ip[32]      = "10.0.2.15";
static char g_net_netmask[32] = "255.255.255.0";
static char g_net_gateway[32] = "10.0.2.2";
static char g_net_dns[32]     = "10.0.2.3";
static int  g_net_focus = -1; /* -1 = none, 0 = IP, 1 = Subnet, 2 = GW, 3 = DNS */
static char g_net_status_msg[96] = "";
static unsigned int g_net_status_col = UK_GREEN;

static int parse_net_ipv4(const char *s, unsigned char out[4])
{
    unsigned int a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return -1;
    if (a > 255 || b > 255 || c > 255 || d > 255) return -1;
    out[0] = (unsigned char)a;
    out[1] = (unsigned char)b;
    out[2] = (unsigned char)c;
    out[3] = (unsigned char)d;
    return 0;
}

static void apply_static_network(void)
{
    unsigned char ip[4], nm[4], gw[4], dns[4];
    if (parse_net_ipv4(g_net_ip, ip) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid IP format");
        g_net_status_col = UK_RED;
        return;
    }
    if (parse_net_ipv4(g_net_netmask, nm) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid Subnet Mask");
        g_net_status_col = UK_RED;
        return;
    }
    if (parse_net_ipv4(g_net_gateway, gw) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid Gateway");
        g_net_status_col = UK_RED;
        return;
    }
    if (parse_net_ipv4(g_net_dns, dns) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid DNS Server");
        g_net_status_col = UK_RED;
        return;
    }

    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, 0x8916 /* SIOCSIFADDR */, (unsigned long)ip);
        ioctl(fd, 0x891c /* SIOCSIFNETMASK */, (unsigned long)nm);
        ioctl(fd, 0x891e /* SIOCSIFGW */, (unsigned long)gw);
        ioctl(fd, 0x8921 /* SIOCSIFDNS */, (unsigned long)dns);
        int flags = 0x4163;
        ioctl(fd, 0x8914 /* SIOCSIFFLAGS */, (unsigned long)&flags);
        close(fd);
    }

    int cfd = open("/etc/network.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cfd >= 0) {
        char cbuf[512];
        int len = snprintf(cbuf, sizeof(cbuf),
            "[network]\n"
            "interface=eth0\n"
            "dhcp=0\n"
            "ip=%s\n"
            "netmask=%s\n"
            "gateway=%s\n"
            "dns=%s\n",
            g_net_ip, g_net_netmask, g_net_gateway, g_net_dns);
        write(cfd, cbuf, (size_t)len);
        close(cfd);
    }

    int rfd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (rfd >= 0) {
        char rbuf[256];
        int len = snprintf(rbuf, sizeof(rbuf),
            "# Generated by AzamiOS Network Settings\n"
            "nameserver %s\n"
            "nameserver 8.8.8.8\n",
            g_net_dns);
        write(rfd, rbuf, (size_t)len);
        close(rfd);
    }

    g_net_dhcp = 0;
    snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Static IP %s applied & saved to /etc/network.conf", g_net_ip);
    g_net_status_col = UK_GREEN;
}

static void apply_dhcp_network(void)
{
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        ioctl(fd, 0x8990 /* SIOCSIFDHCP */, 0);
        close(fd);
    }

    int cfd = open("/etc/network.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cfd >= 0) {
        char cbuf[512];
        int len = snprintf(cbuf, sizeof(cbuf),
            "[network]\n"
            "interface=eth0\n"
            "dhcp=1\n"
            "ip=%s\n"
            "netmask=%s\n"
            "gateway=%s\n"
            "dns=%s\n",
            g_net_ip, g_net_netmask, g_net_gateway, g_net_dns);
        write(cfd, cbuf, (size_t)len);
        close(cfd);
    }

    g_net_dhcp = 1;
    snprintf(g_net_status_msg, sizeof(g_net_status_msg), "DHCP lease requested on net0");
    g_net_status_col = UK_GREEN;
}

static void init_network_settings(void)
{
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        unsigned char ip[4] = {0}, nm[4] = {0}, gw[4] = {0}, dns[4] = {0};
        if (ioctl(fd, 0x8915 /* SIOCGIFADDR */, (unsigned long)ip) == 0 && (ip[0] != 0 || ip[1] != 0)) {
            snprintf(g_net_ip, sizeof(g_net_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        }
        if (ioctl(fd, 0x891b /* SIOCGIFNETMASK */, (unsigned long)nm) == 0 && nm[0] != 0) {
            snprintf(g_net_netmask, sizeof(g_net_netmask), "%u.%u.%u.%u", nm[0], nm[1], nm[2], nm[3]);
        }
        if (ioctl(fd, 0x891d /* SIOCGIFGW */, (unsigned long)gw) == 0 && (gw[0] != 0 || gw[1] != 0)) {
            snprintf(g_net_gateway, sizeof(g_net_gateway), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
        }
        if (ioctl(fd, 0x891f /* SIOCGIFDNS */, (unsigned long)dns) == 0 && (dns[0] != 0 || dns[1] != 0)) {
            snprintf(g_net_dns, sizeof(g_net_dns), "%u.%u.%u.%u", dns[0], dns[1], dns[2], dns[3]);
        }
        close(fd);
    }

    int cfd = open("/etc/network.conf", O_RDONLY, 0);
    if (cfd >= 0) {
        char cbuf[512];
        ssize_t n = read(cfd, cbuf, sizeof(cbuf) - 1);
        close(cfd);
        if (n > 0) {
            cbuf[n] = '\0';
            char *dhcp_p = strstr(cbuf, "dhcp=");
            if (dhcp_p) g_net_dhcp = (atoi(dhcp_p + 5) != 0);
            char *p = strstr(cbuf, "ip=");
            if (p) sscanf(p + 3, "%31s", g_net_ip);
            p = strstr(cbuf, "netmask=");
            if (p) sscanf(p + 8, "%31s", g_net_netmask);
            p = strstr(cbuf, "gateway=");
            if (p) sscanf(p + 8, "%31s", g_net_gateway);
            p = strstr(cbuf, "dns=");
            if (p) sscanf(p + 4, "%31s", g_net_dns);
        }
    }
}

static void draw_input_box(int x, int y, int w, int h, const char *text, int focused)
{
    unsigned int bg_col = focused ? UK_MANTLE : UK_SURFACE0;
    unsigned int border_col = focused ? UK_MAUVE : UK_SURFACE1;
    uk_fill_rounded_rect(&g_win, x, y, w, h, 4, border_col);
    uk_fill_rounded_rect(&g_win, x + 1, y + 1, w - 2, h - 2, 3, bg_col);

    char disp[48];
    if (focused) {
        snprintf(disp, sizeof(disp), "%s_", text);
    } else {
        snprintf(disp, sizeof(disp), "%s", text);
    }
    uk_draw_text(&g_win, x + 8, y + 4, disp, focused ? UK_TEXT : UK_SUBTEXT1);
}

static void draw_network_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "IPv4 Network Configuration & Adapter", UK_TEAL);

    /* Mode selector pills */
    /* DHCP Pill */
    if (g_net_dhcp == 1) {
        uk_fill_rounded_rect(&g_win, px, 112, 190, 26, 6, UK_TEAL);
        uk_draw_text(&g_win, px + 14, 117, "[*] DHCP (Automatic)", UK_CRUST);
    } else {
        uk_fill_rounded_rect(&g_win, px, 112, 190, 26, 6, UK_SURFACE1);
        uk_fill_rounded_rect(&g_win, px + 1, 113, 188, 24, 5, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 117, "[ ] DHCP (Automatic)", UK_SUBTEXT0);
    }

    /* Static Pill */
    if (g_net_dhcp == 0) {
        uk_fill_rounded_rect(&g_win, px + 205, 112, 220, 26, 6, UK_MAUVE);
        uk_draw_text(&g_win, px + 219, 117, "[*] Static Configuration", UK_CRUST);
    } else {
        uk_fill_rounded_rect(&g_win, px + 205, 112, 220, 26, 6, UK_SURFACE1);
        uk_fill_rounded_rect(&g_win, px + 206, 113, 218, 24, 5, UK_SURFACE0);
        uk_draw_text(&g_win, px + 219, 117, "[ ] Static Configuration", UK_SUBTEXT0);
    }

    /* Telemetry strings */
    char rx_info[64] = "RX: 128 packets (14.2 KB)";
    char tx_info[64] = "TX: 64 packets (8.4 KB)";
    int nfd = open("/proc/net", O_RDONLY, 0);
    if (nfd >= 0) {
        char nbuf[512];
        ssize_t n = read(nfd, nbuf, sizeof(nbuf) - 1);
        close(nfd);
        if (n > 0) {
            nbuf[n] = '\0';
            char *line = strstr(nbuf, "net0");
            if (!line) line = strstr(nbuf, "eth0");
            if (line) {
                unsigned long long rx_b = 0, rx_p = 0, tx_b = 0, tx_p = 0;
                char dev[16];
                if (sscanf(line, "%15s %llu %llu %*u %*u %*u %*u %*u %*u %llu %llu",
                           dev, &rx_b, &rx_p, &tx_b, &tx_p) >= 5) {
                    snprintf(rx_info, sizeof(rx_info), "RX: %llu pkts (%llu KB)", rx_p, rx_b / 1024);
                    snprintf(tx_info, sizeof(tx_info), "TX: %llu pkts (%llu KB)", tx_p, tx_b / 1024);
                }
            }
        }
    }

    if (g_net_dhcp == 1) {
        /* DHCP View */
        uk_draw_panel(&g_win, px, 146, (int)w - 40, 102, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 156, "Adapter:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 156, "Intel 82540EM / virtio-net (PCI 00:02.0)", UK_TEXT);
        uk_draw_badge(&g_win, (int)w - 145, 154, "Connected (DHCP)", UK_SURFACE1, UK_GREEN);

        char ip_line[80];
        snprintf(ip_line, sizeof(ip_line), "%s (net0)", g_net_ip);
        uk_draw_text(&g_win, px + 14, 178, "IP Address:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 178, ip_line, UK_GREEN);

        uk_draw_text(&g_win, px + 14, 200, "Subnet Mask:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 200, g_net_netmask, UK_TEXT);

        char gw_dns[80];
        snprintf(gw_dns, sizeof(gw_dns), "Gateway: %s   •   DNS: %s", g_net_gateway, g_net_dns);
        uk_draw_text(&g_win, px + 14, 222, "Routing:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 100, 222, gw_dns, UK_SUBTEXT1);

        uk_draw_button(&g_win, px, 256, 170, 26, "Renew DHCP Lease", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 185, 256, 180, 26, "Configure Static IP", UK_BTN_NORMAL);

        uk_draw_section_header(&g_win, px, 292, (int)w - 40, "Live Network Statistics (/proc/net)", UK_BLUE);
        uk_draw_panel(&g_win, px, 316, (int)w - 40, 64, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 326, "Traffic Flow:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 120, 326, rx_info, UK_TEXT);
        uk_draw_text(&g_win, px + 360, 326, tx_info, UK_TEXT);
        uk_draw_text(&g_win, px + 14, 350, "Link Quality:", UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 120, 350, "1000 Mbps Full Duplex • 0 drops • 0 errors", UK_GREEN);

        if (g_net_status_msg[0]) {
            uk_draw_text(&g_win, px + 4, 390, g_net_status_msg, g_net_status_col);
        }
    } else {
        /* Static Configuration View */
        uk_draw_panel(&g_win, px, 146, (int)w - 40, 188, UK_SURFACE0);
        uk_draw_text(&g_win, px + 14, 154, "Static IPv4 Parameters", UK_PEACH);
        uk_draw_badge(&g_win, (int)w - 130, 152, "Static Mode", UK_SURFACE1, UK_PEACH);

        /* Row 0: IP Address */
        uk_draw_text(&g_win, px + 14, 178, "IP Address:", UK_SUBTEXT0);
        draw_input_box(px + 130, 174, 200, 24, g_net_ip, g_net_focus == 0);
        uk_draw_button(&g_win, px + 340, 174, 130, 24, "Use 10.0.2.15", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 174, 140, 24, "Use 192.168.1.50", UK_BTN_NORMAL);

        /* Row 1: Subnet Mask */
        uk_draw_text(&g_win, px + 14, 206, "Subnet Mask:", UK_SUBTEXT0);
        draw_input_box(px + 130, 202, 200, 24, g_net_netmask, g_net_focus == 1);
        uk_draw_button(&g_win, px + 340, 202, 160, 24, "/24 (255.255.255.0)", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 510, 202, 110, 24, "/16 Netmask", UK_BTN_NORMAL);

        /* Row 2: Default Gateway */
        uk_draw_text(&g_win, px + 14, 234, "Default Gateway:", UK_SUBTEXT0);
        draw_input_box(px + 130, 230, 200, 24, g_net_gateway, g_net_focus == 2);
        uk_draw_button(&g_win, px + 340, 230, 130, 24, "Use 10.0.2.2", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 230, 140, 24, "Use 192.168.1.1", UK_BTN_NORMAL);

        /* Row 3: Primary DNS */
        uk_draw_text(&g_win, px + 14, 262, "Primary DNS:", UK_SUBTEXT0);
        draw_input_box(px + 130, 258, 200, 24, g_net_dns, g_net_focus == 3);
        uk_draw_button(&g_win, px + 340, 258, 130, 24, "8.8.8.8 (Google)", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 480, 258, 140, 24, "1.1.1.1 (Cloudflare)", UK_BTN_NORMAL);

        uk_draw_text(&g_win, px + 14, 298, "Click field to type. Tab moves next, Enter applies.", UK_SUBTEXT1);

        /* Action Buttons */
        uk_draw_button(&g_win, px, 344, 210, 28, "Apply Static Config", UK_BTN_NORMAL);
        uk_draw_button(&g_win, px + 225, 344, 150, 28, "Revert to DHCP", UK_BTN_NORMAL);

        if (g_net_status_msg[0]) {
            uk_draw_text(&g_win, px + 4, 384, g_net_status_msg, g_net_status_col);
        } else {
            uk_draw_text(&g_win, px + 4, 384, "Static settings take effect immediately on net0 and /etc/network.conf", UK_SUBTEXT0);
        }

        /* Compact stats row */
        char stats_line[128];
        snprintf(stats_line, sizeof(stats_line), "Telemetry: %s  •  %s  •  1000 Mbps", rx_info, tx_info);
        uk_draw_text(&g_win, px + 4, 408, stats_line, UK_BLUE);
    }
}

static int read_proc_val(const char *path, int def_val)
{
    int fd = open(path, 0 /* O_RDONLY */, 0);
    if (fd < 0) return def_val;
    char buf[32];
    memset(buf, 0, sizeof(buf));
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        return atoi(buf);
    }
    return def_val;
}

static void write_proc_val(const char *path, int val)
{
    int fd = open(path, 1 /* O_WRONLY */, 0);
    if (fd >= 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d\n", val);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

/* ── Power & Performance Tab ─────────────────────────────────────────────────── */
static int g_power_profile = 1; /* 0: Performance, 1: Balanced, 2: Power Saver */
static int g_screen_timeout = 15; /* 5, 15, 30, 0 (Never) */
static char g_power_status_msg[64] = "ACPI PIIX4 Power Management active.";

static void load_power_config(void)
{
    int fd = open("/etc/power.conf", 0, 0);
    if (fd >= 0) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "profile=");
            if (p) g_power_profile = atoi(p + 8);
            char *t = strstr(buf, "screen_timeout=");
            if (t) g_screen_timeout = atoi(t + 15);
        }
    }
}

static void save_power_config(void)
{
    int fd = open("/etc/power.conf", 0x42 /* O_CREAT|O_WRONLY */, 0644);
    if (fd >= 0) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "# AzamiOS Power Management Configuration\nprofile=%d\nscreen_timeout=%d\nacpi_pm=1\n",
                 g_power_profile, g_screen_timeout);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

static void apply_power_profile(int profile)
{
    if (profile < 0 || profile > 2) return;
    g_power_profile = profile;
    save_power_config();
    if (profile == 0) {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Performance profile active: Max CPU throughput.");
    } else if (profile == 1) {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Balanced profile active: Dynamic power & thermals.");
    } else {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Power Saver active: Energy conservation enabled.");
    }
}

static void apply_screen_timeout(int mins)
{
    g_screen_timeout = mins;
    save_power_config();
    if (mins > 0) {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Screen blanking timeout set to %d minutes.", mins);
    } else {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Screen blanking timeout disabled (Never).");
    }
}

static void draw_power_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "System Power & Energy Profiles", UK_YELLOW);

    /* 3 Profile Cards */
    const char *pnames[3] = { "Performance", "Balanced", "Power Saver" };
    const char *pdescs[3] = { "Max clock & I/O speed", "Adaptive energy balance", "Max battery conservation" };
    int card_w = ((int)w - 40 - 24) / 3;
    for (int i = 0; i < 3; i++) {
        int cx = px + i * (card_w + 12);
        int cy = 114;
        bool is_sel = (g_power_profile == i);
        unsigned int bg_col = is_sel ? UK_SURFACE1 : UK_SURFACE0;
        unsigned int border_col = is_sel ? UK_YELLOW : UK_SURFACE1;

        uk_fill_rounded_rect(&g_win, cx, cy, card_w, 48, 6, bg_col);
        uk_draw_rounded_rect_outline(&g_win, cx, cy, card_w, 48, 6, border_col);
        if (is_sel) {
            uk_fill_rounded_rect(&g_win, cx + 2, cy + 2, 4, 44, 2, UK_YELLOW);
            uk_draw_badge(&g_win, cx + card_w - 56, cy + 8, "Active", UK_SURFACE0, UK_YELLOW);
        }
        uk_draw_text(&g_win, cx + 12, cy + 8, pnames[i], is_sel ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, cx + 12, cy + 26, pdescs[i], UK_OVERLAY0);
    }

    uk_draw_section_header(&g_win, px, 172, (int)w - 40, "Display Sleep & Inactivity Timeout", UK_BLUE);

    int timeouts[4] = { 5, 15, 30, 0 };
    const char *tlabels[4] = { "5 Minutes", "15 Minutes", "30 Minutes", "Never" };
    int pill_w = ((int)w - 40 - 36) / 4;
    for (int i = 0; i < 4; i++) {
        int tx = px + i * (pill_w + 12);
        int ty = 200;
        bool is_sel = (g_screen_timeout == timeouts[i]);
        unsigned int bg_col = is_sel ? UK_MAUVE : UK_SURFACE0;
        unsigned int fg_col = is_sel ? UK_BASE : UK_TEXT;

        uk_fill_rounded_rect(&g_win, tx, ty, pill_w, 26, 4, bg_col);
        uk_draw_rounded_rect_outline(&g_win, tx, ty, pill_w, 26, 4, is_sel ? UK_MAUVE : UK_SURFACE1);
        int slen = uk_strlen(tlabels[i]);
        uk_draw_text(&g_win, tx + (pill_w - slen * 8) / 2, ty + 5, tlabels[i], fg_col);
    }

    uk_draw_section_header(&g_win, px, 236, (int)w - 40, "ACPI Hardware & Subsystem Telemetry", UK_GREEN);
    uk_draw_panel(&g_win, px, 264, (int)w - 40, 56, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, 270, "ACPI Controller: Intel PIIX4 Power Management Interface (I/O 0xB000)", UK_TEXT);
    uk_draw_text(&g_win, px + 12, 286, "PM Timer Clock : 3.579545 MHz High-Precision 24-bit Counter (Fixed Rate)", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, 302, "System Power   : AC Mains Online (100% Standby Ready, S0/S3/S4/S5)", UK_GREEN);

    uk_draw_section_header(&g_win, px, 330, (int)w - 40, "System Power Actions", UK_PEACH);

    uk_draw_button(&g_win, px, 358, 140, 30, "Sleep / Standby", UK_BTN_NORMAL);
    uk_draw_button(&g_win, px + 152, 358, 140, 30, "Restart System", UK_BTN_NORMAL);
    uk_draw_button(&g_win, px + 304, 358, 140, 30, "Power Off", UK_BTN_NORMAL);

    uk_draw_panel(&g_win, px, 400, (int)w - 40, 26, UK_SURFACE0);
    uk_draw_text(&g_win, px + 10, 405, g_power_status_msg, UK_TEXT);
}

/* ── Disks & Storage Tab ────────────────────────────────────────────────────── */
static char g_disk_status_msg[64] = "All filesystem mounts operating nominally.";

static void clean_temp_files(void)
{
    DIR *d = opendir("/tmp");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char path[512];
            snprintf(path, sizeof(path), "/tmp/%s", de->d_name);
            unlink(path);
        }
        closedir(d);
    }
    sync();
    snprintf(g_disk_status_msg, sizeof(g_disk_status_msg), "Temporary scratch space cleaned and VFS buffers synced.");
}

static void draw_storage_card(int x, int y, int w, int h,
                              const char *title, const char *mount_point, const char *fs_type,
                              const char *path)
{
    uk_draw_panel(&g_win, x, y, w, h, UK_SURFACE0);

    unsigned long total_mb = 0, free_mb = 0, used_mb = 0;
    int pct = 0;

    struct statvfs st;
    if (statvfs(path, &st) == 0 && st.f_blocks > 0) {
        total_mb = (st.f_blocks * st.f_bsize) / (1024 * 1024);
        free_mb  = (st.f_bfree * st.f_bsize) / (1024 * 1024);
        used_mb  = total_mb > free_mb ? (total_mb - free_mb) : 0;
        pct      = total_mb ? (int)((used_mb * 100) / total_mb) : 0;
    } else {
        /* Fallback synthetic metrics */
        if (strcmp(path, "/hdd") == 0) {
            total_mb = 1024; used_mb = 128; free_mb = 896; pct = 12;
        } else if (strcmp(path, "/tmp") == 0) {
            total_mb = 128; used_mb = 8; free_mb = 120; pct = 6;
        } else {
            total_mb = 512; used_mb = 96; free_mb = 416; pct = 18;
        }
    }

    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s (%s)", title, mount_point);
    uk_draw_text(&g_win, x + 12, y + 8, title_buf, UK_TEXT);

    char type_buf[64];
    snprintf(type_buf, sizeof(type_buf), "Type: %s", fs_type);
    uk_draw_text(&g_win, x + w - 160, y + 8, type_buf, UK_SUBTEXT0);

    char stat_buf[128];
    snprintf(stat_buf, sizeof(stat_buf), "%lu MB Used of %lu MB  •  %lu MB Free (%d%% full)",
             used_mb, total_mb, free_mb, pct);
    uk_draw_text(&g_win, x + 12, y + 26, stat_buf, UK_SUBTEXT1);

    /* Progress bar */
    int bar_x = x + 12;
    int bar_y = y + 44;
    int bar_w = w - 24;
    int bar_h = 10;
    uk_fill_rounded_rect(&g_win, bar_x, bar_y, bar_w, bar_h, 5, UK_SURFACE1);

    int fill_w = (bar_w * pct) / 100;
    if (fill_w < 4 && pct > 0) fill_w = 4;
    if (fill_w > bar_w) fill_w = bar_w;
    unsigned int bar_col = (pct >= 90) ? UK_RED : (pct >= 75) ? UK_YELLOW : UK_TEAL;
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, bar_x, bar_y, fill_w, bar_h, 5, bar_col);
    }
}

static void draw_disks_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, 86, (int)w - 40, "Storage Partitions & Mounted Filesystems", UK_TEAL);

    draw_storage_card(px, 114, (int)w - 40, 62, "System Volume", "/", "Ext2 File System", "/");
    draw_storage_card(px, 186, (int)w - 40, 62, "Secondary Storage", "/hdd", "SATA AHCI Ext2", "/hdd");
    draw_storage_card(px, 258, (int)w - 40, 62, "RAM Scratchpad", "/tmp", "tmpfs Volatile RAM", "/tmp");

    uk_draw_section_header(&g_win, px, 330, (int)w - 40, "Storage Maintenance & Cache Flush", UK_SAPPHIRE);

    uk_draw_button(&g_win, px, 358, 190, 30, "Clean Temporary Files", UK_BTN_NORMAL);
    uk_draw_text(&g_win, px + 205, 365, "Purges /tmp scratch files and synchronizes VFS block cache.", UK_SUBTEXT0);

    uk_draw_panel(&g_win, px, 400, (int)w - 40, 26, UK_SURFACE0);
    uk_draw_text(&g_win, px + 10, 405, g_disk_status_msg, UK_TEXT);
}

/* ── Security & Auto-Accept Tab ────────────────────────────────────────────── */
static int g_sec_dmesg = 1;
static int g_sec_kptr  = 1;
static int g_sec_mmap  = 1;
static int g_sec_yama  = 1;
static int g_sec_hlinks= 1;
static int g_sec_slinks= 1;

/* Auto-Accept & Unattended Policies */
static int g_sec_auto_ipc   = 1; /* Auto-Accept SCM_RIGHTS IPC transfers */
static int g_sec_auto_admin = 1; /* Auto-Approve Console Admin Escalations */
static int g_sec_auto_dhcp  = 1; /* Auto-Accept Network DHCP Renewals */
static int g_sec_auto_trace = 1; /* Auto-Accept Debug & Tracing Telemetry */

static void load_security_config(void)
{
    int fd = open("/etc/security.conf", 0, 0);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p1 = strstr(buf, "ipc_autoaccept=");
            if (p1) g_sec_auto_ipc = atoi(p1 + 15);
            char *p2 = strstr(buf, "admin_autoaccept=");
            if (p2) g_sec_auto_admin = atoi(p2 + 17);
            char *p3 = strstr(buf, "dhcp_autoaccept=");
            if (p3) g_sec_auto_dhcp = atoi(p3 + 16);
            char *p4 = strstr(buf, "trace_autoaccept=");
            if (p4) g_sec_auto_trace = atoi(p4 + 17);
        }
    }
}

static void save_security_config(void)
{
    int fd = open("/etc/security.conf", 0x42 /* O_CREAT|O_WRONLY */, 0644);
    if (fd >= 0) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "# AzamiOS Security & Auto-Accept Configuration\n"
                 "ipc_autoaccept=%d\n"
                 "admin_autoaccept=%d\n"
                 "dhcp_autoaccept=%d\n"
                 "trace_autoaccept=%d\n",
                 g_sec_auto_ipc, g_sec_auto_admin, g_sec_auto_dhcp, g_sec_auto_trace);
        write(fd, buf, strlen(buf));
        close(fd);
    }
}

static void init_security_settings(void)
{
    g_sec_dmesg  = read_proc_val("/proc/sys/kernel/dmesg_restrict", 1) > 0 ? 1 : 0;
    g_sec_kptr   = read_proc_val("/proc/sys/kernel/kptr_restrict", 1) > 0 ? 1 : 0;
    g_sec_mmap   = read_proc_val("/proc/sys/kernel/mmap_min_addr", 65536) > 0 ? 1 : 0;
    g_sec_yama   = read_proc_val("/proc/sys/kernel/yama/ptrace_scope", 1) > 0 ? 1 : 0;
    g_sec_hlinks = read_proc_val("/proc/sys/fs/protected_hardlinks", 1) > 0 ? 1 : 0;
    g_sec_slinks = read_proc_val("/proc/sys/fs/protected_symlinks", 1) > 0 ? 1 : 0;
    load_security_config();
}

#define SEC_HEADER_Y   86
#define SEC_LCOL_X     20
#define SEC_RCOL_X     370
#define SEC_ROW1_Y    114
#define SEC_ROW2_Y    144
#define SEC_ROW3_Y    174

#define SEC_AUTO_HDR_Y 212
#define SEC_AUTO1_Y    240
#define SEC_AUTO2_Y    268
#define SEC_AUTO3_Y    296
#define SEC_AUTO4_Y    324
#define SEC_FOOTER_Y   362

static void draw_security_tab(void)
{
    int px = 20;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, SEC_HEADER_Y, (int)w - 40, "Kernel Runtime Hardening & Sysctl Mitigations", UK_RED);

    draw_toggle(SEC_LCOL_X, SEC_ROW1_Y, g_sec_dmesg,  "dmesg_restrict (Ring buffer guard)");
    draw_toggle(SEC_LCOL_X, SEC_ROW2_Y, g_sec_kptr,   "kptr_restrict (Mask kernel ptrs)");
    draw_toggle(SEC_LCOL_X, SEC_ROW3_Y, g_sec_mmap,   "mmap_min_addr (NULL deref guard)");

    draw_toggle(SEC_RCOL_X, SEC_ROW1_Y, g_sec_yama,   "yama.ptrace_scope (YAMA security)");
    draw_toggle(SEC_RCOL_X, SEC_ROW2_Y, g_sec_hlinks, "protected_hardlinks (Link guard)");
    draw_toggle(SEC_RCOL_X, SEC_ROW3_Y, g_sec_slinks, "protected_symlinks (Traversal guard)");

    uk_draw_section_header(&g_win, px, SEC_AUTO_HDR_Y, (int)w - 40, "Auto-Accept & Unattended Policies", UK_MAUVE);

    draw_toggle(px, SEC_AUTO1_Y, g_sec_auto_ipc,   "Auto-Accept IPC Capability Transfers (SCM_RIGHTS & Channels)");
    draw_toggle(px, SEC_AUTO2_Y, g_sec_auto_admin, "Auto-Approve Desktop Administrative Tasks (Unattended Admin)");
    draw_toggle(px, SEC_AUTO3_Y, g_sec_auto_dhcp,  "Auto-Accept Network DHCP Lease Transitions (Zero Disruption)");
    draw_toggle(px, SEC_AUTO4_Y, g_sec_auto_trace, "Auto-Accept System Tracing & Telemetry (ptrace / ktrace hooks)");

    uk_draw_panel(&g_win, px, SEC_FOOTER_Y, (int)w - 40, 48, UK_SURFACE0);
    int all_auto = g_sec_auto_ipc && g_sec_auto_admin && g_sec_auto_dhcp && g_sec_auto_trace;
    if (all_auto) {
        uk_draw_badge(&g_win, px + 10, SEC_FOOTER_Y + 8, "Auto-Accept: ACTIVE", UK_SURFACE1, UK_GREEN);
        uk_draw_text(&g_win, px + 175, SEC_FOOTER_Y + 10, "Automated execution enabled for IPC, admin, DHCP, and telemetry.", UK_TEXT);
    } else {
        uk_draw_badge(&g_win, px + 10, SEC_FOOTER_Y + 8, "Auto-Accept: CUSTOM", UK_SURFACE1, UK_YELLOW);
        uk_draw_text(&g_win, px + 175, SEC_FOOTER_Y + 10, "Custom policy active. Selected operations prompt for confirmation.", UK_SUBTEXT0);
    }
    uk_draw_text(&g_win, px + 10, SEC_FOOTER_Y + 28, "Security policies persist to /etc/security.conf and apply immediately.", UK_OVERLAY0);
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
        { "Power Management", "Intel PIIX4 ACPI PM (I/O 0xB000)" },
        { "Security Engine",  "LSM + YAMA + Auto-Accept Policy" },
    };

    int py = SYS_GRID_Y;
    for (int i = 0; i < 9; i++) {
        uk_draw_panel(&g_win, px, py, (int)w - 40, 24, UK_SURFACE0);
        uk_draw_text(&g_win, px + 10, py + 4, sys_info[i][0], UK_SUBTEXT0);
        if (i == 3) {
            uk_draw_text(&g_win, px + 180, py + 4, mem_buf, UK_GREEN);
        } else {
            uk_draw_text(&g_win, px + 180, py + 4, sys_info[i][1], UK_TEXT);
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
    uk_draw_text(&g_win, 16, 24, "System Preferences, Timezones, Themes, Security & Hardware", UK_OVERLAY0);
    uk_hline(&g_win, 0, 44, (int)w, UK_SURFACE1);

    /* Tab bar */
    int tab_w = ((int)w - 20) / NTABS;
    uk_draw_tab_bar(&g_win, 10, 44, tab_w, 36,
                    g_tab_labels, NTABS, g_active_tab);
    uk_hline(&g_win, 0, 80, (int)w, UK_SURFACE1);

    /* Tab content */
    switch (g_active_tab) {
    case 0: draw_display_tab();  break;
    case 1: draw_audio_tab();    break;
    case 2: draw_theme_tab();    break;
    case 3: draw_time_tab();     break;
    case 4: draw_network_tab();  break;
    case 5: draw_power_tab();    break;
    case 6: draw_disks_tab();    break;
    case 7: draw_security_tab(); break;
    case 8: draw_system_tab();   break;
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
    init_security_settings();
    init_network_settings();
    load_desktop_config();
    load_power_config();

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

        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g_win, &msg)) break;
            draw_settings();
            continue;
        }

        if (msg.type == AZ_WM_KEY_EVENT) {
            if (!msg.key.pressed) continue;

            if (g_active_tab == 4 && g_net_dhcp == 0 && g_net_focus >= 0 && g_net_focus <= 3) {
                char *target = (g_net_focus == 0) ? g_net_ip
                             : (g_net_focus == 1) ? g_net_netmask
                             : (g_net_focus == 2) ? g_net_gateway
                             : g_net_dns;
                size_t max_len = 31;
                size_t cur_len = strlen(target);

                if (msg.key.keycode == 0x08) { /* Backspace */
                    if (cur_len > 0) {
                        target[cur_len - 1] = '\0';
                        draw_settings();
                        continue;
                    }
                } else if (msg.key.keycode == '\t') { /* Tab key */
                    g_net_focus = (g_net_focus + 1) % 4;
                    draw_settings();
                    continue;
                } else if (msg.key.keycode == '\n' || msg.key.keycode == '\r') { /* Enter */
                    apply_static_network();
                    draw_settings();
                    continue;
                } else if ((msg.key.keycode >= '0' && msg.key.keycode <= '9') || msg.key.keycode == '.') {
                    if (cur_len < max_len - 1) {
                        target[cur_len] = (char)msg.key.keycode;
                        target[cur_len + 1] = '\0';
                        draw_settings();
                        continue;
                    }
                }
            }
            continue;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = (int)msg.mouse.abs_x;
            int my = (int)msg.mouse.abs_y;
            int lclick = (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT) != 0;

            if (lclick) {
                /* Tab clicks */
                unsigned int w = g_win.width;
                int tab_w = ((int)w - 20) / NTABS;
                if (my >= 44 && my < 80) {
                    int t = (mx - 10) / (tab_w + 2);
                    if (t >= 0 && t < NTABS) {
                        g_active_tab = t;
                        draw_settings();
                        continue;
                    }
                }

                /* Display tab toggles and font selection */
                if (g_active_tab == 0) {
                    if (hit_toggle(20, DISP_TOG1_Y, mx, my)) { g_vsync    ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, DISP_TOG2_Y, mx, my)) { g_composit ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, DISP_TOG3_Y, mx, my)) { g_cursor_aa^= 1; draw_settings(); continue; }

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

                /* Network tab */
                if (g_active_tab == 4) {
                    /* Mode toggle pills */
                    if (mx >= 20 && mx <= 210 && my >= 112 && my <= 138) {
                        apply_dhcp_network();
                        draw_settings();
                        continue;
                    }
                    if (mx >= 225 && mx <= 445 && my >= 112 && my <= 138) {
                        g_net_dhcp = 0;
                        draw_settings();
                        continue;
                    }

                    if (g_net_dhcp == 1) {
                        /* "Renew DHCP Lease" button */
                        if (mx >= 20 && mx <= 190 && my >= 256 && my <= 282) {
                            apply_dhcp_network();
                            draw_settings();
                            continue;
                        }
                        /* "Configure Static IP" button */
                        if (mx >= 205 && mx <= 385 && my >= 256 && my <= 282) {
                            g_net_dhcp = 0;
                            draw_settings();
                            continue;
                        }
                    } else {
                        /* Row 0: IP field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 174 && my <= 198) {
                            g_net_focus = 0;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 174 && my <= 198) {
                            snprintf(g_net_ip, sizeof(g_net_ip), "10.0.2.15");
                            g_net_focus = 0;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 174 && my <= 198) {
                            snprintf(g_net_ip, sizeof(g_net_ip), "192.168.1.50");
                            g_net_focus = 0;
                            draw_settings();
                            continue;
                        }

                        /* Row 1: Subnet mask field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 202 && my <= 226) {
                            g_net_focus = 1;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 360 && mx <= 520 && my >= 202 && my <= 226) {
                            snprintf(g_net_netmask, sizeof(g_net_netmask), "255.255.255.0");
                            g_net_focus = 1;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 530 && mx <= 640 && my >= 202 && my <= 226) {
                            snprintf(g_net_netmask, sizeof(g_net_netmask), "255.255.0.0");
                            g_net_focus = 1;
                            draw_settings();
                            continue;
                        }

                        /* Row 2: Default gateway field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 230 && my <= 254) {
                            g_net_focus = 2;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 230 && my <= 254) {
                            snprintf(g_net_gateway, sizeof(g_net_gateway), "10.0.2.2");
                            g_net_focus = 2;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 230 && my <= 254) {
                            snprintf(g_net_gateway, sizeof(g_net_gateway), "192.168.1.1");
                            g_net_focus = 2;
                            draw_settings();
                            continue;
                        }

                        /* Row 3: Primary DNS field and presets */
                        if (mx >= 150 && mx <= 350 && my >= 258 && my <= 282) {
                            g_net_focus = 3;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 360 && mx <= 490 && my >= 258 && my <= 282) {
                            snprintf(g_net_dns, sizeof(g_net_dns), "8.8.8.8");
                            g_net_focus = 3;
                            draw_settings();
                            continue;
                        }
                        if (mx >= 500 && mx <= 640 && my >= 258 && my <= 282) {
                            snprintf(g_net_dns, sizeof(g_net_dns), "1.1.1.1");
                            g_net_focus = 3;
                            draw_settings();
                            continue;
                        }

                        /* "Apply Static Config" button */
                        if (mx >= 20 && mx <= 230 && my >= 344 && my <= 372) {
                            apply_static_network();
                            draw_settings();
                            continue;
                        }
                        /* "Revert to DHCP" button */
                        if (mx >= 245 && mx <= 395 && my >= 344 && my <= 372) {
                            apply_dhcp_network();
                            draw_settings();
                            continue;
                        }
                    }
                }

                /* Power tab */
                if (g_active_tab == 5) {
                    /* Profile cards (y: 114..162) */
                    int card_w = ((int)w - 40 - 24) / 3;
                    if (my >= 114 && my <= 162) {
                        for (int i = 0; i < 3; i++) {
                            int cx = 20 + i * (card_w + 12);
                            if (mx >= cx && mx <= cx + card_w) {
                                apply_power_profile(i);
                                draw_settings();
                                break;
                            }
                        }
                        continue;
                    }
                    /* Timeout pills (y: 200..226) */
                    int timeouts[4] = { 5, 15, 30, 0 };
                    int pill_w = ((int)w - 40 - 36) / 4;
                    if (my >= 200 && my <= 226) {
                        for (int i = 0; i < 4; i++) {
                            int tx = 20 + i * (pill_w + 12);
                            if (mx >= tx && mx <= tx + pill_w) {
                                apply_screen_timeout(timeouts[i]);
                                draw_settings();
                                break;
                            }
                        }
                        continue;
                    }
                    /* Power buttons (y: 358..388) */
                    if (my >= 358 && my <= 388) {
                        if (mx >= 20 && mx <= 160) {
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Entering ACPI S3 Standby state...");
                            draw_settings();
                            continue;
                        }
                        if (mx >= 172 && mx <= 312) {
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Initiating system reboot...");
                            draw_settings();
                            reboot(RB_AUTOBOOT);
                            continue;
                        }
                        if (mx >= 324 && mx <= 464) {
                            snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Initiating ACPI poweroff...");
                            draw_settings();
                            reboot(RB_POWER_OFF);
                            continue;
                        }
                    }
                }

                /* Disks tab */
                if (g_active_tab == 6) {
                    /* Clean Temporary Files button (y: 358..388, x: 20..210) */
                    if (mx >= 20 && mx <= 210 && my >= 358 && my <= 388) {
                        clean_temp_files();
                        draw_settings();
                        continue;
                    }
                }

                /* Security & Auto-Accept tab */
                if (g_active_tab == 7) {
                    /* Left col sysctl */
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW1_Y, mx, my)) {
                        g_sec_dmesg ^= 1;
                        write_proc_val("/proc/sys/kernel/dmesg_restrict", g_sec_dmesg);
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW2_Y, mx, my)) {
                        g_sec_kptr ^= 1;
                        write_proc_val("/proc/sys/kernel/kptr_restrict", g_sec_kptr);
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle(SEC_LCOL_X, SEC_ROW3_Y, mx, my)) {
                        g_sec_mmap ^= 1;
                        write_proc_val("/proc/sys/kernel/mmap_min_addr", g_sec_mmap ? 65536 : 0);
                        draw_settings();
                        continue;
                    }
                    /* Right col sysctl */
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW1_Y, mx, my)) {
                        g_sec_yama ^= 1;
                        write_proc_val("/proc/sys/kernel/yama/ptrace_scope", g_sec_yama);
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW2_Y, mx, my)) {
                        g_sec_hlinks ^= 1;
                        write_proc_val("/proc/sys/fs/protected_hardlinks", g_sec_hlinks);
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle(SEC_RCOL_X, SEC_ROW3_Y, mx, my)) {
                        g_sec_slinks ^= 1;
                        write_proc_val("/proc/sys/fs/protected_symlinks", g_sec_slinks);
                        draw_settings();
                        continue;
                    }
                    /* Auto-Accept toggles */
                    if (hit_toggle_wide(20, SEC_AUTO1_Y, mx, my, 650)) {
                        g_sec_auto_ipc ^= 1;
                        save_security_config();
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO2_Y, mx, my, 650)) {
                        g_sec_auto_admin ^= 1;
                        save_security_config();
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO3_Y, mx, my, 650)) {
                        g_sec_auto_dhcp ^= 1;
                        save_security_config();
                        draw_settings();
                        continue;
                    }
                    if (hit_toggle_wide(20, SEC_AUTO4_Y, mx, my, 650)) {
                        g_sec_auto_trace ^= 1;
                        save_security_config();
                        draw_settings();
                        continue;
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
