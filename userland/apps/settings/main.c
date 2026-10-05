/* ============================================================================
 * AzamiOS — Settings Panel
 * File: userland/apps/settings/main.c
 *
 * Features:
 *  • Display Configuration (Resolution, VSync, Compositing toggles)
 *  • Audio Control (Master Volume slider, OSS stereo PCM test chime)
 *  • Theme Switcher (file-backed: theme files under /usr/share/themes, /hdd/themes)
 *  • Time & Date Configuration (Timezones, Live clock)
 *  • Network Configuration (net0 live stats, IP, gateway, DNS, packets)
 *  • Security & Kernel Mitigations (Interactive sysctl toggles: dmesg_restrict,
 *    kptr_restrict, mmap_min_addr, yama_ptrace_scope, protected hardlinks/symlinks)
 *  • System Architecture & Memory Inspector
 * ============================================================================ */

#include "settings.h"
#include "config.h"
#include "../../libc/include/errno.h"

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

static int write_audio(int fd, const void *data, size_t len)
{
    const char *bytes = data;
    while (len) {
        ssize_t n = write(fd, bytes, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        bytes += n; len -= (size_t)n;
    }
    return 0;
}

void apply_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int fd = open("/dev/dsp", O_WRONLY);
    unsigned int vol = (unsigned int)pct | ((unsigned int)pct << 8);
    int ok = fd >= 0 && ioctl(fd, SOUND_PCM_WRITE_VOLUME, (unsigned long)&vol) == 0;
    if (fd >= 0) close(fd);
    if (ok) {
        g_volume_pct = pct;
        snprintf(g_settings_status, sizeof(g_settings_status), "Master volume: %d%%", pct);
    } else snprintf(g_settings_status, sizeof(g_settings_status), "Volume failed: audio unavailable or access denied.");
}

static void init_audio_settings(void)
{
    int fd = open("/dev/dsp", O_RDONLY);
    unsigned int volume;
    if (fd >= 0) {
        if (ioctl(fd, 0x80045004, (unsigned long)&volume) == 0) {
            unsigned int pct = ((volume & 255) + ((volume >> 8) & 255)) / 2;
            if (pct <= 100) g_volume_pct = (int)pct;
        }
        close(fd);
    }
}

void play_test_chime(void)
{
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) {
        snprintf(g_settings_status, sizeof(g_settings_status), "Chime failed: cannot open audio output.");
        return;
    }
    int rate = 44100, channels = 2, format = 0x10;
    int ok = ioctl(fd, 0xC0045005, (unsigned long)&format) == 0 && format == 0x10 &&
             ioctl(fd, 0xC0045006, (unsigned long)&channels) == 0 && channels == 2 &&
             ioctl(fd, 0xC0045002, (unsigned long)&rate) == 0 && rate > 0;
    short buffer[1024];
    const int notes[] = {523, 659, 784};
    for (int note = 0; ok && note < 3; note++) {
        int period = rate / notes[note];
        if (period < 2) { ok = 0; break; }
        int t = 0;
        for (int chunk = 0; ok && chunk < 8; chunk++) {
            for (int i = 0; i < 1024; i += 2) {
                short sample = t++ % period < period / 2 ? 12000 : -12000;
                buffer[i] = buffer[i + 1] = sample;
            }
            ok = write_audio(fd, buffer, sizeof(buffer)) == 0;
        }
    }
    close(fd);
    snprintf(g_settings_status, sizeof(g_settings_status), ok
             ? "Test chime sent to stereo audio output." : "Chime failed: PCM configuration or write rejected.");
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
char g_settings_status[128] = "Select a category to configure your system.";

