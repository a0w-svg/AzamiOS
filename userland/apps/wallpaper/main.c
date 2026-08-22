/* ============================================================================
 * AzamiOS Desktop Environment — Dynamic Desktop & Animated Wallpaper
 * File: userland/apps/wallpaper/main.c   v4.0
 *
 * Modern Features:
 * ────────────────
 *  • Dynamic Desktop Icons: Scans /home/azami/Desktop and /desktop dynamically
 *  • Smart File Type & Glyph Detection (Folders, Text Docs, Apps, Audio, Art, Scripts)
 *  • Drag & Drop Desktop Icons: Real-time interactive positioning & custom coordinates
 *  • Right-Click Desktop Context Menu:
 *      - "New Text Note" (instant file creation in /home/azami/Desktop)
 *      - "New Folder" (instant folder creation)
 *      - "Refresh Desktop" / "Auto-Arrange Icons"
 *      - "Delete File / Shortcut"
 *      - Quick App Launchers & Multi-Theme Switching (5 Catppuccin themes)
 *  • Marquee Multi-Selection Box with glowing translucent frosted overlay
 *  • Autonomous Background Timer (SYS_AZ_SET_TIMER) for shifting gradients & twinkling stars
 *  • Automatic periodic directory re-scan to reflect filesystem changes
 *  • Full ZORDER_BOTTOM lock and seamless root background integration
 * ============================================================================ */

#include <stdbool.h>
#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/stat.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/de_log.h"

/* ── Build-time defaults ───────────────────────────────────────────────────── */
#define DEFAULT_WIDTH   1280
#define DEFAULT_HEIGHT   800
#define SERVER_CHAN        1

#define WALLPAPER_MAP_ADDR  ((void *)0x70000000)

/* ============================================================================
 * Color and Math Primitives
 * ============================================================================ */

