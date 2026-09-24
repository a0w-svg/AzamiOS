/* ============================================================================
 * AzamiOS Desktop Environment — Modern Image Viewer (imageviewer.elf v2.0)
 * File: userland/apps/imageviewer/main.c
 *
 * Features:
 *  • Multi-format image decoders:
 *     - PPM (P6 24-bit binary & P3 ASCII)
 *     - BMP (24-bit & 32-bit uncompressed RGB/RGBA)
 *     - TGA (Truevision 24/32bpp uncompressed & Type 10 RLE compressed)
 *     - QOI (Quite OK Image format fast lossless decoder)
 *     - ICN (AzamiOS 32x32 native ARGB icon format)
 *  • Advanced Viewport & Transform:
 *     - Dynamic Zoom (10% to 1600%) & Fit-to-window
 *     - Smooth Drag-to-Pan
 *     - 90° / 180° / 270° Rotation
 *  • Image Filters & Effects:
 *     - Invert Colors ('i')
 *     - Grayscale ('g')
 *     - Brightness increase/decrease ('[' and ']')
 *  • Slideshow Mode ('s' / Spacebar auto-advance)
 *  • Set Active Image as Desktop Wallpaper ('w')
 *  • Image Properties & Metadata Overlay (Tab / Info button)
 *  • Catppuccin Mocha aesthetic with toolbar & status bar
 * ============================================================================ */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "../../libc/include/az/ipc.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1

/* Opening size only: WIN_W/WIN_H below read the live window, so the
 * toolbar, canvas, status bar and the zoom-to-fit calculation all follow a
 * resize or a maximize. They used to be fixed at 780x540, so a maximized
 * viewer drew its image and chrome into the top-left corner of the window
 * and left the rest unpainted. */
#define INIT_W      780
#define INIT_H      540
#define WIN_W       ((int)g_win.width)
#define WIN_H       ((int)g_win.height)
#define MAP_ADDR    ((void *)0x6A000000)

#define TOOLBAR_H   42
#define STATUS_H    26
#define CANVAS_Y    TOOLBAR_H
#define CANVAS_H    (WIN_H - TOOLBAR_H - STATUS_H)

/* ── Image Data ────────────────────────────────────────────────────────────── */
static unsigned int *g_img_pixels = NULL;
static int           g_img_w = 0;
static int           g_img_h = 0;
static char          g_img_format[24] = "";
static size_t        g_img_file_size = 0;

/* Transformed buffer (rotation + filters) */
static unsigned int *g_rot_pixels = NULL;
static int           g_rot_w = 0;
static int           g_rot_h = 0;
static int           g_rotation = 0; /* 0, 90, 180, 270 */

/* Filters */
static bool g_filter_invert = false;
static bool g_filter_grayscale = false;
static int  g_brightness = 0; /* -120 to +120 */

/* Slideshow */
static bool g_slideshow = false;
static int  g_slideshow_ticks = 0;

/* Info Overlay */
static bool g_show_info = false;

/* Viewport state */
static float g_zoom = 1.0f;
static bool  g_fit_mode = true;
static int   g_pan_x = 0;
static int   g_pan_y = 0;
static bool  g_dragging = false;
static int   g_drag_start_mx = 0;
static int   g_drag_start_my = 0;
static int   g_drag_orig_pan_x = 0;
static int   g_drag_orig_pan_y = 0;

/* File cycling in folder */
#define MAX_FOLDER_IMAGES 128
static char g_folder_images[MAX_FOLDER_IMAGES][256];
static int  g_folder_count = 0;
static int  g_folder_index = -1;
static char g_current_path[512] = "";

static uk_window_t g_win;

/* ── Toolbar Buttons ───────────────────────────────────────────────────────── */
typedef struct {
    const char *label;
    int x, y, w, h;
    int id;
} iv_button_t;

enum {
    BTN_PREV = 1,
    BTN_NEXT,
    BTN_ZOOM_OUT,
    BTN_ZOOM_IN,
    BTN_ZOOM_100,
    BTN_ZOOM_FIT,
    BTN_ROTATE,
    BTN_SLIDESHOW,
    BTN_INVERT,
    BTN_GRAYSCALE,
    BTN_WALLPAPER,
    BTN_INFO
};

static const iv_button_t g_buttons[] = {
    { "< Prev",   8,   6, 62, 30, BTN_PREV },
    { "Next >",  74,   6, 62, 30, BTN_NEXT },
    { "[ - ]",  142,   6, 44, 30, BTN_ZOOM_OUT },
    { "[ + ]",  190,   6, 44, 30, BTN_ZOOM_IN },
    { "100%",   238,   6, 48, 30, BTN_ZOOM_100 },
    { "Fit",    290,   6, 42, 30, BTN_ZOOM_FIT },
    { "Rotate", 336,   6, 58, 30, BTN_ROTATE },
    { "Slide",  398,   6, 52, 30, BTN_SLIDESHOW },
    { "Invert", 454,   6, 56, 30, BTN_INVERT },
    { "Gray",   514,   6, 48, 30, BTN_GRAYSCALE },
    { "Wall",   566,   6, 48, 30, BTN_WALLPAPER },
    { "Info",   618,   6, 46, 30, BTN_INFO },
};
#define NUM_BUTTONS (sizeof(g_buttons) / sizeof(g_buttons[0]))
static int g_hovered_btn = -1;

