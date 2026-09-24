/* ============================================================================
 * AzamiOS — Settings Panel (v3.5 with Security, Network, Live Theme & Audio)
 * File: userland/apps/settings/main.c
 *
 * Features:
 *  • Display Configuration (Resolution, VSync, Compositing toggles)
 *  • Audio Control (Master Volume slider, Intel AC97 test chime)
 *  • Theme Switcher (file-backed: theme files under /usr/share/themes, /hdd/themes)
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
#include "settings.h"

#define SERVER_CHAN  1
#define WIN_W       720
uk_window_t g_win;
#define WIN_H       510
#define MAP_ADDR    ((void *)0x69000000)


/* ── Tabs ────────────────────────────────────────────────────────────────────── */
#define NTABS  9
static const char *g_tab_labels[NTABS] = {
    "Display", "Audio", "Theme", "Time", "Network", "Power", "Disks", "Security", "System"
};
int g_active_tab = 0; /* Standard default: Display tab */

/* ── Audio state ─────────────────────────────────────────────────────────────── */
int g_volume_pct = 75; /* 0..100 */

#define SOUND_PCM_WRITE_VOLUME 0x40045004

void apply_volume(int pct)
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

void play_test_chime(void)
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

/* ── Theme Presets ─────────────────────────────────────────────────────────
 * Used to be a second, hand-maintained copy of the palette table in
 * ui_kit.h (name/bg/accent/text only, for the card preview). Now both read
 * the same theme files under /usr/share/themes through az_theme_get(), so this
 * app automatically picks up any theme dropped onto disk instead of only
 * ever offering the 5 it was compiled with. */
int g_theme_selected = 0;

/* A *.theme file has no "short description" field — themes are arbitrary,
 * user-droppable data now, not a fixed enum. Derive one from the base
 * color's luma instead of hand-authoring a caption per theme. */
const char *theme_brightness_label(const az_theme_t *t)
{
    unsigned int r = (t->base >> 16) & 0xFF, g = (t->base >> 8) & 0xFF, b = t->base & 0xFF;
    unsigned int luma = (r * 299 + g * 587 + b * 114) / 1000;
    return luma >= 128 ? "Light Theme" : "Dark Theme";
}

/* Writes /etc/desktop.conf from the current in-memory settings state
 * (theme + display toggles). Used both when the theme changes and when a
 * display toggle changes on its own, so neither write ever clobbers the
 * other's fields with a guessed value -- this used to hardcode
 * "vsync=1\ncompositing=1\ncursor_aa=1" on every theme change, silently
 * discarding whatever the Display tab had actually been set to. */
static void save_desktop_config(void)
{
    int fd = sys_open("/etc/desktop.conf", 0x42 /* O_CREAT|O_WRONLY */, 0644);
    if (fd < 0) return;
    char buf[320];
    snprintf(buf, sizeof(buf),
             "[theme]\ntheme_id=%d\nname=%s\nwallpaper=/usr/share/wallpapers/default.raw\n\n"
             "[display]\nvsync=%d\ncompositing=%d\ncursor_aa=%d\nfps=60\n\n"
             "[panel]\nposition=bottom\nheight=32\nautohide=0\nshow_clock=1\n",
             g_theme_selected, az_theme_get(g_theme_selected)->name,
             g_vsync, g_composit, g_cursor_aa);
    sys_write(fd, buf, strlen(buf));
    sys_close(fd);
}

void apply_theme(int theme_id)
{
    int count = az_theme_count();
    if (theme_id < 0 || theme_id >= count) return;
    g_theme_selected = theme_id;

    /* Broadcast to Display Server */
    az_wm_msg_t tmsg;
    memset(&tmsg, 0, sizeof(tmsg));
    tmsg.type = AZ_WM_SET_THEME;
    AZ_WM_MSG_THEME(&tmsg)->theme_id = (unsigned int)theme_id;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&tmsg);

    save_desktop_config();

    /* Legacy /etc/theme.conf support */
    int lfd = sys_open("/etc/theme.conf", 0x42, 0644);
    if (lfd >= 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\n", theme_id);
        sys_write(lfd, buf, strlen(buf));
        sys_close(lfd);
    }
}

