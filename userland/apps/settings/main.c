/* ============================================================================
 * AzamiOS — Settings Panel (v2.0 with Audio Controls & Telemetry)
 * File: userland/apps/settings/main.c
 * ============================================================================ */
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/string.h"
#include "../../libc/include/sys/syscall.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       540
#define WIN_H       420
#define MAP_ADDR    ((void *)0x69000000)

static uk_window_t g_win;

/* ── Tabs ────────────────────────────────────────────────────────────────────── */
#define NTABS  4
static const char *g_tab_labels[NTABS] = { "Display", "Audio", "Theme", "System" };
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

/* ── Theme swatches (Catppuccin flavours) ─────────────────────────────────────── */
typedef struct { const char *name; unsigned int bg; unsigned int accent; unsigned int text; } theme_t;
static const theme_t g_themes[4] = {
    { "Mocha",    UK_BASE,   UK_MAUVE,   UK_TEXT      },
    { "Macchiato",0xFF24273A, 0xFFC6A0F6, 0xFFCAD3F5  },
    { "Frappe",   0xFF303446, 0xFFCA9EE6, 0xFFC6D0F5  },
    { "Latte",    0xFFEFF1F5, 0xFF8839EF, 0xFF4C4F69  },
};
static int g_theme_selected = 0;

/* ── Toggle switches ─────────────────────────────────────────────────────────── */
static int g_vsync    = 1;
static int g_composit = 1;
static int g_cursor_aa= 0;

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
    return (mx >= tx && mx < tx + 40 && my >= ty && my < ty + 20);
}

static void draw_display_tab(void)
{
    int px = 20, py = 90;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Framebuffer", UK_BLUE);
    py += 32;

    az_fb_info_t fb;
    char res[32];
    if (az_fb_info(&fb) == 0) {
        snprintf(res, sizeof(res), "%ux%u", fb.width, fb.height);
    } else {
        snprintf(res, sizeof(res), "1280x800");
    }

    uk_draw_panel(&g_win, px, py, (int)w - 40, 36, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, py + 4,  "Resolution", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, py + 20, res, UK_TEXT);
    py += 44;

    uk_draw_panel(&g_win, px, py, (int)w - 40, 36, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, py + 4,  "Colour depth", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, py + 20, "32bpp ARGB (Hardware accelerated)", UK_TEXT);
    py += 52;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Compositor", UK_MAUVE);
    py += 32;

    draw_toggle(px, py,       g_vsync,    "VSync Page Flipping"); py += 36;
    draw_toggle(px, py,       g_composit, "Double Buffered Framebuffer"); py += 36;
    draw_toggle(px, py,       g_cursor_aa,"Hardware Cursor Blending");
}

static void draw_audio_tab(void)
{
    int px = 20, py = 90;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Audio Device", UK_GREEN);
    py += 32;

    uk_draw_panel(&g_win, px, py, (int)w - 40, 36, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, py + 4,  "Output Device", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, py + 20, "Intel AC97 Audio Controller (/dev/dsp)", UK_TEXT);
    py += 44;

    uk_draw_panel(&g_win, px, py, (int)w - 40, 36, UK_SURFACE0);
    uk_draw_text(&g_win, px + 12, py + 4,  "Audio Format", UK_SUBTEXT0);
    uk_draw_text(&g_win, px + 12, py + 20, "44,100 Hz, 16-bit PCM Stereo", UK_TEXT);
    py += 52;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Master Volume", UK_YELLOW);
    py += 32;

    /* Volume Slider Track */
    int slider_w = (int)w - 180;
    uk_fill_rounded_rect(&g_win, px, py + 4, slider_w, 12, 6, UK_SURFACE1);
    int fill_w = (slider_w * g_volume_pct) / 100;
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, px, py + 4, fill_w, 12, 6, UK_GREEN);
    }
    uk_fill_circle(&g_win, px + fill_w, py + 10, 9, UK_TEXT);

    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_volume_pct);
    uk_draw_text(&g_win, px + slider_w + 16, py + 2, vol_str, UK_TEXT);

    /* Test Chime button */
    uk_draw_button(&g_win, px, py + 36, 120, 26, "Play Chime", UK_BTN_NORMAL);
}

static void draw_theme_tab(void)
{
    int px = 20, py = 90;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Catppuccin Flavour", UK_MAUVE);
    py += 32;

    int i;
    for (i = 0; i < 4; i++) {
        int row_x = px + (i % 2) * ((int)(w - 40) / 2);
        int row_y = py + (i / 2) * 80;

        unsigned int border = (i == g_theme_selected) ? UK_MAUVE : UK_SURFACE1;
        uk_fill_rounded_rect(&g_win, row_x, row_y, ((int)(w - 40)) / 2 - 8, 64, 8, g_themes[i].bg);

        int bx, by;
        for (bx = row_x; bx < row_x + ((int)(w - 40))/2 - 8; bx++) {
            uk_put_pixel(&g_win, bx, row_y, border);
            uk_put_pixel(&g_win, bx, row_y + 63, border);
        }
        for (by = row_y; by < row_y + 64; by++) {
            uk_put_pixel(&g_win, row_x, by, border);
            uk_put_pixel(&g_win, row_x + ((int)(w-40))/2 - 9, by, border);
        }

        uk_fill_circle(&g_win, row_x + 20, row_y + 28, 10, g_themes[i].accent);
        uk_fill_circle(&g_win, row_x + 42, row_y + 28, 10, g_themes[i].text);
        uk_fill_circle(&g_win, row_x + 64, row_y + 28, 10, g_themes[i].bg);

        uk_draw_text(&g_win, row_x + 10, row_y + 46, g_themes[i].name, g_themes[i].text);

        if (i == g_theme_selected) {
            uk_draw_text(&g_win, row_x + ((int)(w-40))/2 - 28, row_y + 6, "* ", UK_MAUVE);
        }
    }
}