/* ── Format Check Helpers ─────────────────────────────────────────────────── */
static bool has_image_ext(const char *name)
{
    if (!name) return false;
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = strrchr(name, '.');
    if (!ext) return false;
    ext++;
    if (strcasecmp(ext, "ppm") == 0) return true;
    if (strcasecmp(ext, "bmp") == 0) return true;
    if (strcasecmp(ext, "tga") == 0) return true;
    if (strcasecmp(ext, "qoi") == 0) return true;
    if (strcasecmp(ext, "icn") == 0) return true;
    if (strcasecmp(ext, "png") == 0) return true;
    return false;
}

/* ── PPM Decoder (P6 and P3) ──────────────────────────────────────────────── */
static unsigned int *load_ppm(const unsigned char *data, size_t size, int *out_w, int *out_h)
{
    if (size < 8) return NULL;
    if (data[0] != 'P' || (data[1] != '6' && data[1] != '3')) return NULL;
    int is_p6 = (data[1] == '6');

    size_t pos = 2;
    #define SKIP_WS() do { \
        while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\n' || data[pos] == '\r')) pos++; \
        if (pos < size && data[pos] == '#') { \
            while (pos < size && data[pos] != '\n' && data[pos] != '\r') pos++; \
            while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\n' || data[pos] == '\r')) pos++; \
        } \
    } while(0)

    SKIP_WS();
    int w = 0, h = 0, maxval = 0;
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        w = w * 10 + (data[pos++] - '0');
    }
    SKIP_WS();
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        h = h * 10 + (data[pos++] - '0');
    }
    SKIP_WS();
    while (pos < size && data[pos] >= '0' && data[pos] <= '9') {
        maxval = maxval * 10 + (data[pos++] - '0');
    }

    if (w <= 0 || h <= 0 || maxval <= 0 || w > 4096 || h > 4096) return NULL;

    if (pos < size && (data[pos] == ' ' || data[pos] == '\n' || data[pos] == '\r' || data[pos] == '\t')) pos++;

    unsigned int *px = (unsigned int *)malloc((size_t)w * h * sizeof(unsigned int));
    if (!px) return NULL;

    if (is_p6) {
        for (int i = 0; i < w * h; i++) {
            if (pos + 3 > size) { free(px); return NULL; }
            unsigned int r = data[pos++];
            unsigned int g = data[pos++];
            unsigned int b = data[pos++];
            if (maxval != 255) {
                r = (r * 255) / maxval;
                g = (g * 255) / maxval;
                b = (b * 255) / maxval;
            }
            px[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    } else {
        for (int i = 0; i < w * h; i++) {
            SKIP_WS();
            int r = 0, g = 0, b = 0;
            while (pos < size && data[pos] >= '0' && data[pos] <= '9') r = r * 10 + (data[pos++] - '0');
            SKIP_WS();
            while (pos < size && data[pos] >= '0' && data[pos] <= '9') g = g * 10 + (data[pos++] - '0');
            SKIP_WS();
            while (pos < size && data[pos] >= '0' && data[pos] <= '9') b = b * 10 + (data[pos++] - '0');
            if (maxval != 255) {
                r = (r * 255) / maxval;
                g = (g * 255) / maxval;
                b = (b * 255) / maxval;
            }
            px[i] = 0xFF000000 | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
        }
    }
    #undef SKIP_WS

    *out_w = w;
    *out_h = h;
    return px;
}

/* ── BMP Decoder (24-bit and 32-bit uncompressed) ────────────────────────── */
static unsigned int *load_bmp(const unsigned char *data, size_t size, int *out_w, int *out_h)
{
    if (size < 54) return NULL;
    if (data[0] != 'B' || data[1] != 'M') return NULL;

    unsigned int data_offset = *(const unsigned int *)&data[10];
    int w = *(const int *)&data[18];
    int h = *(const int *)&data[22];
    unsigned short bpp = *(const unsigned short *)&data[28];
    unsigned int compression = *(const unsigned int *)&data[30];

    if (w <= 0 || h == 0 || (bpp != 24 && bpp != 32) || compression != 0) return NULL;
    if (data_offset >= size) return NULL;

    bool top_down = (h < 0);
    if (h < 0) h = -h;

    unsigned int *px = (unsigned int *)malloc((size_t)w * h * sizeof(unsigned int));
    if (!px) return NULL;

    size_t row_stride = (bpp == 24) ? (((w * 3) + 3) & ~3) : (w * 4);

    for (int y = 0; y < h; y++) {
        int src_y = top_down ? y : (h - 1 - y);
        size_t row_pos = data_offset + (size_t)src_y * row_stride;
        if (row_pos + w * (bpp / 8) > size) { free(px); return NULL; }

        const unsigned char *src_p = data + row_pos;
        unsigned int *dst_line = px + (size_t)y * w;

        if (bpp == 24) {
            for (int x = 0; x < w; x++) {
                unsigned int b = src_p[0];
                unsigned int g = src_p[1];
                unsigned int r = src_p[2];
                dst_line[x] = 0xFF000000 | (r << 16) | (g << 8) | b;
                src_p += 3;
            }
        } else {
            for (int x = 0; x < w; x++) {
                unsigned int b = src_p[0];
                unsigned int g = src_p[1];
                unsigned int r = src_p[2];
                unsigned int a = src_p[3];
                if (a == 0) a = 0xFF;
                dst_line[x] = (a << 24) | (r << 16) | (g << 8) | b;
                src_p += 4;
            }
        }
    }

    *out_w = w;
    *out_h = h;
    return px;
}

/* ── TGA Decoder (24/32bpp Truecolor, uncompressed & Type 10 RLE) ────────── */
static unsigned int *load_tga(const unsigned char *data, size_t size, int *out_w, int *out_h)
{
    if (size < 18) return NULL;
    uint8_t id_len = data[0];
    uint8_t img_type = data[2];
    int w = (int)data[12] | ((int)data[13] << 8);
    int h = (int)data[14] | ((int)data[15] << 8);
    uint8_t bpp = data[16];
    uint8_t desc = data[17];

    if (w <= 0 || h <= 0 || (bpp != 24 && bpp != 32)) return NULL;
    if (img_type != 2 && img_type != 10) return NULL;

    bool top_to_bottom = (desc & 0x20) != 0;
    int bytes_per_px = bpp / 8;

    unsigned int *px = (unsigned int *)malloc((size_t)w * h * sizeof(unsigned int));
    if (!px) return NULL;

    const unsigned char *src = data + 18 + id_len;
    const unsigned char *src_end = data + size;

    if (img_type == 2) {
        for (int row = 0; row < h; row++) {
            int y = top_to_bottom ? row : (h - 1 - row);
            unsigned int *dst_row = px + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                if (src + bytes_per_px > src_end) { free(px); return NULL; }
                uint8_t b = src[0];
                uint8_t g = src[1];
                uint8_t r = src[2];
                uint8_t a = (bytes_per_px == 4) ? src[3] : 0xFF;
                dst_row[x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
                src += bytes_per_px;
            }
        }
    } else {
        int total = w * h;
        unsigned int *flat = (unsigned int *)malloc((size_t)total * sizeof(unsigned int));
        if (!flat) { free(px); return NULL; }

        int count = 0;
        while (count < total && src < src_end) {
            uint8_t pkt = *src++;
            int run = (pkt & 0x7F) + 1;
            if (pkt & 0x80) {
                if (src + bytes_per_px > src_end) break;
                uint8_t b = src[0];
                uint8_t g = src[1];
                uint8_t r = src[2];
                uint8_t a = (bytes_per_px == 4) ? src[3] : 0xFF;
                unsigned int col = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
                src += bytes_per_px;
                for (int i = 0; i < run && count < total; i++) flat[count++] = col;
            } else {
                for (int i = 0; i < run && count < total; i++) {
                    if (src + bytes_per_px > src_end) break;
                    uint8_t b = src[0];
                    uint8_t g = src[1];
                    uint8_t r = src[2];
                    uint8_t a = (bytes_per_px == 4) ? src[3] : 0xFF;
                    flat[count++] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
                    src += bytes_per_px;
                }
            }
        }

        for (int y = 0; y < h; y++) {
            int src_y = top_to_bottom ? y : (h - 1 - y);
            memcpy(px + (size_t)y * w, flat + (size_t)src_y * w, (size_t)w * sizeof(unsigned int));
        }
        free(flat);
    }

    *out_w = w;
    *out_h = h;
    return px;
}

/* ── QOI Decoder (Quite OK Image format) ──────────────────────────────────── */
static unsigned int *load_qoi(const unsigned char *data, size_t size, int *out_w, int *out_h)
{
    if (size < 14) return NULL;
    if (data[0] != 'q' || data[1] != 'o' || data[2] != 'i' || data[3] != 'f') return NULL;

    uint32_t w = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | (uint32_t)data[7];
    uint32_t h = ((uint32_t)data[8] << 24) | ((uint32_t)data[9] << 16) | ((uint32_t)data[10] << 8) | (uint32_t)data[11];
    uint8_t channels = data[12];
    if (w == 0 || h == 0 || (channels != 3 && channels != 4)) return NULL;

    size_t total = (size_t)w * h;
    unsigned int *px = (unsigned int *)malloc(total * sizeof(unsigned int));
    if (!px) return NULL;

    unsigned int index[64];
    memset(index, 0, sizeof(index));

    uint8_t r = 0, g = 0, b = 0, a = 255;
    size_t p_idx = 0;
    size_t pos = 14;
    size_t end = (size >= 8) ? size - 8 : size;

    while (p_idx < total && pos < end) {
        uint8_t b1 = data[pos++];
        if (b1 == 0xFE) {
            if (pos + 3 > size) break;
            r = data[pos++]; g = data[pos++]; b = data[pos++];
        } else if (b1 == 0xFF) {
            if (pos + 4 > size) break;
            r = data[pos++]; g = data[pos++]; b = data[pos++]; a = data[pos++];
        } else if ((b1 & 0xC0) == 0x00) {
            unsigned int c = index[b1 & 0x3F];
            a = (c >> 24) & 0xFF; r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
        } else if ((b1 & 0xC0) == 0x40) {
            r += ((b1 >> 4) & 0x03) - 2; g += ((b1 >> 2) & 0x03) - 2; b += (b1 & 0x03) - 2;
        } else if ((b1 & 0xC0) == 0x80) {
            if (pos >= size) break;
            uint8_t b2 = data[pos++];
            int dg = (b1 & 0x3F) - 32;
            r += dg + ((b2 >> 4) & 0x0F) - 8; g += dg; b += dg + (b2 & 0x0F) - 8;
        } else if ((b1 & 0xC0) == 0xC0) {
            int run = (b1 & 0x3F) + 1;
            unsigned int cur_val = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
            for (int k = 0; k < run && p_idx < total; k++) px[p_idx++] = cur_val;
            continue;
        }

        unsigned int cur_val = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
        int hash = ((int)r * 3 + (int)g * 5 + (int)b * 7 + (int)a * 11) % 64;
        index[hash] = cur_val;
        px[p_idx++] = cur_val;
    }

    *out_w = (int)w;
    *out_h = (int)h;
    return px;
}

/* ── ICN Decoder (Azami native 32x32 ARGB) ────────────────────────────────── */
static unsigned int *load_icn(const unsigned char *data, size_t size, int *out_w, int *out_h)
{
    if (size != 4096) return NULL;
    unsigned int *px = (unsigned int *)malloc(4096);
    if (!px) return NULL;
    memcpy(px, data, 4096);
    *out_w = 32;
    *out_h = 32;
    return px;
}

/* ── Transformed Pixels Update (Rotation + Filters) ───────────────────────── */
static void update_transformed_pixels(void)
{
    if (!g_img_pixels || g_img_w <= 0 || g_img_h <= 0) return;
    if (g_rot_pixels) {
        free(g_rot_pixels);
        g_rot_pixels = NULL;
    }

    if (g_rotation == 0 || g_rotation == 180) {
        g_rot_w = g_img_w;
        g_rot_h = g_img_h;
    } else {
        g_rot_w = g_img_h;
        g_rot_h = g_img_w;
    }

    g_rot_pixels = (unsigned int *)malloc((size_t)g_rot_w * g_rot_h * sizeof(unsigned int));
    if (!g_rot_pixels) return;

    for (int y = 0; y < g_rot_h; y++) {
        for (int x = 0; x < g_rot_w; x++) {
            int src_x = x, src_y = y;
            if (g_rotation == 90) {
                src_x = y;
                src_y = g_img_h - 1 - x;
            } else if (g_rotation == 180) {
                src_x = g_img_w - 1 - x;
                src_y = g_img_h - 1 - y;
            } else if (g_rotation == 270) {
                src_x = g_img_w - 1 - y;
                src_y = x;
            }

            unsigned int col = g_img_pixels[src_y * g_img_w + src_x];
            unsigned int a = (col >> 24) & 0xFF;
            int r = (col >> 16) & 0xFF;
            int g = (col >> 8) & 0xFF;
            int b = col & 0xFF;

            /* Filter: Invert */
            if (g_filter_invert) {
                r = 255 - r;
                g = 255 - g;
                b = 255 - b;
            }

            /* Filter: Grayscale */
            if (g_filter_grayscale) {
                int luma = (r * 77 + g * 150 + b * 29) >> 8;
                r = g = b = luma;
            }

            /* Filter: Brightness */
            if (g_brightness != 0) {
                r += g_brightness;
                g += g_brightness;
                b += g_brightness;
                if (r < 0) { r = 0; } else if (r > 255) { r = 255; }
                if (g < 0) { g = 0; } else if (g > 255) { g = 255; }
                if (b < 0) { b = 0; } else if (b > 255) { b = 255; }
            }

            g_rot_pixels[y * g_rot_w + x] = ((unsigned int)a << 24) | ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
        }
    }
}

/* ── Reset Zoom & Center ─────────────────────────────────────────────────── */
static void reset_zoom_and_fit(void)
{
    if (g_rot_w <= 0 || g_rot_h <= 0) return;

    if (g_fit_mode) {
        float zx = (float)(WIN_W - 20) / (float)g_rot_w;
        float zy = (float)(CANVAS_H - 20) / (float)g_rot_h;
        g_zoom = (zx < zy) ? zx : zy;
        if (g_zoom > 1.0f) g_zoom = 1.0f;
    }

    int disp_w = (int)((float)g_rot_w * g_zoom);
    int disp_h = (int)((float)g_rot_h * g_zoom);
    g_pan_x = (WIN_W - disp_w) / 2;
    g_pan_y = CANVAS_Y + (CANVAS_H - disp_h) / 2;
}

/* ── Open Image File ──────────────────────────────────────────────────────── */
static bool open_image(const char *path)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    unsigned char *data = (unsigned char *)malloc(st.st_size);
    if (!data) {
        close(fd);
        return false;
    }

    ssize_t n = read(fd, data, st.st_size);
    close(fd);
    if (n != st.st_size) {
        free(data);
        return false;
    }

    int w = 0, h = 0;
    unsigned int *px = NULL;
    const char *fmt = "Unknown";

    if (data[0] == 'P' && (data[1] == '6' || data[1] == '3')) {
        px = load_ppm(data, (size_t)n, &w, &h);
        fmt = (data[1] == '6') ? "PPM (P6)" : "PPM (P3)";
    } else if (data[0] == 'B' && data[1] == 'M') {
        px = load_bmp(data, (size_t)n, &w, &h);
        fmt = "BMP";
    } else if (data[0] == 'q' && data[1] == 'o' && data[2] == 'i' && data[3] == 'f') {
        px = load_qoi(data, (size_t)n, &w, &h);
        fmt = "QOI";
    } else if (n >= 18 && (data[2] == 2 || data[2] == 10)) {
        px = load_tga(data, (size_t)n, &w, &h);
        fmt = "TGA";
    } else if (n == 4096) {
        px = load_icn(data, (size_t)n, &w, &h);
        fmt = "ICN (Icon)";
    }

    free(data);
    if (!px) return false;

    if (g_img_pixels) free(g_img_pixels);
    g_img_pixels = px;
    g_img_w = w;
    g_img_h = h;
    strncpy(g_img_format, fmt, sizeof(g_img_format) - 1);
    g_img_file_size = (size_t)st.st_size;

    strncpy(g_current_path, path, sizeof(g_current_path) - 1);
    g_rotation = 0;
    update_transformed_pixels();
    reset_zoom_and_fit();
    return true;
}