int settings_write_file(const char *path, const char *data, size_t len)
{
    /* Write beside the destination and rename only a complete file. A denied
     * write or a full disk must leave the user's previous configuration intact. */
    char temporary[256];
    static unsigned int serial;
    if (snprintf(temporary, sizeof(temporary), "%s.settings.%u.%u", path,
                 (unsigned int)getpid(), ++serial) >= (int)sizeof(temporary)) return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(temporary); return -1; }
        done += (size_t)n;
    }
    if (close(fd) < 0 || rename(temporary, path) < 0) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int save_desktop_config(void)
{
    char input[4096] = "", output[4096], value[32];
    int fd = open("/etc/desktop.conf", O_RDONLY);
    if (fd >= 0) {
        size_t used = 0;
        for (;;) {
            ssize_t n = read(fd, input + used, sizeof(input) - 1 - used);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) { close(fd); return -1; }
            if (!n) break;
            used += (size_t)n;
            if (used == sizeof(input) - 1) { close(fd); return -1; }
        }
        close(fd);
        input[used] = '\0';
    } else if (errno != ENOENT) return -1;
    const char *sections[] = { "theme", "theme", "display", "display", "display" };
    const char *keys[] = { "theme_id", "name", "vsync", "compositing", "cursor_aa" };
    int values[] = { g_theme_selected, 0, g_vsync, g_composit, g_cursor_aa };
    for (int i = 0; i < 5; i++) {
        snprintf(value, sizeof(value), "%d", values[i]);
        const char *v = i == 1 ? az_theme_get(g_theme_selected)->name : value;
        if (settings_config_set(input, output, sizeof(output), sections[i], keys[i], v) < 0)
            return -1;
        strcpy(input, output);
    }
    return settings_write_file("/etc/desktop.conf", input, strlen(input));
}

void apply_theme(int theme_id)
{
    int count = az_theme_count();
    if (theme_id < 0 || theme_id >= count) return;
    int previous = g_theme_selected;
    g_theme_selected = theme_id;
    if (save_desktop_config() < 0) {
        g_theme_selected = previous;
        snprintf(g_settings_status, sizeof(g_settings_status), "Could not save desktop theme.");
        return;
    }
    az_wm_msg_t tmsg;
    memset(&tmsg, 0, sizeof(tmsg));
    tmsg.type = AZ_WM_SET_THEME;
    AZ_WM_MSG_THEME(&tmsg)->theme_id = (unsigned int)theme_id;
    int sent = az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&tmsg);
    char legacy[16];
    snprintf(legacy, sizeof(legacy), "%d\n", theme_id);
    settings_write_file("/etc/theme.conf", legacy, strlen(legacy));
    snprintf(g_settings_status, sizeof(g_settings_status), sent < 0
             ? "Theme saved; desktop notification failed." : "Desktop theme applied and saved.");
}

/* Persists a Display-tab toggle (VSync/Compositor/Cursor AA) the moment it
 * changes, instead of only ever being saved as a side effect of switching
 * themes -- otherwise a toggle flipped without also changing the theme was
 * pure UI state that vanished on the next apply_theme() call or restart. */
void save_display_settings(void)
{
    if (save_desktop_config() < 0) {
        g_vsync ^= 1;
        snprintf(g_settings_status, sizeof(g_settings_status), "Could not save display settings.");
    } else {
        snprintf(g_settings_status, sizeof(g_settings_status), "VSync saved. Restart the desktop to apply.");
    }
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
    { "London / Dublin",      "Europe/London",       "GMT/BST (UTC+00 / +01)" },
    { "Warsaw / Central EU",  "Europe/Warsaw",       "CET/CEST (UTC+01 / +02)" },
    { "Athens / Helsinki",    "Europe/Athens",       "EET/EEST (UTC+02 / +03)" },
    { "New York / Toronto",   "America/New_York",    "EST/EDT (UTC-05 / -04)" },
    { "Chicago / Dallas",     "America/Chicago",     "CST/CDT (UTC-06 / -05)" },
    { "Los Angeles / SF",     "America/Los_Angeles", "PST/PDT (UTC-08 / -07)" },
    { "Tokyo / Seoul",        "Asia/Tokyo",          "JST/KST (UTC+09:00)" }
};

int g_selected_tz_idx = 2; /* Default: Europe/Warsaw / Central EU */

void init_timezone_setting(void)
{
    char buf[64];
    int fd = open("/etc/timezone", O_RDONLY);
    if (fd < 0) return;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = '\0';
    for (int i = 0; i < 8; i++) {
        if (!strcmp(buf, g_tz_settings_list[i].tz_id) ||
            (i == 2 && (!strcmp(buf, "Europe/Paris") || !strcmp(buf, "Europe/Berlin")))) {
            g_selected_tz_idx = i;
            return;
        }
    }
}