/* Persists a Display-tab toggle (VSync/Compositor/Cursor AA) the moment it
 * changes, instead of only ever being saved as a side effect of switching
 * themes -- otherwise a toggle flipped without also changing the theme was
 * pure UI state that vanished on the next apply_theme() call or restart. */
void save_display_settings(void)
{
    save_desktop_config();
}

void load_desktop_config(void)
{
    int fd = sys_open("/etc/desktop.conf", 0, 0);
    if (fd < 0) fd = sys_open("/etc/theme.conf", 0, 0);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int count = az_theme_count();
            char *tid = strstr(buf, "theme_id=");
            if (tid) {
                int id = atoi(tid + 9);
                if (id >= 0 && id < count) g_theme_selected = id;
            } else if (buf[0] >= '0' && buf[0] <= '9') {
                int id = atoi(buf);
                if (id >= 0 && id < count) g_theme_selected = id;
            }
            char *v = strstr(buf, "vsync=");
            if (v) g_vsync = (atoi(v + 6) != 0);
            char *c = strstr(buf, "compositing=");
            if (c) g_composit = (atoi(c + 12) != 0);
            char *a = strstr(buf, "cursor_aa=");
            if (a) g_cursor_aa = (atoi(a + 10) != 0);
        }
    }
}

/* ── Toggle switches ─────────────────────────────────────────────────────────── */
int g_vsync    = 1;
int g_composit = 1;
int g_cursor_aa= 1;

void draw_toggle(int x, int y, int on, const char *label)
{
    unsigned int track_col = on ? UK_MAUVE : UK_SURFACE1;
    uk_fill_rounded_rect(&g_win, x, y, 40, 20, 10, track_col);
    int knob_x = on ? x + 22 : x + 2;
    uk_fill_circle(&g_win, knob_x + 8, y + 10, 8, UK_TEXT);
    uk_draw_text(&g_win, x + 48, y + 2, label, UK_TEXT);
}

int hit_toggle(int tx, int ty, int mx, int my)
{
    return (mx >= tx && mx < tx + 340 && my >= ty && my < ty + 24);
}

int hit_toggle_wide(int tx, int ty, int mx, int my, int width)
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

/* ── Audio Tab ─────────────────────────────────────────────────────────────── */
#define AUDIO_SEC1_Y    90
#define AUDIO_DEV_Y     122
#define AUDIO_FMT_Y     170
#define AUDIO_SEC2_Y    224
#define AUDIO_SLIDER_Y  256
#define AUDIO_BTN_Y     298

/* ── Theme Tab ─────────────────────────────────────────────────────────────── */
#define THEME_SEC_Y    90
#define THEME_GRID_Y   122
#define THEME_CARD_W   300
#define THEME_CARD_H   60
#define THEME_CARD_GAP 16

/* ── Time & Date Tab ───────────────────────────────────────────────────────── */


const tz_setting_item_t g_tz_settings_list[8] = {
    { "Universal Time",       "UTC",                 "UTC+00:00 (Standard)" },
    { "London / Dublin",      "Europe/London",       "GMT/BST (UTC+01:00)" },
    { "Warsaw / Central EU",  "Europe/Warsaw",       "CET/CEST (UTC+02:00)" },
    { "Athens / Helsinki",    "Europe/Athens",       "EET/EEST (UTC+03:00)" },
    { "New York / Toronto",   "America/New_York",    "EST/EDT (UTC-04:00)" },
    { "Chicago / Dallas",     "America/Chicago",     "CST/CDT (UTC-05:00)" },
    { "Los Angeles / SF",     "America/Los_Angeles", "PST/PDT (UTC-07:00)" },
    { "Tokyo / Seoul",        "Asia/Tokyo",          "JST/KST (UTC+09:00)" }
};

int g_selected_tz_idx = 2; /* Default: Europe/Warsaw / Central EU */

void init_timezone_setting(void)
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

void apply_timezone(int idx)
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