/* ── Scan Directory for Other Images ─────────────────────────────────────── */
static void scan_directory(const char *filepath)
{
    g_folder_count = 0;
    g_folder_index = -1;

    char dirpath[512];
    strncpy(dirpath, filepath, sizeof(dirpath) - 1);
    char *last_slash = strrchr(dirpath, '/');
    if (last_slash) {
        if (last_slash == dirpath) *(last_slash + 1) = '\0';
        else *last_slash = '\0';
    } else {
        strcpy(dirpath, ".");
    }

    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_folder_count < MAX_FOLDER_IMAGES) {
        if (de->d_name[0] == '.') continue;
        if (has_image_ext(de->d_name)) {
            char full[512];
            if (strcmp(dirpath, "/") == 0)
                snprintf(full, sizeof(full), "/%s", de->d_name);
            else if (strcmp(dirpath, ".") == 0)
                snprintf(full, sizeof(full), "%s", de->d_name);
            else
                snprintf(full, sizeof(full), "%s/%s", dirpath, de->d_name);

            strncpy(g_folder_images[g_folder_count], full, sizeof(g_folder_images[0]) - 1);
            if (strcmp(full, filepath) == 0) {
                g_folder_index = g_folder_count;
            }
            g_folder_count++;
        }
    }
    closedir(d);
}

