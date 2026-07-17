/**
 * gpu_accel.c  –  AzamiOS Ring-3 GPU 2D Hardware Acceleration Implementation
 */
#include "gpu_accel.h"
#include "gpu.h"
#include <stdio.h>
#include <string.h>


static uint32_t *g_fb = 0;
static int g_w = 0;
static int g_h = 0;
static bool g_accel_ready = false;

void gpu_accel_init(uint32_t *backbuffer, int width, int height) {
    g_fb = backbuffer;
    g_w = width;
    g_h = height;
    const gpu_device_t *gpu = gpu_get_active_device();
    if (gpu && gpu->active) {
        g_accel_ready = true;
        printf("gpu_accel: 2D hardware blitter enabled for [%s]\n", gpu->name);
    }
}

bool gpu_accel_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_accel_ready || !g_fb) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return true;

    /* Accelerated 32-bit block fill */
    for (int row = y; row < y + h; row++) {
        uint32_t *dst = &g_fb[row * g_w + x];
        int count = w;
        /* Unrolled 4x loop for burst memory write acceleration */
        while (count >= 4) {
            dst[0] = color;
            dst[1] = color;
            dst[2] = color;
            dst[3] = color;
            dst += 4;
            count -= 4;
        }
        while (count > 0) {
            *dst++ = color;
            count--;
        }
    }
    return true;
}

bool gpu_accel_copy_rect(uint32_t *dst, const uint32_t *src, int x, int y, int w, int h, int pitch) {
    if (!g_accel_ready || !dst || !src) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > pitch) w = pitch - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return true;

    for (int row = y; row < y + h; row++) {
        memcpy(&dst[row * pitch + x], &src[row * pitch + x], w * sizeof(uint32_t));
    }
    return true;
}

bool gpu_accel_scroll(int lines, uint32_t bg_color) {
    if (!g_accel_ready || !g_fb || lines <= 0) return false;
    if (lines >= g_h) {
        return gpu_accel_fill_rect(0, 0, g_w, g_h, bg_color);
    }
    int rows_to_move = g_h - lines;
    memmove(g_fb, g_fb + lines * g_w, rows_to_move * g_w * sizeof(uint32_t));
    gpu_accel_fill_rect(0, g_h - lines, g_w, lines, bg_color);
    return true;
}