static void draw_system_tab(void)
{
    int px = 20, py = 90;
    unsigned int w = g_win.width;

    uk_draw_section_header(&g_win, px, py, (int)w - 40, "Kernel & System", UK_TEAL);
    py += 26;

    struct sysinfo si;
    sysinfo(&si);
    unsigned long total_mb = (si.totalram * si.mem_unit) / (1024 * 1024);

    char mem_buf[48];
    snprintf(mem_buf, sizeof(mem_buf), "%lu MB Total (CFS Active)", total_mb);

    static const char *sys_info[][2] = {
        { "OS Version",    "AzamiOS v7.0.0 (x86_64)" },
        { "Kernel",        "AzamiOS Microkernel SMP (4 Cores)" },
        { "Scheduler",     "CFS (Completely Fair Scheduler)" },
        { "Memory Model",  "PMM Buddy + 4-Level VMM PML4" },
        { "IPC System",    "az_channel (Zero-Copy Ring)" },
        { "Filesystem",    "Virtual File System (VFS) + Ext2" },
        { "Bootloader",    "Limine v8 Protocol" },
    };

    for (int i = 0; i < 7; i++) {
        uk_draw_panel(&g_win, px, py, (int)w - 40, 22, UK_SURFACE0);
        uk_draw_text(&g_win, px + 8,  py + 3, sys_info[i][0], UK_SUBTEXT0);
        uk_draw_text(&g_win, px + 150, py + 3, sys_info[i][1], UK_TEXT);
        py += 26;
    }
}

static void draw_settings(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* Header */
    uk_gradient_h(&g_win, 0, 0, (int)w, 44, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 44, UK_OVERLAY1);
    uk_draw_text(&g_win, 16, 6,  "Settings", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "System Preferences & Hardware Controls", UK_OVERLAY0);
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
    case 3: draw_system_tab();  break;
    }

    /* Footer */
    uk_fill_rect(&g_win, 0, (int)h - 36, (int)w, 36, UK_SURFACE0);
    uk_hline(&g_win, 0, (int)h - 36, (int)w, UK_SURFACE1);
    uk_draw_button(&g_win, (int)w - 120, (int)h - 28, 100, 22, "Apply", UK_BTN_NORMAL);

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[settings] Starting...");

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0) { sw = fb.width; sh = fb.height; }

    int ret = uk_window_connect(&g_win, "Settings",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) { de_log("[settings] FATAL"); return -1; }

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
                    if (t >= 0 && t < NTABS) { g_active_tab = t; draw_settings(); continue; }
                }

                /* Display tab toggles */
                if (g_active_tab == 0) {
                    if (hit_toggle(20, 90 + 32 + 44 + 52 + 32,      mx, my)) { g_vsync    ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, 90 + 32 + 44 + 52 + 32 + 36, mx, my)) { g_composit ^= 1; draw_settings(); continue; }
                    if (hit_toggle(20, 90 + 32 + 44 + 52 + 32 + 72, mx, my)) { g_cursor_aa^= 1; draw_settings(); continue; }
                }

                /* Audio tab */
                if (g_active_tab == 1) {
                    int slider_y = 90 + 32 + 44 + 52 + 32;
                    int slider_w = (int)w - 180;
                    if (mx >= 20 && mx <= 20 + slider_w && my >= slider_y - 8 && my <= slider_y + 24) {
                        int pct = ((mx - 20) * 100) / slider_w;
                        apply_volume(pct);
                        draw_settings();
                        continue;
                    }
                    /* Play chime button */
                    if (mx >= 20 && mx <= 140 && my >= slider_y + 36 && my <= slider_y + 62) {
                        play_test_chime();
                        continue;
                    }
                }

                /* Theme tab swatch clicks */
                if (g_active_tab == 2) {
                    int i;
                    for (i = 0; i < 4; i++) {
                        int row_x = 20 + (i % 2) * ((int)(w - 40) / 2);
                        int row_y = 90 + 32 + (i / 2) * 80;
                        if (mx >= row_x && mx < row_x + ((int)(w - 40)) / 2 - 8 &&
                            my >= row_y && my < row_y + 64) {
                            g_theme_selected = i;
                            draw_settings();
                            break;
                        }
                    }
                }
            }
        }
    }
    sys_exit(0);
}