/* ── Cycle to Prev / Next ─────────────────────────────────────────────────── */
static void cycle_image(int delta)
{
    if (g_folder_count <= 0) return;
    int next_idx = g_folder_index + delta;
    if (next_idx < 0) next_idx = g_folder_count - 1;
    if (next_idx >= g_folder_count) next_idx = 0;

    if (open_image(g_folder_images[next_idx])) {
        g_folder_index = next_idx;
    }
}

/* ── Set as Desktop Wallpaper ─────────────────────────────────────────────── */
static void set_as_wallpaper(void)
{
    if (strlen(g_current_path) == 0) return;
    int fd = open("/etc/desktop.conf", O_RDWR, 0644);
    if (fd >= 0) {
        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 256);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "wallpaper=");
            if (p) {
                char *nl = strchr(p, '\n');
                if (nl) {
                    char new_conf[1024];
                    size_t pre_len = (size_t)(p - buf + 10);
                    memcpy(new_conf, buf, pre_len);
                    int added = snprintf(new_conf + pre_len, sizeof(new_conf) - pre_len, "%s", g_current_path);
                    strcpy(new_conf + pre_len + added, nl);
                    lseek(fd, 0, SEEK_SET);
                    write(fd, new_conf, strlen(new_conf));
                    ftruncate(fd, strlen(new_conf));
                }
            }
        }
        close(fd);
    }
    /* Broadcast reload signal */
    az_wm_msg_t tmsg;
    memset(&tmsg, 0, sizeof(tmsg));
    tmsg.type = AZ_WM_SET_THEME;
    az_channel_send(SERVER_CHAN, (az_ipc_msg_t *)&tmsg);
}