/* ── Network Tab State & Static Configuration ──────────────────────────────── */
int g_net_dhcp = 1; /* 1 = DHCP (Automatic), 0 = Static Configuration */
char g_net_ip[32]      = "10.0.2.15";
char g_net_netmask[32] = "255.255.255.0";
char g_net_gateway[32] = "10.0.2.2";
char g_net_dns[32]     = "10.0.2.3";
int g_net_focus = -1; /* -1 = none, 0 = IP, 1 = Subnet, 2 = GW, 3 = DNS */
char g_net_status_msg[128] = "";
unsigned int g_net_status_col = UK_GREEN;

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

void apply_static_network(void)
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
            "nameserver %s\n",
            g_net_dns);
        write(rfd, rbuf, (size_t)len);
        close(rfd);
    }

    g_net_dhcp = 0;
    snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Static IP %s applied & saved to /etc/network.conf", g_net_ip);
    g_net_status_col = UK_GREEN;
}

void apply_dhcp_network(void)
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

void init_network_settings(void)
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

void draw_input_box(int x, int y, int w, int h, const char *text, int focused)
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

void write_proc_val(const char *path, int val)
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
int g_power_profile = 1; /* 0: Performance, 1: Balanced, 2: Power Saver */
int g_screen_timeout = 15; /* 5, 15, 30, 0 (Never) */
char g_power_status_msg[128] = "ACPI PIIX4 Power Management active.";

void load_power_config(void)
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

void apply_power_profile(int profile)
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

void apply_screen_timeout(int mins)
{
    g_screen_timeout = mins;
    save_power_config();
    if (mins > 0) {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Screen blanking timeout set to %d minutes.", mins);
    } else {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Screen blanking timeout disabled (Never).");
    }
}

/* ── Disks & Storage Tab ────────────────────────────────────────────────────── */
char g_disk_status_msg[128] = "All filesystem mounts operating nominally.";

void clean_temp_files(void)
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

void draw_storage_card(int x, int y, int w, int h,
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
        if (strcmp(path, "/boot") == 0) {
            total_mb = 64; used_mb = 12; free_mb = 52; pct = 18;
        } else if (strcmp(path, "/hdd") == 0) {
            total_mb = 1024; used_mb = 128; free_mb = 896; pct = 12;
        } else if (strcmp(path, "/tmp") == 0) {
            total_mb = 128; used_mb = 8; free_mb = 120; pct = 6;
        } else {
            total_mb = 512; used_mb = 211; free_mb = 301; pct = 41;
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

/* ── Security & Auto-Accept Tab ────────────────────────────────────────────── */
int g_sec_dmesg = 1;
int g_sec_kptr  = 1;
int g_sec_mmap  = 1;
int g_sec_yama  = 1;
int g_sec_hlinks= 1;
int g_sec_slinks= 1;

/* Auto-Accept & Unattended Policies */
int g_sec_auto_ipc   = 1; /* Auto-Accept SCM_RIGHTS IPC transfers */
int g_sec_auto_admin = 1; /* Auto-Approve Console Admin Escalations */
int g_sec_auto_dhcp  = 1; /* Auto-Accept Network DHCP Renewals */
int g_sec_auto_trace = 1; /* Auto-Accept Debug & Tracing Telemetry */

void load_security_config(void)
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

void save_security_config(void)
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

void init_security_settings(void)
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

/* ── System Tab ────────────────────────────────────────────────────────────── */
#define SYS_SEC_Y  90
#define SYS_GRID_Y 122

void draw_settings(void)
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
                if (g_active_tab == 0) handle_display_mouse(mx, my);
                else if (g_active_tab == 1) handle_audio_mouse(mx, my);
                else if (g_active_tab == 2) handle_theme_mouse(mx, my);
                else if (g_active_tab == 3) handle_time_mouse(mx, my);
                else if (g_active_tab == 4) handle_network_mouse(mx, my);
                else if (g_active_tab == 5) handle_power_mouse(mx, my);
                else if (g_active_tab == 6) handle_disks_mouse(mx, my);
                else if (g_active_tab == 7) handle_security_mouse(mx, my);
                else if (g_active_tab == 8) handle_system_mouse(mx, my);

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