void apply_timezone(int idx)
{
    if (idx < 0 || idx >= 8) return;
    const char *tz = g_tz_settings_list[idx].tz_id;
    if (settings_write_file("/etc/timezone", tz, strlen(tz)) < 0) {
        snprintf(g_settings_status, sizeof(g_settings_status), "Error: Could not save system timezone.");
        return;
    }
    g_selected_tz_idx = idx;
    unsetenv("TZ");
    tzset();
    snprintf(g_settings_status, sizeof(g_settings_status), "Timezone saved. Taskbar updates within one second.");
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
char g_net_ip[32]      = "0.0.0.0";
char g_net_netmask[32] = "0.0.0.0";
char g_net_gateway[32] = "0.0.0.0";
char g_net_dns[32]     = "0.0.0.0";
int g_net_focus = -1; /* -1 = none, 0 = IP, 1 = Subnet, 2 = GW, 3 = DNS */
char g_net_status_msg[128] = "";
unsigned int g_net_status_col = UK_GREEN;


/* Keep the Settings UI's state in standard ifupdown syntax.  /etc/network.conf
 * was an Azami-only INI file, so tools expecting the conventional Linux
 * /etc/network/interfaces layout could neither inspect nor reuse a profile. */
static int save_network_interfaces(int dhcp)
{
    char buf[512];
    int len;
    if (dhcp) {
        len = snprintf(buf, sizeof(buf),
            "# Generated by AzamiOS Settings\n"
            "auto lo\niface lo inet loopback\n\n"
            "auto net0\niface net0 inet dhcp\n");
    } else {
        len = snprintf(buf, sizeof(buf),
            "# Generated by AzamiOS Settings\n"
            "auto lo\niface lo inet loopback\n\n"
            "auto net0\niface net0 inet static\n"
            "    address %s\n    netmask %s\n    gateway %s\n"
            "    dns-nameservers %s\n",
            g_net_ip, g_net_netmask, g_net_gateway, g_net_dns);
    }
    return len > 0 ? settings_write_file("/etc/network/interfaces", buf, (size_t)len) : -1;
}

void apply_static_network(void)
{
    unsigned char ip[4], nm[4], gw[4], dns[4];
    if (settings_parse_ipv4(g_net_ip, ip) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid IP format");
        g_net_status_col = UK_RED;
        return;
    }
    if (settings_parse_ipv4(g_net_netmask, nm) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid Subnet Mask");
        g_net_status_col = UK_RED;
        return;
    }
    if (settings_parse_ipv4(g_net_gateway, gw) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid Gateway");
        g_net_status_col = UK_RED;
        return;
    }
    if (settings_parse_ipv4(g_net_dns, dns) != 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Invalid DNS Server");
        g_net_status_col = UK_RED;
        return;
    }

    unsigned int mask = ((unsigned int)nm[0] << 24) | ((unsigned int)nm[1] << 16) |
                        ((unsigned int)nm[2] << 8) | nm[3];
    unsigned int inverse = ~mask;
    if (!mask || (inverse & (inverse + 1U))) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Subnet mask must have contiguous network bits.");
        g_net_status_col = UK_RED;
        return;
    }
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd < 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Network adapter unavailable or access denied.");
        g_net_status_col = UK_RED;
        return;
    }
    int flags = 0x1043; /* UP | BROADCAST | RUNNING | MULTICAST */
    int ok = ioctl(fd, 0x8916, (unsigned long)ip) == 0 &&
             ioctl(fd, 0x891c, (unsigned long)nm) == 0 &&
             ioctl(fd, 0x891e, (unsigned long)gw) == 0 &&
             ioctl(fd, 0x8921, (unsigned long)dns) == 0 &&
             ioctl(fd, 0x8914, (unsigned long)&flags) == 0;
    close(fd);
    if (!ok) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Network operation failed; adapter may be partially configured.");
        g_net_status_col = UK_RED;
        return;
    }
    g_net_dhcp = 0;
    char rbuf[128];
    int len = snprintf(rbuf, sizeof(rbuf), "# Generated by AzamiOS Settings\nnameserver %s\n", g_net_dns);
    int saved = save_network_interfaces(0) == 0;
    if (settings_write_file("/etc/resolv.conf", rbuf, (size_t)len) < 0) saved = 0;
    snprintf(g_net_status_msg, sizeof(g_net_status_msg), saved
             ? "Static IP %s applied and saved." : "Static IP %s applied; configuration could not be saved.", g_net_ip);
    g_net_status_col = saved ? UK_GREEN : UK_YELLOW;
}