/* ── Render Image to Window Canvas ────────────────────────────────────────── */
static void render_image_canvas(uk_window_t *win)
{
    uk_fill_rect(win, 0, CANVAS_Y, WIN_W, CANVAS_H, 0xFF11111B);

    int chk_sz = 16;
    for (int cy = CANVAS_Y; cy < CANVAS_Y + CANVAS_H; cy += chk_sz) {
        for (int cx = 0; cx < WIN_W; cx += chk_sz) {
            unsigned int chk_col = (((cx / chk_sz) + (cy / chk_sz)) % 2 == 0) ? 0xFF181825 : 0xFF11111B;
            int cw = (cx + chk_sz <= WIN_W) ? chk_sz : (WIN_W - cx);
            int ch = (cy + chk_sz <= CANVAS_Y + CANVAS_H) ? chk_sz : (CANVAS_Y + CANVAS_H - cy);
            uk_fill_rect(win, cx, cy, cw, ch, chk_col);
        }
    }

    if (!g_rot_pixels || g_rot_w <= 0 || g_rot_h <= 0) {
        const char *msg = "No Image Loaded. Open a .ppm, .bmp, .tga, .qoi, or .icn file.";
        int len = uk_strlen(msg);
        uk_draw_text(win, (WIN_W - len * 8) / 2, CANVAS_Y + CANVAS_H / 2 - 4, msg, UK_OVERLAY0);
        return;
    }

    int disp_w = (int)((float)g_rot_w * g_zoom);
    int disp_h = (int)((float)g_rot_h * g_zoom);
    if (disp_w <= 0) disp_w = 1;
    if (disp_h <= 0) disp_h = 1;

    int clip_x0 = 0;
    int clip_y0 = CANVAS_Y;
    int clip_x1 = WIN_W;
    int clip_y1 = CANVAS_Y + CANVAS_H;

    int dst_x0 = g_pan_x;
    int dst_y0 = g_pan_y;
    int dst_x1 = dst_x0 + disp_w;
    int dst_y1 = dst_y0 + disp_h;

    int render_x0 = (dst_x0 < clip_x0) ? clip_x0 : dst_x0;
    int render_y0 = (dst_y0 < clip_y0) ? clip_y0 : dst_y0;
    int render_x1 = (dst_x1 > clip_x1) ? clip_x1 : dst_x1;
    int render_y1 = (dst_y1 > clip_y1) ? clip_y1 : dst_y1;

    if (render_x0 >= render_x1 || render_y0 >= render_y1) return;

    float inv_zoom = 1.0f / g_zoom;
    unsigned int *dst_px = win->pixels;

    for (int y = render_y0; y < render_y1; y++) {
        int iy = (int)((float)(y - dst_y0) * inv_zoom);
        if (iy < 0) iy = 0;
        if (iy >= g_rot_h) iy = g_rot_h - 1;

        const unsigned int *src_row = g_rot_pixels + (size_t)iy * g_rot_w;
        unsigned int *dst_row = dst_px + (size_t)y * WIN_W;

        for (int x = render_x0; x < render_x1; x++) {
            int ix = (int)((float)(x - dst_x0) * inv_zoom);
            if (ix < 0) ix = 0;
            if (ix >= g_rot_w) ix = g_rot_w - 1;

            unsigned int col = src_row[ix];
            unsigned int a = (col >> 24) & 0xFF;

            if (a == 255) {
                dst_row[x] = col;
            } else if (a > 0) {
                unsigned int bg = dst_row[x];
                unsigned int br = (bg >> 16) & 0xFF;
                unsigned int bg_ = (bg >> 8) & 0xFF;
                unsigned int bb = bg & 0xFF;

                unsigned int sr = (col >> 16) & 0xFF;
                unsigned int sg = (col >> 8) & 0xFF;
                unsigned int sb = col & 0xFF;

                unsigned int dr = (sr * a + br * (255 - a)) / 255;
                unsigned int dg = (sg * a + bg_ * (255 - a)) / 255;
                unsigned int db = (sb * a + bb * (255 - a)) / 255;

                dst_row[x] = 0xFF000000 | (dr << 16) | (dg << 8) | db;
            }
        }
    }

    /* 1px border around image */
    if (dst_x0 >= 0 && dst_y0 >= CANVAS_Y) {
        uk_fill_rect(win, dst_x0, dst_y0, disp_w, 1, 0xFF313244);
        uk_fill_rect(win, dst_x0, dst_y0 + disp_h - 1, disp_w, 1, 0xFF313244);
        uk_fill_rect(win, dst_x0, dst_y0, 1, disp_h, 0xFF313244);
        uk_fill_rect(win, dst_x0 + disp_w - 1, dst_y0, 1, disp_h, 0xFF313244);
    }

    /* ── Info Overlay Card ── */
    if (g_show_info && g_img_pixels) {
        int card_w = 340;
        int card_h = 190;
        int card_x = (WIN_W - card_w) / 2;
        int card_y = CANVAS_Y + (CANVAS_H - card_h) / 2;

        uk_fill_rect(win, card_x, card_y, card_w, card_h, 0xFF181825);
        uk_fill_rect(win, card_x, card_y, card_w, 24, 0xFF313244);
        uk_draw_text(win, card_x + 12, card_y + 4, "IMAGE PROPERTIES", UK_MAUVE);

        const char *fname = strrchr(g_current_path, '/');
        fname = fname ? (fname + 1) : g_current_path;

        char buf[128];
        snprintf(buf, sizeof(buf), "Filename:  %s", fname);
        uk_draw_text(win, card_x + 16, card_y + 36, buf, UK_TEXT);

        snprintf(buf, sizeof(buf), "Format:    %s", g_img_format);
        uk_draw_text(win, card_x + 16, card_y + 56, buf, UK_SAPPHIRE);

        snprintf(buf, sizeof(buf), "Dimensions: %d x %d px", g_img_w, g_img_h);
        uk_draw_text(win, card_x + 16, card_y + 76, buf, UK_GREEN);

        snprintf(buf, sizeof(buf), "File Size:  %u KB (%zu bytes)", (unsigned int)(g_img_file_size / 1024), g_img_file_size);
        uk_draw_text(win, card_x + 16, card_y + 96, buf, UK_PEACH);

        snprintf(buf, sizeof(buf), "Zoom:       %d%% (Fit: %s)", (int)(g_zoom * 100.0f), g_fit_mode ? "Yes" : "No");
        uk_draw_text(win, card_x + 16, card_y + 116, buf, UK_TEXT);

        snprintf(buf, sizeof(buf), "Filters:    Invert:%s Gray:%s Bright:%+d",
                 g_filter_invert ? "ON" : "OFF", g_filter_grayscale ? "ON" : "OFF", g_brightness);
        uk_draw_text(win, card_x + 16, card_y + 136, buf, UK_YELLOW);

        uk_draw_text(win, card_x + 16, card_y + 162, "Press Tab or click Info to close", UK_SUBTEXT0);
    }
}