static inline int clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static unsigned int xorshift32(unsigned int *state)
{
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void wp_fill_rect(unsigned int *px, unsigned int w, unsigned int h,
                         int rx, int ry, int rw, int rh, unsigned int color)
{
    int x, y;
    for (y = ry; y < ry + rh; y++) {
        if (y < 0 || (unsigned int)y >= h) continue;
        for (x = rx; x < rx + rw; x++) {
            if (x < 0 || (unsigned int)x >= w) continue;
            px[(unsigned int)y * w + (unsigned int)x] = color;
        }
    }
}

static unsigned int __attribute__((unused)) wp_blend(unsigned int dst, unsigned int src, unsigned int alpha)
{
    if (alpha > 256) alpha = 256;
    unsigned int sr = (src >> 16) & 0xFF;
    unsigned int sg = (src >>  8) & 0xFF;
    unsigned int sb =  src        & 0xFF;
    unsigned int dr = (dst >> 16) & 0xFF;
    unsigned int dg = (dst >>  8) & 0xFF;
    unsigned int db =  dst        & 0xFF;
    unsigned int r = (sr * alpha + dr * (256 - alpha)) >> 8;
    unsigned int g = (sg * alpha + dg * (256 - alpha)) >> 8;
    unsigned int b = (sb * alpha + db * (256 - alpha)) >> 8;
    return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/* ============================================================================
 * Dynamic Animated Gradient Scene
 * ============================================================================ */

static int g_wallpaper_theme = 0;

/* Integer triangle wave in range [-amplitude, +amplitude] */
static int tri_wave(unsigned int phase, unsigned int period, int amp)
{
    unsigned int p = phase % period;
    unsigned int half = period / 2;
    if (p < half) {
        return -amp + (int)((2 * amp * p) / half);
    } else {
        return amp - (int)((2 * amp * (p - half)) / half);
    }
}

static void wp_render_animated_gradient(unsigned int *px, unsigned int w, unsigned int h, unsigned int phase)
{
    if (w <= 0 || h <= 0) return;

    int tl_r = 0x1E, tl_g = 0x1E, tl_b = 0x2E;
    int tr_r = 0x1A, tr_g = 0x18, tr_b = 0x28;
    int bl_r = 0x11, bl_g = 0x11, bl_b = 0x1B;
    int br_r = 0x18, br_g = 0x18, br_b = 0x25;

    if (g_wallpaper_theme == 0) {
        /* Theme 0: Catppuccin Mocha */
        int shift1 = tri_wave(phase, 120, 10);
        int shift2 = tri_wave(phase + 40, 140, 12);
        int shift3 = tri_wave(phase + 80, 160, 8);
        tl_r = clamp_u8(0x1E + shift1); tl_g = 0x1E;                  tl_b = clamp_u8(0x2E + shift2);
        tr_r = 0x1A;                   tr_g = clamp_u8(0x18 + shift3); tr_b = clamp_u8(0x28 + shift1);
        bl_r = 0x11;                   bl_g = clamp_u8(0x11 + shift2); bl_b = 0x1B;
        br_r = clamp_u8(0x18 + shift2); br_g = 0x18;                  br_b = clamp_u8(0x25 + shift3);
    } else if (g_wallpaper_theme == 1) {
        /* Theme 1: Cyberpunk Neon */
        int s1 = tri_wave(phase, 90, 18);
        int s2 = tri_wave(phase + 45, 110, 22);
        tl_r = clamp_u8(0x35 + s1); tl_g = 0x0A; tl_b = clamp_u8(0x45 + s2);
        tr_r = 0x05; tr_g = clamp_u8(0x2A + s2); tr_b = clamp_u8(0x4A + s1);
        bl_r = 0x0D; bl_g = 0x05; bl_b = 0x1A;
        br_r = clamp_u8(0x22 + s2); br_g = 0x08; br_b = clamp_u8(0x30 + s1);
    } else if (g_wallpaper_theme == 2) {
        /* Theme 2: Emerald Aurora */
        int s1 = tri_wave(phase, 100, 20);
        int s2 = tri_wave(phase + 50, 130, 15);
        tl_r = 0x0A; tl_g = clamp_u8(0x32 + s1); tl_b = clamp_u8(0x28 + s2);
        tr_r = 0x06; tr_g = clamp_u8(0x40 + s2); tr_b = clamp_u8(0x35 + s1);
        bl_r = 0x06; bl_g = 0x12; bl_b = 0x15;
        br_r = 0x08; br_g = clamp_u8(0x24 + s1); br_b = 0x1F;
    } else if (g_wallpaper_theme == 3) {
        /* Theme 3: Sunset Horizon */
        int s1 = tri_wave(phase, 110, 16);
        int s2 = tri_wave(phase + 55, 125, 18);
        tl_r = clamp_u8(0x3C + s1); tl_g = clamp_u8(0x18 + s2); tl_b = 0x30;
        tr_r = clamp_u8(0x4A + s2); tr_g = clamp_u8(0x22 + s1); tr_b = 0x24;
        bl_r = 0x18; bl_g = 0x0C; bl_b = 0x22;
        br_r = clamp_u8(0x34 + s1); br_g = 0x12; br_b = 0x1C;
    } else {
        /* Theme 4: Midnight Ocean */
        int s1 = tri_wave(phase, 130, 14);
        int s2 = tri_wave(phase + 65, 150, 16);
        tl_r = 0x08; tl_g = clamp_u8(0x1C + s1); tl_b = clamp_u8(0x3A + s2);
        tr_r = 0x0A; tr_g = clamp_u8(0x26 + s2); tr_b = clamp_u8(0x48 + s1);
        bl_r = 0x04; bl_g = 0x0A; bl_b = 0x18;
        br_r = 0x06; br_g = clamp_u8(0x14 + s1); br_b = clamp_u8(0x28 + s2);
    }

    int div_h = (h > 1) ? (int)(h - 1) : 1;
    int div_w = (w > 1) ? (int)(w - 1) : 1;

    for (unsigned int y = 0; y < h; y++) {
        int left_r  = tl_r + (bl_r - tl_r) * (int)y / div_h;
        int left_g  = tl_g + (bl_g - tl_g) * (int)y / div_h;
        int left_b  = tl_b + (bl_b - tl_b) * (int)y / div_h;

        int right_r = tr_r + (br_r - tr_r) * (int)y / div_h;
        int right_g = tr_g + (br_g - tr_g) * (int)y / div_h;
        int right_b = tr_b + (br_b - tr_b) * (int)y / div_h;

        int step_r = ((right_r - left_r) << 16) / div_w;
        int step_g = ((right_g - left_g) << 16) / div_w;
        int step_b = ((right_b - left_b) << 16) / div_w;

        int cur_r = left_r << 16;
        int cur_g = left_g << 16;
        int cur_b = left_b << 16;

        unsigned int *line = &px[y * w];
        for (unsigned int x = 0; x < w; x++) {
            unsigned int r = (unsigned int)(cur_r >> 16);
            unsigned int g = (unsigned int)(cur_g >> 16);
            unsigned int b = (unsigned int)(cur_b >> 16);
            line[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
            cur_r += step_r;
            cur_g += step_g;
            cur_b += step_b;
        }
    }
}

/* Add subtle grain/film noise for modern texture */
static void wp_render_grain(unsigned int *px, unsigned int w, unsigned int h)
{
    unsigned int rng = 0x1337BEEF;
    unsigned int total = w * h;
    for (unsigned int i = 0; i < total; i += 3) {
        rng = xorshift32(&rng);
        int noise = (int)(rng & 0x07) - 3;
        unsigned int c = px[i];
        int r = (int)((c >> 16) & 0xFF) + noise;
        int g = (int)((c >>  8) & 0xFF) + noise;
        int b = (int)( c        & 0xFF) + noise;
        if (r < 0) { r = 0; }
        if (r > 255) { r = 255; }
        if (g < 0) { g = 0; }
        if (g > 255) { g = 255; }
        if (b < 0) { b = 0; }
        if (b > 255) { b = 255; }
        px[i] = 0xFF000000 | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
    }
}

/* Twinkling starfield */
static void wp_render_stars_animated(unsigned int *px, unsigned int w, unsigned int h, unsigned int phase)
{
    if (w == 0 || h == 0) return;
    unsigned int star_h = (h * 8) / 10;
    if (star_h == 0) return;
    unsigned int rng = 0xDEADBEEFu;

    for (unsigned int i = 0; i < 160; i++) {
        rng = xorshift32(&rng);
        unsigned int sx = rng % w;
        rng = xorshift32(&rng);
        unsigned int sy = rng % star_h;
        rng = xorshift32(&rng);

        unsigned int base = px[sy * w + sx];
        unsigned int br   = (base >> 16) & 0xFF;
        unsigned int bg   = (base >>  8) & 0xFF;
        unsigned int bb   =  base        & 0xFF;
        unsigned int lum  = (br + bg + bb) / 3;
        if (lum > 40) continue;

        /* Twinkle modulation */
        int twinkle = tri_wave(phase + i * 17, 60, 40);
        int brightness = 140 + (int)(rng % 60) + twinkle;
        if (brightness < 30) brightness = 30;
        if (brightness > 255) brightness = 255;

        unsigned int star_col = 0xFF000000
                              | ((unsigned int)brightness << 16)
                              | ((unsigned int)brightness << 8)
                              |  (unsigned int)brightness;
        px[sy * w + sx] = star_col;
    }
}

/* Logo and brand rendering */
static void wp_render_logo(unsigned int *px, unsigned int w, unsigned int h)
{
    const char *logo    = "AzamiOS";
    const char *tagline = "v7.0  x86_64 Microkernel";

    int logo_chars = 7;
    int logo_pixel_w = logo_chars * 16;
    int logo_x = (int)w / 2 - logo_pixel_w / 2;
    int logo_y = (int)h / 2 - 50;

    /* Drop Shadow */
    de_font_draw_str_2x(px, w, w, h, logo_x + 3, logo_y + 3, logo, 0xFF0B0B12);

    /* Main Mauve Logo */
    de_font_draw_str_2x(px, w, w, h, logo_x, logo_y, logo, 0xFFCBA6F7);

    /* Subtitle Tagline */
    int tag_chars = 0;
    while (tagline[tag_chars]) tag_chars++;
    int tag_x = (int)w / 2 - (tag_chars * 8) / 2;
    int tag_y = logo_y + 38;
    de_font_draw_str(px, w, w, h, tag_x + 1, tag_y + 1, tagline, 0xFF0B0B12);
    de_font_draw_str(px, w, w, h, tag_x,     tag_y,     tagline, 0xFF6C7086);
}

/* ============================================================================
 * Dynamic Desktop Icons & Context Menu
 * ============================================================================ */

#define MAX_DESKTOP_ICONS 64

#define ICON_TYPE_APP    0
#define ICON_TYPE_DOC    1
#define ICON_TYPE_DIR    2
#define ICON_TYPE_MEDIA  3
#define ICON_TYPE_SCRIPT 4

typedef struct {
    char label[48];
    char path[128];
    char glyph[8];
    int x, y;
    int is_custom_pos;
    unsigned int color;
    int is_dir;
    int file_type;
} desktop_icon_t;

static desktop_icon_t g_desktop_icons[MAX_DESKTOP_ICONS];
static int g_num_desktop_icons = 0;

static int g_selected_icon = -1;
static int g_hovered_icon  = -1;
static int g_dragged_icon  = -1;
static int g_drag_off_x    = 0;
static int g_drag_off_y    = 0;
static bool g_drag_moved   = false;

static bool g_context_menu_open = false;
static int g_context_menu_x = 0;
static int g_context_menu_y = 0;
static int g_context_menu_hover = -1;
static int g_context_menu_target_icon = -1;

/* Marquee Selection Box State */
static bool g_marquee_active = false;
static int  g_marquee_x1 = 0;
static int  g_marquee_y1 = 0;
static int  g_marquee_x2 = 0;
static int  g_marquee_y2 = 0;

static const char *g_theme_names[5] = {
    "Catppuccin Mocha",
    "Cyberpunk Neon",
    "Emerald Aurora",
    "Sunset Horizon",
    "Midnight Ocean"
};

/* Context Menu Items */
static const char *g_ctx_global_items[] = {
    "New Text Note",
    "New Folder",
    "Refresh Desktop",
    "Auto-Arrange Icons",
    "Open Terminal",
    "File Manager",
    "Text Editor",
    "Minesweeper Game",
    "Audio Player",
    "Paint & Sketch",
    "System Monitor",
    "Switch Theme",
    "About AzamiOS"
};
#define NUM_GLOBAL_CTX_ITEMS 13

static const char *g_ctx_icon_items[] = {
    "Open / Launch",
    "Edit in Text Editor",
    "Delete File",
    "Reset Position",
    "Refresh Desktop"
};
#define NUM_ICON_CTX_ITEMS 5

static void wp_launch(const char *path)
{
    az_wm_msg_t lmsg;
    memset(&lmsg, 0, sizeof(lmsg));
    lmsg.type = AZ_WM_LAUNCH_APP;
    az_wm_launch_payload_t *pl = AZ_WM_MSG_LAUNCH(&lmsg);
    int j;
    for (j = 0; j < AZ_WM_LAUNCH_PATH_MAX - 1 && path[j]; j++)
        pl->path[j] = path[j];
    pl->path[j] = '\0';
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&lmsg);
}

static void wp_open_icon(int idx)
{
    if (idx < 0 || idx >= g_num_desktop_icons) return;
    desktop_icon_t *icon = &g_desktop_icons[idx];

    if (icon->file_type == ICON_TYPE_DIR) {
        wp_launch("/bin/filemanager.elf");
    } else if (icon->file_type == ICON_TYPE_DOC) {
        /* Open text editor */
        wp_launch("/bin/texteditor.elf");
    } else if (icon->file_type == ICON_TYPE_MEDIA) {
        wp_launch("/bin/audioplayer.elf");
    } else {
        wp_launch(icon->path);
    }
}

static void wp_add_icon_entry(const char *label, const char *path, const char *glyph,
                              unsigned int color, int is_dir, int file_type,
                              unsigned int screen_w, unsigned int screen_h)
{
    if (g_num_desktop_icons >= MAX_DESKTOP_ICONS) return;

    /* Avoid duplicates */
    for (int i = 0; i < g_num_desktop_icons; i++) {
        if (strcmp(g_desktop_icons[i].path, path) == 0) return;
    }

    int idx = g_num_desktop_icons++;
    strncpy(g_desktop_icons[idx].label, label, sizeof(g_desktop_icons[idx].label) - 1);
    g_desktop_icons[idx].label[sizeof(g_desktop_icons[idx].label) - 1] = '\0';

    strncpy(g_desktop_icons[idx].path, path, sizeof(g_desktop_icons[idx].path) - 1);
    g_desktop_icons[idx].path[sizeof(g_desktop_icons[idx].path) - 1] = '\0';

    strncpy(g_desktop_icons[idx].glyph, glyph, sizeof(g_desktop_icons[idx].glyph) - 1);
    g_desktop_icons[idx].glyph[sizeof(g_desktop_icons[idx].glyph) - 1] = '\0';

    g_desktop_icons[idx].color = color;
    g_desktop_icons[idx].is_dir = is_dir;
    g_desktop_icons[idx].file_type = file_type;
    g_desktop_icons[idx].is_custom_pos = 0;

    /* Dynamic grid placement: column-major layout */
    int margin_x = 32;
    int margin_y = 36;
    int gap_x = 88;
    int gap_y = 88;
    int max_rows = ((int)screen_h - 70 - margin_y) / gap_y;
    if (max_rows < 1) max_rows = 1;

    int col = idx / max_rows;
    int row = idx % max_rows;
    g_desktop_icons[idx].x = margin_x + col * gap_x;
    g_desktop_icons[idx].y = margin_y + row * gap_y;
}

static void wp_auto_arrange_icons(unsigned int screen_w, unsigned int screen_h)
{
    int margin_x = 32;
    int margin_y = 36;
    int gap_x = 88;
    int gap_y = 88;
    int max_rows = ((int)screen_h - 70 - margin_y) / gap_y;
    if (max_rows < 1) max_rows = 1;

    for (int i = 0; i < g_num_desktop_icons; i++) {
        int col = i / max_rows;
        int row = i % max_rows;
        g_desktop_icons[i].x = margin_x + col * gap_x;
        g_desktop_icons[i].y = margin_y + row * gap_y;
        g_desktop_icons[i].is_custom_pos = 0;
    }
}

static void wp_scan_desktop_icons(unsigned int screen_w, unsigned int screen_h)
{
    /* Preserve any custom positions of existing icons */
    desktop_icon_t saved[MAX_DESKTOP_ICONS];
    int saved_count = g_num_desktop_icons;
    for (int i = 0; i < saved_count; i++) saved[i] = g_desktop_icons[i];

    g_num_desktop_icons = 0;

    /* 1. Scan /home/azami/Desktop and /desktop directories for dynamic user files */
    const char *desktop_dirs[] = { "/home/azami/Desktop", "/desktop", NULL };
    for (int d_idx = 0; desktop_dirs[d_idx] != NULL; d_idx++) {
        DIR *dir = opendir(desktop_dirs[d_idx]);
        if (dir) {
            struct dirent *d;
            while ((d = readdir(dir)) != NULL) {
                if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) continue;

                char full_path[128];
                snprintf(full_path, sizeof(full_path), "%s/%s", desktop_dirs[d_idx], d->d_name);

                struct stat st;
                int is_dir = 0;
                int ftype = ICON_TYPE_DOC;
                const char *glyph = "Txt";
                unsigned int color = 0xFF94E2D5; /* Teal */

                if (stat(full_path, &st) == 0) {
                    if (S_ISDIR(st.st_mode) || d->d_type == DT_DIR) {
                        is_dir = 1;
                        ftype = ICON_TYPE_DIR;
                        glyph = "[_]";
                        color = 0xFFF9E2AF; /* Yellow */
                    }
                }

                if (!is_dir) {
                    if (strstr(d->d_name, ".elf") != NULL) {
                        ftype = ICON_TYPE_APP;
                        glyph = "App";
                        color = 0xFF89B4FA; /* Blue */
                    } else if (strstr(d->d_name, ".sh") != NULL) {
                        ftype = ICON_TYPE_SCRIPT;
                        glyph = "sh";
                        color = 0xFFA6E3A1; /* Green */
                    } else if (strstr(d->d_name, ".wav") || strstr(d->d_name, ".mp3")) {
                        ftype = ICON_TYPE_MEDIA;
                        glyph = "Snd";
                        color = 0xFFF5C2E7; /* Pink */
                    } else if (strstr(d->d_name, ".png") || strstr(d->d_name, ".bmp") || strstr(d->d_name, ".icn")) {
                        ftype = ICON_TYPE_MEDIA;
                        glyph = "Art";
                        color = 0xFFCBA6F7; /* Mauve */
                    }
                }

                wp_add_icon_entry(d->d_name, full_path, glyph, color, is_dir, ftype, screen_w, screen_h);
            }
            closedir(dir);
        }
    }

    /* 2. Core Default Applications */
    wp_add_icon_entry("Terminal",     "/bin/terminal.elf",    ">_",  0xFF89B4FA, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Files",        "/bin/filemanager.elf", "[_]", 0xFFF9E2AF, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Calculator",   "/bin/calculator.elf",  "+-",  0xFFFAB387, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Text Editor",  "/bin/texteditor.elf",  "Txt", 0xFF94E2D5, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Paint",        "/bin/paint.elf",       "Art", 0xFFCBA6F7, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Audio",        "/bin/audioplayer.elf", "Snd", 0xFFF5C2E7, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Sys Monitor",  "/bin/sysmon.elf",      "CPU", 0xFFF38BA8, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Settings",     "/bin/settings.elf",    "Set", 0xFF74C7EC, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Minesweeper",  "/bin/minesweeper.elf", "[*]", 0xFFA6E3A1, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("2048",         "/bin/2048.elf",        "2048",0xFFFAB387, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Snake",        "/bin/snake.elf",       "~:>", 0xFFA6E3A1, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("Clock",        "/bin/clock.elf",       "(t)", 0xFFB4BEFE, 0, ICON_TYPE_APP, screen_w, screen_h);
    wp_add_icon_entry("About OS",     "/bin/about.elf",       "info", 0xFFF2CDCD, 0, ICON_TYPE_APP, screen_w, screen_h);

    /* 3. Restore any previously dragged custom positions */
    for (int i = 0; i < g_num_desktop_icons; i++) {
        for (int s = 0; s < saved_count; s++) {
            if (strcmp(g_desktop_icons[i].path, saved[s].path) == 0 && saved[s].is_custom_pos) {
                g_desktop_icons[i].x = saved[s].x;
                g_desktop_icons[i].y = saved[s].y;
                g_desktop_icons[i].is_custom_pos = 1;
                break;
            }
        }
    }
}

static void wp_create_desktop_note(unsigned int screen_w, unsigned int screen_h)
{
    /* Ensure desktop folder exists */
    mkdir("/home/azami", 0755);
    mkdir("/home/azami/Desktop", 0755);

    char filepath[128];
    for (int i = 1; i <= 99; i++) {
        if (i == 1) snprintf(filepath, sizeof(filepath), "/home/azami/Desktop/New_Note.txt");
        else snprintf(filepath, sizeof(filepath), "/home/azami/Desktop/New_Note_%d.txt", i);

        struct stat st;
        if (stat(filepath, &st) != 0) break;
    }

    int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char *content = "AzamiOS Desktop Quick Note\n===========================\nWrite your ideas here!\n";
        write(fd, content, strlen(content));
        close(fd);
    }

    wp_scan_desktop_icons(screen_w, screen_h);
}

static void wp_create_desktop_folder(unsigned int screen_w, unsigned int screen_h)
{
    mkdir("/home/azami", 0755);
    mkdir("/home/azami/Desktop", 0755);

    char dirpath[128];
    for (int i = 1; i <= 99; i++) {
        if (i == 1) snprintf(dirpath, sizeof(dirpath), "/home/azami/Desktop/New_Folder");
        else snprintf(dirpath, sizeof(dirpath), "/home/azami/Desktop/New_Folder_%d", i);

        struct stat st;
        if (stat(dirpath, &st) != 0) break;
    }

    mkdir(dirpath, 0755);
    wp_scan_desktop_icons(screen_w, screen_h);
}

static void wp_delete_desktop_file(int icon_idx, unsigned int screen_w, unsigned int screen_h)
{
    if (icon_idx < 0 || icon_idx >= g_num_desktop_icons) return;
    const char *path = g_desktop_icons[icon_idx].path;
    if (strncmp(path, "/home/azami/Desktop/", 20) == 0 || strncmp(path, "/desktop/", 9) == 0) {
        unlink(path);
        wp_scan_desktop_icons(screen_w, screen_h);
    }
}

static void wp_draw_icon(unsigned int *pixels, unsigned int w, unsigned int h, desktop_icon_t *icon, bool selected, bool hovered)
{
    unsigned int bg = selected ? 0x8845475A : (hovered ? 0x55313244 : 0x22181825);
    wp_fill_rect(pixels, w, h, icon->x, icon->y, 56, 56, bg);
    wp_fill_rect(pixels, w, h, icon->x + 2, icon->y + 2, 52, 2, hovered ? 0xFF89B4FA : 0x4445475A);
    wp_fill_rect(pixels, w, h, icon->x + 10, icon->y + 8, 36, 26, icon->color);

    /* Render short glyph badge inside icon */
    int glen = 0;
    while (icon->glyph[glen]) glen++;
    int gx = icon->x + 10 + (36 - glen * 8) / 2;
    int gy = icon->y + 13;
    de_font_draw_str(pixels, w, w, h, gx, gy, icon->glyph, 0xFF11111B);

    /* Label text centered below icon (max 10 chars per line) */
    int llen = 0;
    while (icon->label[llen]) llen++;
    int lx = icon->x + 28 - (llen * 8) / 2;
    int ly = icon->y + 60;
    de_font_draw_str(pixels, w, w, h, lx + 1, ly + 1, icon->label, 0xFF0B0B12);
    de_font_draw_str(pixels, w, w, h, lx,     ly,     icon->label, hovered ? 0xFFFFFFFF : 0xFFCDD6F4);
}

static void wp_draw_context_menu(unsigned int *pixels, unsigned int w, unsigned int h, int x, int y, int hover)
{
    bool is_icon_ctx = (g_context_menu_target_icon >= 0 && g_context_menu_target_icon < g_num_desktop_icons);
    int num_items = is_icon_ctx ? NUM_ICON_CTX_ITEMS : NUM_GLOBAL_CTX_ITEMS;
    const char **items = is_icon_ctx ? g_ctx_icon_items : g_ctx_global_items;

    int menu_w = 176;
    int menu_h = num_items * 28 + 8;
    if (x + menu_w > (int)w) x = (int)w - menu_w - 4;
    if (y + menu_h > (int)h) y = (int)h - menu_h - 4;

    wp_fill_rect(pixels, w, h, x, y, menu_w, menu_h, 0xFF181825);
    wp_fill_rect(pixels, w, h, x + 1, y + 1, menu_w - 2, menu_h - 2, 0xFF1E1E2E);

    for (int i = 0; i < num_items; i++) {
        int iy = y + 4 + i * 28;
        if (i == hover) {
            wp_fill_rect(pixels, w, h, x + 4, iy, menu_w - 8, 24, 0xFF45475A);
        }
        const char *item_text = (!is_icon_ctx && i == 11 && g_wallpaper_theme >= 0 && g_wallpaper_theme < 5)
                                ? g_theme_names[g_wallpaper_theme]
                                : items[i];
        for (int j = 0; item_text[j]; j++) {
            unsigned char c = (unsigned char)item_text[j];
            if (c >= 0x20 && c < 0x7F) {
                const unsigned char *glyph = de_font8x16[c - 0x20];
                for (int r = 0; r < 16; r++) {
                    unsigned char row = glyph[r];
                    for (int b = 0; b < 8; b++) {
                        if (row & (0x80 >> b)) {
                            int px = x + 12 + j * 8 + b;
                            int py = iy + 4 + r;
                            if (px >= 0 && (unsigned int)px < w && py >= 0 && (unsigned int)py < h) {
                                pixels[py * w + px] = (i == hover) ? 0xFFCDD6F4 : (!is_icon_ctx && i == 11 ? 0xFFCBA6F7 : 0xFFBAC2DE);
                            }
                        }
                    }
                }
            }
        }
    }
}

#define BG_CACHE_MAP_ADDR ((void *)0x66000000)
static unsigned int *g_bg_cache = (unsigned int *)0;

static void wallpaper_render_base(unsigned int *px, unsigned int w, unsigned int h)
{
    wp_render_animated_gradient(px, w, h, 0);
    wp_render_grain(px, w, h);
    wp_render_logo(px, w, h);
}

static void wp_draw_marquee(unsigned int *pixels, unsigned int w, unsigned int h)
{
    if (!g_marquee_active) return;
    int min_x = g_marquee_x1 < g_marquee_x2 ? g_marquee_x1 : g_marquee_x2;
    int max_x = g_marquee_x1 > g_marquee_x2 ? g_marquee_x1 : g_marquee_x2;
    int min_y = g_marquee_y1 < g_marquee_y2 ? g_marquee_y1 : g_marquee_y2;
    int max_y = g_marquee_y1 > g_marquee_y2 ? g_marquee_y1 : g_marquee_y2;

    int mw = max_x - min_x;
    int mh = max_y - min_y;
    if (mw <= 2 || mh <= 2) return;

    /* Frosted translucent tint */
    for (int y = min_y; y < max_y; y++) {
        if (y < 0 || (unsigned int)y >= h) continue;
        unsigned int *line = &pixels[y * w];
        for (int x = min_x; x < max_x; x++) {
            if (x < 0 || (unsigned int)x >= w) continue;
            unsigned int bg = line[x];
            unsigned int r = (((bg >> 16) & 0xFF) * 192 + 0x89 * 64) >> 8;
            unsigned int g = (((bg >>  8) & 0xFF) * 192 + 0xB4 * 64) >> 8;
            unsigned int b = (( bg        & 0xFF) * 192 + 0xFA * 64) >> 8;
            line[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    /* Glowing accent border */
    for (int x = min_x; x < max_x; x++) {
        if (x >= 0 && (unsigned int)x < w) {
            if (min_y >= 0 && (unsigned int)min_y < h) pixels[min_y * w + x] = 0xFF89B4FA;
            if (max_y - 1 >= 0 && (unsigned int)(max_y - 1) < h) pixels[(max_y - 1) * w + x] = 0xFF89B4FA;
        }
    }
    for (int y = min_y; y < max_y; y++) {
        if (y >= 0 && (unsigned int)y < h) {
            if (min_x >= 0 && (unsigned int)min_x < w) pixels[y * w + min_x] = 0xFF89B4FA;
            if (max_x - 1 >= 0 && (unsigned int)(max_x - 1) < w) pixels[y * w + max_x - 1] = 0xFF89B4FA;
        }
    }
}

static void wallpaper_render_frame(unsigned int *pixels, unsigned int w, unsigned int h, unsigned int phase)
{
    wp_render_animated_gradient(pixels, w, h, phase);
    wp_render_stars_animated(pixels, w, h, phase);
    wp_render_logo(pixels, w, h);

    /* Draw desktop icons */
    for (int i = 0; i < g_num_desktop_icons; i++) {
        wp_draw_icon(pixels, w, h, &g_desktop_icons[i], (g_selected_icon == i), (g_hovered_icon == i));
    }

    wp_draw_marquee(pixels, w, h);

    if (g_context_menu_open) {
        wp_draw_context_menu(pixels, w, h, g_context_menu_x, g_context_menu_y, g_context_menu_hover);
    }
}

/* ============================================================================
 * main
 * ============================================================================ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    de_log("[wallpaper] v4.0 — dynamic desktop and animated root window starting");

    az_fb_info_t fb;
    unsigned int screen_w = DEFAULT_WIDTH;
    unsigned int screen_h = DEFAULT_HEIGHT;

    if (az_fb_info(&fb) == 0) {
        if (fb.width  > 0 && fb.width  <= 7680) screen_w = fb.width;
        if (fb.height > 0 && fb.height <= 4320) screen_h = fb.height;
    }

    int client_chan = az_channel_create();
    if (client_chan < 0) {
        de_log("[wallpaper] FATAL: az_channel_create failed");
        return -1;
    }

    az_wm_msg_t req;
    memset(&req, 0, sizeof(req));
    req.type            = AZ_WM_CREATE_WINDOW;
    req.client_chan      = (unsigned int)client_chan;
    req.create.x        = 0;
    req.create.y        = 0;
    req.create.w        = screen_w;
    req.create.h        = screen_h;
    req.create.title[0] = '\0';   /* blank title */

    if (az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&req) < 0) {
        de_log("[wallpaper] FATAL: channel_send failed");
        return -1;
    }

    az_wm_msg_t resp;
    unsigned int wid = 0;
    int shmem_id = -1;
    for (;;) {
        int r = az_channel_recv(client_chan, (az_ipc_msg_t *)&resp);
        if (r < 0) {
            de_log("[wallpaper] FATAL: recv failed");
            return -1;
        }
        if (resp.type == AZ_WM_WINDOW_CREATED) {
            wid      = resp.created.assigned_wid;
            shmem_id = (int)resp.created.shmem_id;
            break;
        }
    }

    if (az_shmem_map(shmem_id, WALLPAPER_MAP_ADDR) < 0) {
        de_log("[wallpaper] FATAL: shmem_map failed");
        return -1;
    }
    unsigned int *pixels = (unsigned int *)WALLPAPER_MAP_ADDR;

    /* Allocate background cache buffer */
    size_t bg_pages = (screen_w * screen_h * sizeof(unsigned int) + 4095) / 4096;
    int bg_shmem = az_shmem_create(bg_pages);
    if (bg_shmem >= 0 && az_shmem_map(bg_shmem, BG_CACHE_MAP_ADDR) == 0) {
        g_bg_cache = (unsigned int *)BG_CACHE_MAP_ADDR;
        int cfd = open("/etc/desktop.conf", O_RDONLY, 0);
        if (cfd < 0) cfd = open("/etc/theme.conf", O_RDONLY, 0);
        if (cfd >= 0) {
            char cbuf[256];
            ssize_t cn = read(cfd, cbuf, sizeof(cbuf) - 1);
            close(cfd);
            if (cn > 0) {
                cbuf[cn] = '\0';
                char *tid = strstr(cbuf, "theme_id=");
                if (tid) {
                    int id = atoi(tid + 9);
                    if (id >= 0 && id < 5) g_wallpaper_theme = id;
                } else if (cbuf[0] >= '0' && cbuf[0] <= '9') {
                    int id = atoi(cbuf);
                    if (id >= 0 && id < 5) g_wallpaper_theme = id;
                }
            }
        }
        wallpaper_render_base(g_bg_cache, screen_w, screen_h);
    }

    /* Initialize dynamic desktop icons */
    wp_scan_desktop_icons(screen_w, screen_h);

    unsigned int phase = 0;
    wallpaper_render_frame(pixels, screen_w, screen_h, phase);

    /* Lock to bottom */
    az_wm_msg_t zmsg;
    memset(&zmsg, 0, sizeof(zmsg));
    zmsg.type = AZ_WM_SET_ZORDER_HINT;
    az_wm_zorder_payload_t *zpl = AZ_WM_MSG_ZORDER(&zmsg);
    zpl->wid  = wid;
    zpl->band = AZ_WM_ZORDER_BOTTOM;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&zmsg);

    /* Invalidate initial frame */
    az_wm_msg_t inv;
    memset(&inv, 0, sizeof(inv));
    inv.type = AZ_WM_INVALIDATE;
    inv.wid  = wid;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);

    /* Subscribe to DE broadcasts (e.g. theme changes) */
    az_wm_msg_t smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.type = AZ_WM_SUBSCRIBE_EVENTS;
    AZ_WM_MSG_SUBSCRIBE(&smsg)->subscriber_chan = (unsigned int)client_chan;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&smsg);

    /* Register autonomous timer for smooth periodic animation (~200ms = 5 FPS) */
    az_set_timer(client_chan, 200, 0);

    de_log("[wallpaper] Entering autonomous animation & event loop.");

    unsigned int prev_btn = 0;
    unsigned int rescan_counter = 0;

    for (;;) {
        az_wm_msg_t msg;
        int r = az_channel_recv(client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) {
            de_log("[wallpaper] IPC channel disconnected, exiting loop.");
            break;
        }

        switch (msg.type) {
        case AZ_WM_TIMER_TICK:
            phase++;
            rescan_counter++;
            /* Periodic filesystem re-scan every ~3 seconds (15 ticks) */
            if (rescan_counter >= 15) {
                rescan_counter = 0;
                wp_scan_desktop_icons(screen_w, screen_h);
            }
            wallpaper_render_frame(pixels, screen_w, screen_h, phase);
            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);
            break;

        case AZ_WM_MOUSE_EVENT: {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            bool lclick_down = (msg.mouse.buttons & 1) != 0;
            bool lclick = lclick_down && !(prev_btn & 1);
            bool lrelease = !lclick_down && (prev_btn & 1);
            bool rclick = (msg.mouse.buttons & 2) && !(prev_btn & 2);
            prev_btn = msg.mouse.buttons;

            bool redraw = false;

            if (g_context_menu_open) {
                bool is_icon_ctx = (g_context_menu_target_icon >= 0 && g_context_menu_target_icon < g_num_desktop_icons);
                int num_items = is_icon_ctx ? NUM_ICON_CTX_ITEMS : NUM_GLOBAL_CTX_ITEMS;
                int menu_w = 176;
                int item_h = 28;
                int old_h = g_context_menu_hover;
                if (mx >= g_context_menu_x && mx < g_context_menu_x + menu_w &&
                    my >= g_context_menu_y && my < g_context_menu_y + num_items * item_h + 8) {
                    g_context_menu_hover = (my - (g_context_menu_y + 4)) / item_h;
                    if (g_context_menu_hover < 0 || g_context_menu_hover >= num_items) g_context_menu_hover = -1;
                } else {
                    g_context_menu_hover = -1;
                }
                if (g_context_menu_hover != old_h) redraw = true;

                if (lclick) {
                    if (g_context_menu_hover >= 0) {
                        if (is_icon_ctx) {
                            switch (g_context_menu_hover) {
                            case 0: /* Open */
                                wp_open_icon(g_context_menu_target_icon);
                                break;
                            case 1: /* Edit in Text Editor */
                                wp_launch("/bin/texteditor.elf");
                                break;
                            case 2: /* Delete File */
                                wp_delete_desktop_file(g_context_menu_target_icon, screen_w, screen_h);
                                break;
                            case 3: /* Reset Position */
                                g_desktop_icons[g_context_menu_target_icon].is_custom_pos = 0;
                                wp_auto_arrange_icons(screen_w, screen_h);
                                break;
                            case 4: /* Refresh */
                                wp_scan_desktop_icons(screen_w, screen_h);
                                break;
                            }
                        } else {
                            switch (g_context_menu_hover) {
                            case 0: wp_create_desktop_note(screen_w, screen_h); break;
                            case 1: wp_create_desktop_folder(screen_w, screen_h); break;
                            case 2: wp_scan_desktop_icons(screen_w, screen_h); break;
                            case 3: wp_auto_arrange_icons(screen_w, screen_h); break;
                            case 4: wp_launch("/bin/terminal.elf"); break;
                            case 5: wp_launch("/bin/filemanager.elf"); break;
                            case 6: wp_launch("/bin/texteditor.elf"); break;
                            case 7: wp_launch("/bin/minesweeper.elf"); break;
                            case 8: wp_launch("/bin/audioplayer.elf"); break;
                            case 9: wp_launch("/bin/paint.elf"); break;
                            case 10: wp_launch("/bin/sysmon.elf"); break;
                            case 11:
                                g_wallpaper_theme = (g_wallpaper_theme + 1) % 5;
                                break;
                            case 12: wp_launch("/bin/about.elf"); break;
                            }
                        }
                    }
                    g_context_menu_open = false;
                    g_context_menu_target_icon = -1;
                    redraw = true;
                }
            } else {
                int hover_icon = -1;
                for (int i = 0; i < g_num_desktop_icons; i++) {
                    if (mx >= g_desktop_icons[i].x && mx < g_desktop_icons[i].x + 56 &&
                        my >= g_desktop_icons[i].y && my < g_desktop_icons[i].y + 76) {
                        hover_icon = i;
                        break;
                    }
                }
                if (hover_icon != g_hovered_icon) {
                    g_hovered_icon = hover_icon;
                    redraw = true;
                }

                if (rclick) {
                    int clicked_icon = -1;
                    for (int i = 0; i < g_num_desktop_icons; i++) {
                        if (mx >= g_desktop_icons[i].x && mx < g_desktop_icons[i].x + 56 &&
                            my >= g_desktop_icons[i].y && my < g_desktop_icons[i].y + 76) {
                            clicked_icon = i;
                            break;
                        }
                    }
                    g_context_menu_open = true;
                    g_context_menu_target_icon = clicked_icon;
                    g_context_menu_x = mx;
                    g_context_menu_y = my;
                    g_context_menu_hover = -1;
                    g_marquee_active = false;
                    g_dragged_icon = -1;
                    redraw = true;
                } else if (lclick) {
                    int clicked_icon = -1;
                    for (int i = 0; i < g_num_desktop_icons; i++) {
                        if (mx >= g_desktop_icons[i].x && mx < g_desktop_icons[i].x + 56 &&
                            my >= g_desktop_icons[i].y && my < g_desktop_icons[i].y + 76) {
                            clicked_icon = i;
                            break;
                        }
                    }
                    if (clicked_icon >= 0) {
                        g_dragged_icon = clicked_icon;
                        g_drag_off_x = mx - g_desktop_icons[clicked_icon].x;
                        g_drag_off_y = my - g_desktop_icons[clicked_icon].y;
                        g_drag_moved = false;
                        g_selected_icon = clicked_icon;
                        g_marquee_active = false;
                        redraw = true;
                    } else {
                        /* Start Marquee Selection Box */
                        g_selected_icon = -1;
                        g_marquee_active = true;
                        g_marquee_x1 = mx;
                        g_marquee_y1 = my;
                        g_marquee_x2 = mx;
                        g_marquee_y2 = my;
                        redraw = true;
                    }
                } else if (lclick_down && g_dragged_icon >= 0) {
                    /* Dragging Desktop Icon */
                    int new_x = mx - g_drag_off_x;
                    int new_y = my - g_drag_off_y;
                    if (new_x < 0) new_x = 0;
                    if (new_x > (int)screen_w - 60) new_x = (int)screen_w - 60;
                    if (new_y < 0) new_y = 0;
                    if (new_y > (int)screen_h - 100) new_y = (int)screen_h - 100;

                    if (abs(new_x - g_desktop_icons[g_dragged_icon].x) > 3 ||
                        abs(new_y - g_desktop_icons[g_dragged_icon].y) > 3) {
                        g_drag_moved = true;
                        g_desktop_icons[g_dragged_icon].is_custom_pos = 1;
                    }
                    g_desktop_icons[g_dragged_icon].x = new_x;
                    g_desktop_icons[g_dragged_icon].y = new_y;
                    redraw = true;
                } else if (lrelease && g_dragged_icon >= 0) {
                    if (!g_drag_moved) {
                        /* Regular click / double click */
                        static int last_click_icon = -1;
                        static unsigned int last_click_phase = 0;
                        if (last_click_icon == g_dragged_icon && (phase - last_click_phase) < 4) {
                            wp_open_icon(g_dragged_icon);
                            g_selected_icon = -1;
                            last_click_icon = -1;
                        } else {
                            last_click_icon = g_dragged_icon;
                            last_click_phase = phase;
                        }
                    }
                    g_dragged_icon = -1;
                    g_drag_moved = false;
                    redraw = true;
                } else if (lclick_down && g_marquee_active) {
                    /* Dragging Marquee Box */
                    g_marquee_x2 = mx;
                    g_marquee_y2 = my;

                    /* Check intersected icons */
                    int min_x = g_marquee_x1 < g_marquee_x2 ? g_marquee_x1 : g_marquee_x2;
                    int max_x = g_marquee_x1 > g_marquee_x2 ? g_marquee_x1 : g_marquee_x2;
                    int min_y = g_marquee_y1 < g_marquee_y2 ? g_marquee_y1 : g_marquee_y2;
                    int max_y = g_marquee_y1 > g_marquee_y2 ? g_marquee_y1 : g_marquee_y2;

                    for (int i = 0; i < g_num_desktop_icons; i++) {
                        int ix = g_desktop_icons[i].x + 28;
                        int iy = g_desktop_icons[i].y + 28;
                        if (ix >= min_x && ix <= max_x && iy >= min_y && iy <= max_y) {
                            g_selected_icon = i;
                            break;
                        }
                    }
                    redraw = true;
                } else if (lrelease && g_marquee_active) {
                    g_marquee_active = false;
                    redraw = true;
                }
            }

            if (redraw) {
                wallpaper_render_frame(pixels, screen_w, screen_h, phase);
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);
            }
            break;
        }

        case AZ_WM_EVT_THEME_CHANGED: {
            az_wm_theme_payload_t *tp = AZ_WM_MSG_THEME(&msg);
            g_wallpaper_theme = (int)tp->theme_id;
            if (g_bg_cache) {
                wallpaper_render_base(g_bg_cache, screen_w, screen_h);
            }
            wallpaper_render_frame(pixels, screen_w, screen_h, phase);
            az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&inv);
            break;
        }

        case AZ_WM_FOCUS_CHANGE:
            if (msg.focus.focused) {
                az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&zmsg);
            }
            break;

        default:
            break;
        }
    }

    sys_exit(0);
}

