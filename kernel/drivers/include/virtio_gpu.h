#ifndef VIRTIO_GPU_H
#define VIRTIO_GPU_H

#include <stdint.h>
#include <stdbool.h>

bool     virtio_gpu_init(void);
bool     virtio_gpu_is_active(void);
void     virtio_gpu_blit(const uint32_t *pixels, uint32_t w, uint32_t h);
void     virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);

#endif