/* ── Render Toolbar & Status Bar ─────────────────────────────────────────── */
static void render_gui(uk_window_t *win)
{
    /* 1. Toolbar background */
    uk_gradient_v(win, 0, 0, WIN_W, TOOLBAR_H, 0xFF1E1E2E, 0xFF181825);
    uk_fill_rect(win, 0, TOOLBAR_H - 1, WIN_W, 1, 0xFF313244);

    /* Draw toolbar buttons */
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        const iv_button_t *btn = &g_buttons[i];
        int style = UK_BTN_NORMAL;
        if ((int)i == g_hovered_btn) style = UK_BTN_HOVER;
        if (btn->id == BTN_SLIDESHOW && g_slideshow) style = UK_BTN_PRESSED;
        if (btn->id == BTN_INVERT && g_filter_invert) style = UK_BTN_PRESSED;
        if (btn->id == BTN_GRAYSCALE && g_filter_grayscale) style = UK_BTN_PRESSED;

        uk_draw_button(win, btn->x, btn->y, btn->w, btn->h, btn->label, style);
    }

    /* Folder index indicator on right side of toolbar */
    if (g_folder_count > 0) {
        char idx_buf[32];
        snprintf(idx_buf, sizeof(idx_buf), "[ %d / %d ]", g_folder_index + 1, g_folder_count);
        int len = uk_strlen(idx_buf);
        uk_draw_text(win, WIN_W - len * 8 - 14, 14, idx_buf, UK_SUBTEXT0);
    }

    /* 2. Status Bar background */
    int stat_y = WIN_H - STATUS_H;
    uk_fill_rect(win, 0, stat_y, WIN_W, STATUS_H, 0xFF181825);
    uk_fill_rect(win, 0, stat_y, WIN_W, 1, 0xFF313244);

    /* Status Bar Info */
    if (g_img_pixels) {
        const char *fname = strrchr(g_current_path, '/');
        fname = fname ? (fname + 1) : g_current_path;

        char stat_str[180];
        int zoom_pct = (int)(g_zoom * 100.0f);
        snprintf(stat_str, sizeof(stat_str), "%s  |  %dx%d px  |  %s  |  %d%%  |  %u KB%s%s",
                 fname, g_rot_w, g_rot_h, g_img_format, zoom_pct, (unsigned int)(g_img_file_size / 1024),
                 g_filter_invert ? " | Invert" : "", g_filter_grayscale ? " | Gray" : "");
        uk_draw_text(win, 12, stat_y + 5, stat_str, UK_SUBTEXT0);

        if (g_slideshow) {
            uk_draw_text(win, WIN_W - 140, stat_y + 5, "[SLIDESHOW ON]", UK_GREEN);
        } else if (g_rotation != 0) {
            char rot_str[24];
            snprintf(rot_str, sizeof(rot_str), "(Rot: %d°)", g_rotation);
            uk_draw_text(win, WIN_W - 120, stat_y + 5, rot_str, UK_PEACH);
        }
    } else {
        uk_draw_text(win, 12, stat_y + 5, "Ready  |  AzamiOS Photo & Image Viewer", UK_OVERLAY0);
    }
}

