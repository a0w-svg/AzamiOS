/**
 * include/gpu_accel.h  –  AzamiOS Ring-3 GPU 2D Hardware Acceleration Header
 */
#ifndef _USER_GPU_ACCEL_H
#define _USER_GPU_ACCEL_H

#include <stdint.h>
#include <stdbool.h>

void gpu_accel_init(uint32_t *backbuffer, int width, int height);
bool gpu_accel_fill_rect(int x, int y, int w, int h, uint32_t color);
bool gpu_accel_copy_rect(uint32_t *dst, const uint32_t *src, int x, int y, int w, int h, int pitch);
bool gpu_accel_scroll(int lines, uint32_t bg_color);

#endif /* _USER_GPU_ACCEL_H */
