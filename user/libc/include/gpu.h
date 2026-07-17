/**
 * include/gpu.h  –  AzamiOS Ring-3 Userspace Unified GPU Driver Interface
 */
#ifndef _USER_GPU_H
#define _USER_GPU_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    GPU_VENDOR_NONE = 0,
    GPU_VENDOR_BOCHS = 0x1234,
    GPU_VENDOR_INTEL = 0x8086,
    GPU_VENDOR_AMD   = 0x1002,
    GPU_VENDOR_AMD_APU = 0x1022
} gpu_vendor_t;

typedef struct {
    gpu_vendor_t vendor;
    uint16_t     device_id;
    uint8_t      bus;
    uint8_t      slot;
    uint8_t      func;
    uint32_t     bar0; /* MMIO Registers base */
    uint32_t     bar2; /* Linear Framebuffer / Aperture base */
    const char*  name;
    bool         active;
} gpu_device_t;

void gpu_init(void);
const gpu_device_t* gpu_get_active_device(void);
const char* gpu_get_name(void);

#endif /* _USER_GPU_H */