void apply_dhcp_network(void)
{
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd < 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: Network adapter unavailable or access denied.");
        g_net_status_col = UK_RED;
        return;
    }
    int result = ioctl(fd, 0x8990, 0);
    close(fd);
    if (result < 0) {
        snprintf(g_net_status_msg, sizeof(g_net_status_msg), "Error: DHCP request failed.");
        g_net_status_col = UK_RED;
        return;
    }
    g_net_dhcp = 1;
    g_net_focus = -1;
    int saved = save_network_interfaces(1) == 0;
    snprintf(g_net_status_msg, sizeof(g_net_status_msg), saved
             ? "DHCP requested; waiting for an assigned address." : "DHCP requested; profile could not be saved.");
    g_net_status_col = saved ? UK_GREEN : UK_YELLOW;
}

void refresh_network_stats(void)
{
    int fd = open("/dev/net0", O_RDWR, 0);
    if (fd >= 0) {
        unsigned char ip[4] = {0}, nm[4] = {0}, gw[4] = {0}, dns[4] = {0};
        if (ioctl(fd, 0x8915 /* SIOCGIFADDR */, (unsigned long)ip) == 0) {
            snprintf(g_net_ip, sizeof(g_net_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        }
        if (ioctl(fd, 0x891b /* SIOCGIFNETMASK */, (unsigned long)nm) == 0) {
            snprintf(g_net_netmask, sizeof(g_net_netmask), "%u.%u.%u.%u", nm[0], nm[1], nm[2], nm[3]);
        }
        if (ioctl(fd, 0x891d /* SIOCGIFGW */, (unsigned long)gw) == 0) {
            snprintf(g_net_gateway, sizeof(g_net_gateway), "%u.%u.%u.%u", gw[0], gw[1], gw[2], gw[3]);
        }
        if (ioctl(fd, 0x891f /* SIOCGIFDNS */, (unsigned long)dns) == 0) {
            snprintf(g_net_dns, sizeof(g_net_dns), "%u.%u.%u.%u", dns[0], dns[1], dns[2], dns[3]);
        }
        close(fd);
    }

}

void init_network_settings(void)
{
    refresh_network_stats();

    int cfd = open("/etc/network/interfaces", O_RDONLY, 0);
    if (cfd >= 0) {
        char cbuf[512];
        ssize_t n = read(cfd, cbuf, sizeof(cbuf) - 1);
        close(cfd);
        if (n > 0) {
            cbuf[n] = '\0';
            char *iface = strstr(cbuf, "iface net0 inet ");
            if (iface) g_net_dhcp = (strstr(iface, "dhcp") != NULL);
            char *p = strstr(cbuf, "address ");
            if (p) sscanf(p + 8, "%31s", g_net_ip);
            p = strstr(cbuf, "netmask ");
            if (p) sscanf(p + 8, "%31s", g_net_netmask);
            p = strstr(cbuf, "gateway ");
            if (p) sscanf(p + 8, "%31s", g_net_gateway);
            p = strstr(cbuf, "dns-nameservers ");
            if (p) sscanf(p + 16, "%31s", g_net_dns);
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

int write_proc_val(const char *path, int val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d\n", val);
    int ok = write(fd, buf, (size_t)len) == len;
    if (close(fd) < 0) ok = 0;
    return ok ? 0 : -1;
}

void toggle_proc_setting(const char *path, int *value, int enabled_value)
{
    int next = !*value;
    if (write_proc_val(path, next ? enabled_value : 0) < 0) {
        snprintf(g_settings_status, sizeof(g_settings_status), "Kernel setting failed: unavailable or access denied.");
        return;
    }
    *value = next;
    snprintf(g_settings_status, sizeof(g_settings_status), "Kernel setting applied for this boot.");
}

/* ── Power & Performance Tab ─────────────────────────────────────────────────── */
int g_power_profile = 1; /* 0: Performance, 1: Balanced, 2: Power Saver */
int g_screen_timeout = 0; /* 5, 15, 30, 0 (Never) */
char g_power_status_msg[128] = "Display blanking is available. CPU power profiles are unsupported.";

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
            if (t) {
                int timeout = atoi(t + 15);
                g_screen_timeout = timeout == 5 || timeout == 15 || timeout == 30 ? timeout : 0;
            }
        }
    }
}

static int save_power_config(void)
{
    char buf[256];
    int len = snprintf(buf, sizeof(buf),
             "# AzamiOS Power Management Configuration\nprofile=%d\nscreen_timeout=%d\n",
             g_power_profile, g_screen_timeout);
    return settings_write_file("/etc/power.conf", buf, (size_t)len);
}

void apply_screen_timeout(int mins)
{
    if (mins != 0 && mins != 5 && mins != 15 && mins != 30) return;
    int previous = g_screen_timeout;
    g_screen_timeout = mins;
    if (save_power_config() < 0) {
        g_screen_timeout = previous;
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Error: Could not save display timeout.");
    } else if (mins > 0) {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Display timeout: %d minutes. Desktop reloads within 5 seconds.", mins);
    } else {
        snprintf(g_power_status_msg, sizeof(g_power_status_msg), "Display timeout disabled. Desktop reloads within 5 seconds.");
    }
}

/* ── Disks & Storage Tab ────────────────────────────────────────────────────── */
char g_disk_status_msg[128] = "Usage comes from mounted filesystem statistics.";

void clean_temp_files(void)
{
    DIR *d = opendir("/tmp");
    if (!d) {
        snprintf(g_disk_status_msg, sizeof(g_disk_status_msg), "Error: Cannot open /tmp.");
        return;
    }
    unsigned int removed = 0, skipped = 0, failed = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char path[512];
        snprintf(path, sizeof(path), "/tmp/%s", de->d_name);
        struct stat st;
        if (lstat(path, &st) < 0) { failed++; continue; }
        if (!S_ISREG(st.st_mode)) { skipped++; continue; }
        if (unlink(path) == 0) removed++; else failed++;
    }
    closedir(d);
    sync();
    snprintf(g_disk_status_msg, sizeof(g_disk_status_msg), "%u files removed; %u non-files kept; %u failures. Buffers synced.", removed, skipped, failed);
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
        uk_draw_text_clip(&g_win, x + 12, y + 8, title, UK_TEXT, w - 24);
        uk_draw_text(&g_win, x + 12, y + 28, "Filesystem statistics unavailable.", UK_SUBTEXT0);
        return;
    }

    char title_buf[128];
    snprintf(title_buf, sizeof(title_buf), "%s (%s)", title, mount_point);
    uk_draw_text_clip(&g_win, x + 12, y + 8, title_buf, UK_TEXT, w - 24 - (fs_type[0] ? 160 : 0));

    char type_buf[64];
    snprintf(type_buf, sizeof(type_buf), "Type: %s", fs_type);
    if (fs_type[0]) uk_draw_text_clip(&g_win, x + w - 160, y + 8, type_buf, UK_SUBTEXT0, 148);

    char stat_buf[128];
    snprintf(stat_buf, sizeof(stat_buf), "%lu MB Used of %lu MB  •  %lu MB Free (%d%% full)",
             used_mb, total_mb, free_mb, pct);
    uk_draw_text_clip(&g_win, x + 12, y + 26, stat_buf, UK_SUBTEXT1, w - 24);

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

void init_security_settings(void)
{
    g_sec_dmesg  = read_proc_val("/proc/sys/kernel/dmesg_restrict", 1) > 0 ? 1 : 0;
    g_sec_kptr   = read_proc_val("/proc/sys/kernel/kptr_restrict", 1) > 0 ? 1 : 0;
    g_sec_mmap   = read_proc_val("/proc/sys/kernel/mmap_min_addr", 65536) > 0 ? 1 : 0;
    g_sec_yama   = read_proc_val("/proc/sys/kernel/yama/ptrace_scope", 1) > 0 ? 1 : 0;
    g_sec_hlinks = read_proc_val("/proc/sys/fs/protected_hardlinks", 1) > 0 ? 1 : 0;
    g_sec_slinks = read_proc_val("/proc/sys/fs/protected_symlinks", 1) > 0 ? 1 : 0;
}

static unsigned int g_mouse_buttons;
static int g_scroll_x, g_scroll_y, g_slider_drag;
static unsigned int *g_content_pixels;
static size_t g_content_capacity;

int settings_content_width(void)
{
    return g_win.width < WIN_W ? WIN_W : (int)g_win.width;
}

static int tab_columns(void)
{
    int cols = ((int)g_win.width - 18) / 78;
    return cols < 1 ? 1 : cols > NTABS ? NTABS : cols;
}

static int content_top(void)
{
    if (g_win.width < 360) return 80;
    return 44 + ((NTABS + tab_columns() - 1) / tab_columns()) * 36;
}

static int content_bottom(void)
{
    int bottom = (int)g_win.height - 38;
    return bottom > content_top() ? bottom : content_top();
}

static int content_height(void)
{
    int bottom = 464;
    if (g_active_tab == 0) bottom = display_content_height();
    if (g_active_tab == 6) bottom = disks_content_height();
    if (g_active_tab == 2) {
        int themes_bottom = 122 + ((az_theme_count() + 1) / 2) * 76;
        if (themes_bottom > bottom) bottom = themes_bottom;
    }
    return bottom;
}

static void clamp_scroll(void)
{
    int max_x = settings_content_width() - (int)g_win.width;
    int max_y = content_height() - 80 - (content_bottom() - content_top());
    if (max_y < 0) max_y = 0;
    if (g_scroll_x > max_x) g_scroll_x = max_x;
    if (g_scroll_y > max_y) g_scroll_y = max_y;
    if (g_scroll_x < 0) g_scroll_x = 0;
    if (g_scroll_y < 0) g_scroll_y = 0;
}

static void select_tab(int tab)
{
    g_active_tab = (tab + NTABS) % NTABS;
    g_scroll_x = g_scroll_y = g_slider_drag = 0;
    g_net_focus = -1;
    draw_settings();
}

void draw_settings(void)
{
    clamp_scroll();
    int w = (int)g_win.width, h = (int)g_win.height;
    uk_fill_rect(&g_win, 0, 0, w, h, UK_BASE);
    uk_gradient_h(&g_win, 0, 0, w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_MAUVE);
    uk_draw_text(&g_win, 16, 6, "AzamiOS Settings", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "Ctrl+Tab: category | Wheel: scroll | Shift+Arrows: sideways", UK_OVERLAY0);
    int cols = tab_columns();
    int tab_w = (w - 20 - (cols - 1) * 2) / cols;
    if (w < 360) {
        uk_draw_button(&g_win, 8, 48, 24, 28, "<", UK_BTN_NORMAL);
        uk_draw_text_clip(&g_win, 40, 56, g_tab_labels[g_active_tab], UK_TEXT, w - 80);
        uk_draw_button(&g_win, w - 32, 48, 24, 28, ">", UK_BTN_NORMAL);
    } else for (int i = 0; i < NTABS; i++) {
        uk_draw_tab_bar(&g_win, 10 + (i % cols) * (tab_w + 2),
                        44 + (i / cols) * 36, tab_w, 36,
                        &g_tab_labels[i], 1, i == g_active_tab ? 0 : -1);
    }
    int cw = settings_content_width(), ch = content_height();
    size_t needed = (size_t)cw * (size_t)ch;
    if (needed > g_content_capacity) {
        unsigned int *pixels = realloc(g_content_pixels, needed * sizeof(*pixels));
        if (pixels) { g_content_pixels = pixels; g_content_capacity = needed; }
    }
    if (needed <= g_content_capacity) {
        uk_window_t actual = g_win;
        g_win.pixels = g_content_pixels;
        g_win.width = (unsigned int)cw;
        g_win.height = (unsigned int)ch;
        g_win.clip_x0 = g_win.clip_y0 = g_win.clip_depth = 0;
        g_win.clip_x1 = cw; g_win.clip_y1 = ch;
        uk_fill_rect(&g_win, 0, 0, cw, ch, UK_BASE);
        switch (g_active_tab) {
        case 0: draw_display_tab(); break;
        case 1: draw_audio_tab(); break;
        case 2: draw_theme_tab(); break;
        case 3: draw_time_tab(); break;
        case 4: draw_network_tab(); break;
        case 5: draw_power_tab(); break;
        case 6: draw_disks_tab(); break;
        case 7: draw_security_tab(); break;
        case 8: draw_system_tab(); break;
        }
        g_win = actual;
        int top = content_top(), bottom = content_bottom();
        for (int y = top; y < bottom && y < h; y++) {
            int source_y = 80 + g_scroll_y + y - top;
            if (source_y >= ch) break;
            memcpy(g_win.pixels + (size_t)y * w,
                   g_content_pixels + (size_t)source_y * cw + g_scroll_x,
                   (size_t)w * sizeof(*g_content_pixels));
        }
        if (ch - 80 > bottom - top && bottom > top) {
            int track = bottom - top;
            int thumb = track * track / (ch - 80);
            if (thumb < 12) thumb = 12;
            if (thumb > track) thumb = track;
            int max_y = ch - 80 - track;
            int thumb_y = top + (track - thumb) * g_scroll_y / max_y;
            uk_fill_rect(&g_win, w - 5, top, 5, track, UK_SURFACE0);
            uk_fill_rect(&g_win, w - 5, thumb_y, 5, thumb, UK_OVERLAY0);
        }
    } else {
        uk_draw_text(&g_win, 20, content_top() + 12, "Insufficient memory for settings content.", UK_RED);
    }
    uk_fill_rect(&g_win, 0, h - 38, w, 38, UK_SURFACE0);
    uk_hline(&g_win, 0, h - 38, w, UK_SURFACE1);
    uk_push_clip(&g_win, 0, h - 38, w - 118, 38);
    uk_draw_text(&g_win, 16, h - 26, g_settings_status, UK_SUBTEXT0);
    uk_pop_clip(&g_win);
    uk_draw_button(&g_win, w - 110, h - 32, 96, 26, "Close", UK_BTN_NORMAL);
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
    init_audio_settings();
    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width && fb.height) { sw = fb.width; sh = fb.height; }
    if (uk_window_connect(&g_win, "Settings", (int)(sw / 2) - WIN_W / 2,
                          (int)(sh / 2) - WIN_H / 2, WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN) < 0)
        return -1;
    az_set_timer(g_win.client_chan, 1000, 0);
    draw_settings();
    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;
        if (msg.type == AZ_WM_DESTROY_WINDOW) break;
        if (msg.type == AZ_WM_FOCUS_CHANGE && !msg.focus.focused) {
            g_mouse_buttons = 0;
            g_slider_drag = 0;
        }
        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g_win, &msg)) break;
            g_slider_drag = 0;
            draw_settings();
            continue;
        }
        if (msg.type == AZ_WM_TIMER_TICK) {
            if (g_active_tab == 3 || g_active_tab == 4 || g_active_tab == 8) {
                if (g_active_tab == 4 && g_net_dhcp) refresh_network_stats();
                draw_settings();
            }
            continue;
        }
        if (msg.type == AZ_WM_KEY_EVENT) {
            if (!msg.key.pressed) continue;
            unsigned int key = msg.key.keycode;
            if (key == KEY_ESC) break;
            if (key == KEY_TAB && (msg.key.modifiers & AZ_MOD_CTRL)) {
                select_tab(g_active_tab + ((msg.key.modifiers & AZ_MOD_SHIFT) ? -1 : 1));
                continue;
            }
            if (key == KEY_PAGEUP || key == KEY_PAGEDOWN) {
                g_scroll_y += (key == KEY_PAGEUP ? -1 : 1) * (content_bottom() - content_top());
                draw_settings(); continue;
            }
            if ((msg.key.modifiers & AZ_MOD_SHIFT) && (key == KEY_LEFT || key == KEY_RIGHT)) {
                g_scroll_x += key == KEY_LEFT ? -80 : 80;
                draw_settings(); continue;
            }
            if (g_active_tab == 4 && !g_net_dhcp) {
                if (key == KEY_TAB) {
                    g_net_focus = (g_net_focus + ((msg.key.modifiers & AZ_MOD_SHIFT) ? 3 : 1)) % 4;
                } else if (key == KEY_ENTER || key == '\r') {
                    apply_static_network();
                } else if (g_net_focus >= 0 && g_net_focus < 4) {
                    char *fields[] = { g_net_ip, g_net_netmask, g_net_gateway, g_net_dns };
                    char *target = fields[g_net_focus];
                    size_t len = strlen(target);
                    if ((msg.key.modifiers & AZ_MOD_CTRL) && (key == 'a' || key == 'A')) target[0] = '\0';
                    else if (key == KEY_BACKSPACE && len) target[len - 1] = '\0';
                    else if (((key >= '0' && key <= '9') || key == '.') && len < 31) {
                        target[len] = (char)key; target[len + 1] = '\0';
                    }
                }
                draw_settings();
            }
            continue;
        }
        if (msg.type == AZ_WM_MOUSE_EVENT) {
            unsigned int pressed = uk_mouse_press(&g_mouse_buttons, msg.mouse.buttons);
            int mx = msg.mouse.abs_x, my = msg.mouse.abs_y;
            if (!(msg.mouse.buttons & AZ_MOUSE_BTN_LEFT)) g_slider_drag = 0;
            if (msg.mouse.wheel) {
                if (g_win.width < WIN_W && (g_scroll_y == 0 && content_height() - 80 <= content_bottom() - content_top()))
                    g_scroll_x += msg.mouse.wheel * 40;
                else g_scroll_y += msg.mouse.wheel * 40;
                draw_settings(); continue;
            }
            if (pressed & AZ_MOUSE_BTN_LEFT) {
                int w = (int)g_win.width, h = (int)g_win.height;
                if (mx >= w - 110 && mx < w - 14 && my >= h - 32 && my < h - 6) break;
                int cols = tab_columns(), tw = (w - 20 - (cols - 1) * 2) / cols;
                if (w < 360 && my >= 48 && my < 76) {
                    if (mx >= 8 && mx < 32) select_tab(g_active_tab - 1);
                    else if (mx >= w - 32 && mx < w - 8) select_tab(g_active_tab + 1);
                    continue;
                }
                if (w >= 360 && my >= 44 && my < content_top() && mx >= 10) {
                    int col = (mx - 10) / (tw + 2), row = (my - 44) / 36;
                    int tab = row * cols + col;
                    if (col < cols && (mx - 10) % (tw + 2) < tw && tab < NTABS) select_tab(tab);
                    continue;
                }
            }
            if (my < content_top() || my >= content_bottom() || mx < 0 || mx >= (int)g_win.width) continue;
            mx += g_scroll_x;
            my += 80 + g_scroll_y - content_top();
            if (g_active_tab == 1 && (pressed & AZ_MOUSE_BTN_LEFT) &&
                mx >= 20 && mx <= settings_content_width() - 160 && my >= 250 && my <= 280)
                g_slider_drag = 1;
            if (g_slider_drag && (msg.mouse.buttons & AZ_MOUSE_BTN_LEFT)) {
                int pct = (mx - 20) * 100 / (settings_content_width() - 180);
                apply_volume(pct); draw_settings(); continue;
            }
            if (!(pressed & AZ_MOUSE_BTN_LEFT)) continue;
            switch (g_active_tab) {
            case 0: handle_display_mouse(mx, my); break;
            case 1: handle_audio_mouse(mx, my); break;
            case 2: handle_theme_mouse(mx, my); break;
            case 3: handle_time_mouse(mx, my); break;
            case 4: handle_network_mouse(mx, my); break;
            case 5: handle_power_mouse(mx, my); break;
            case 6: handle_disks_mouse(mx, my); break;
            case 7: handle_security_mouse(mx, my); break;
            }
        }
    }
    az_wm_msg_t close_msg;
    memset(&close_msg, 0, sizeof(close_msg));
    close_msg.type = AZ_WM_DESTROY_WINDOW;
    close_msg.wid = g_win.wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&close_msg);
    free(g_content_pixels);
    return 0;
}