static void redraw_all(uk_window_t *win)
{
    render_image_canvas(win);
    render_gui(win);
    uk_invalidate(win);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    const char *initial_file = NULL;
    if (argc >= 2) {
        initial_file = argv[1];
    } else {
        if (access("/home/azami/Pictures/sunset.ppm", R_OK) == 0)
            initial_file = "/home/azami/Pictures/sunset.ppm";
        else if (access("/usr/share/wallpapers/default.ppm", R_OK) == 0)
            initial_file = "/usr/share/wallpapers/default.ppm";
    }

    if (uk_window_connect(&g_win, "Azami Image Viewer", 100, 80, INIT_W, INIT_H, MAP_ADDR, SERVER_CHAN) < 0) {
        fprintf(stderr, "Failed to create Image Viewer window\n");
        return 1;
    }

    if (initial_file) {
        if (open_image(initial_file)) {
            scan_directory(initial_file);
        }
    }

    az_set_timer(g_win.client_chan, 100, 0);

    redraw_all(&g_win);

    bool running = true;
    while (running) {
        az_wm_msg_t msg;
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r < 0) break;
        if (r != 0) continue;

        if (msg.type == AZ_WM_WINDOW_RESIZED) {
            if (!uk_handle_resize(&g_win, &msg)) break;
            reset_zoom_and_fit();   /* re-fit the image to the new canvas */
            redraw_all(&g_win);
            continue;
        }

        if (msg.type == AZ_WM_DESTROY_WINDOW) {
            break;
        }

        if (msg.type == AZ_WM_TIMER_TICK) {
            if (g_slideshow && g_folder_count > 1) {
                g_slideshow_ticks++;
                if (g_slideshow_ticks >= 30) { /* 30 * 100ms = 3.0 seconds */
                    g_slideshow_ticks = 0;
                    cycle_image(1);
                    redraw_all(&g_win);
                }
            }
            continue;
        }

        if (msg.type == AZ_WM_MOUSE_EVENT) {
            int mx = msg.mouse.abs_x;
            int my = msg.mouse.abs_y;
            unsigned int btns = msg.mouse.buttons;

            int old_hov = g_hovered_btn;
            g_hovered_btn = -1;
            if (my < TOOLBAR_H) {
                for (size_t i = 0; i < NUM_BUTTONS; i++) {
                    if (mx >= g_buttons[i].x && mx < g_buttons[i].x + g_buttons[i].w &&
                        my >= g_buttons[i].y && my < g_buttons[i].y + g_buttons[i].h) {
                        g_hovered_btn = (int)i;
                        break;
                    }
                }
            }

            if (old_hov != g_hovered_btn) {
                render_gui(&g_win);
                uk_invalidate(&g_win);
            }

            if ((btns & 1) && !g_dragging) {
                if (my < TOOLBAR_H && g_hovered_btn >= 0) {
                    int btn_id = g_buttons[g_hovered_btn].id;
                    switch (btn_id) {
                    case BTN_PREV: cycle_image(-1); break;
                    case BTN_NEXT: cycle_image(1); break;
                    case BTN_ZOOM_OUT:
                        g_fit_mode = false;
                        g_zoom *= 0.8f;
                        if (g_zoom < 0.1f) g_zoom = 0.1f;
                        reset_zoom_and_fit();
                        break;
                    case BTN_ZOOM_IN:
                        g_fit_mode = false;
                        g_zoom *= 1.25f;
                        if (g_zoom > 16.0f) g_zoom = 16.0f;
                        reset_zoom_and_fit();
                        break;
                    case BTN_ZOOM_100:
                        g_fit_mode = false;
                        g_zoom = 1.0f;
                        reset_zoom_and_fit();
                        break;
                    case BTN_ZOOM_FIT:
                        g_fit_mode = true;
                        reset_zoom_and_fit();
                        break;
                    case BTN_ROTATE:
                        g_rotation = (g_rotation + 90) % 360;
                        update_transformed_pixels();
                        reset_zoom_and_fit();
                        break;
                    case BTN_SLIDESHOW:
                        g_slideshow = !g_slideshow;
                        g_slideshow_ticks = 0;
                        break;
                    case BTN_INVERT:
                        g_filter_invert = !g_filter_invert;
                        update_transformed_pixels();
                        break;
                    case BTN_GRAYSCALE:
                        g_filter_grayscale = !g_filter_grayscale;
                        update_transformed_pixels();
                        break;
                    case BTN_WALLPAPER:
                        set_as_wallpaper();
                        break;
                    case BTN_INFO:
                        g_show_info = !g_show_info;
                        break;
                    }
                    redraw_all(&g_win);
                } else if (my >= CANVAS_Y && my < CANVAS_Y + CANVAS_H) {
                    if (g_show_info) {
                        g_show_info = false;
                        redraw_all(&g_win);
                    } else {
                        g_dragging = true;
                        g_drag_start_mx = mx;
                        g_drag_start_my = my;
                        g_drag_orig_pan_x = g_pan_x;
                        g_drag_orig_pan_y = g_pan_y;
                    }
                }
            }

            if (g_dragging) {
                if (btns & 1) {
                    int dx = mx - g_drag_start_mx;
                    int dy = my - g_drag_start_my;
                    g_pan_x = g_drag_orig_pan_x + dx;
                    g_pan_y = g_drag_orig_pan_y + dy;
                    redraw_all(&g_win);
                } else {
                    g_dragging = false;
                }
            }
        } else if (msg.type == AZ_WM_KEY_EVENT) {
            if (msg.key.pressed) {
                int k = msg.key.keycode;
                if (k == 0x1B || k == 'q' || k == 'Q') {
                    break;
                } else if (k == 0x4B || k == 'a') {
                    cycle_image(-1);
                    redraw_all(&g_win);
                } else if (k == 0x4D || k == 'd') {
                    cycle_image(1);
                    redraw_all(&g_win);
                } else if (k == '+' || k == '=') {
                    g_fit_mode = false;
                    g_zoom *= 1.25f;
                    reset_zoom_and_fit();
                    redraw_all(&g_win);
                } else if (k == '-' || k == '_') {
                    g_fit_mode = false;
                    g_zoom *= 0.8f;
                    reset_zoom_and_fit();
                    redraw_all(&g_win);
                } else if (k == 'f' || k == 'F') {
                    g_fit_mode = true;
                    reset_zoom_and_fit();
                    redraw_all(&g_win);
                } else if (k == '1') {
                    g_fit_mode = false;
                    g_zoom = 1.0f;
                    reset_zoom_and_fit();
                    redraw_all(&g_win);
                } else if (k == 'r' || k == 'R') {
                    g_rotation = (g_rotation + 90) % 360;
                    update_transformed_pixels();
                    reset_zoom_and_fit();
                    redraw_all(&g_win);
                } else if (k == 's' || k == ' ') {
                    g_slideshow = !g_slideshow;
                    g_slideshow_ticks = 0;
                    redraw_all(&g_win);
                } else if (k == 'i') {
                    g_filter_invert = !g_filter_invert;
                    update_transformed_pixels();
                    redraw_all(&g_win);
                } else if (k == 'g') {
                    g_filter_grayscale = !g_filter_grayscale;
                    update_transformed_pixels();
                    redraw_all(&g_win);
                } else if (k == '[') {
                    g_brightness -= 15;
                    if (g_brightness < -120) g_brightness = -120;
                    update_transformed_pixels();
                    redraw_all(&g_win);
                } else if (k == ']') {
                    g_brightness += 15;
                    if (g_brightness > 120) g_brightness = 120;
                    update_transformed_pixels();
                    redraw_all(&g_win);
                } else if (k == 'w' || k == 'W') {
                    set_as_wallpaper();
                } else if (k == '\t') {
                    g_show_info = !g_show_info;
                    redraw_all(&g_win);
                }
            }
        }
    }

    if (g_img_pixels) free(g_img_pixels);
    if (g_rot_pixels) free(g_rot_pixels);
    uk_window_destroy(&g_win);
    return 0;
}
