/**
 * gpu.c  –  AzamiOS Ring-3 Userspace Unified GPU Driver Subsystem
 */
#include "gpu.h"
#include "intel_gpu.h"
#include "amd_gpu.h"
#include "port.h"
#include <stdio.h>
#include <string.h>


#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static gpu_device_t g_active_gpu;

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    sys_outl(PCI_CONFIG_ADDRESS, addr);
    return sys_inl(PCI_CONFIG_DATA);
}

static bool bochs_gpu_probe(gpu_device_t *dev) {
    if (!dev) return false;
    for (uint8_t bus = 0; bus < 8; bus++) {
        if (bus > 0 && pci_read32(bus, 0, 0, 0) == 0xFFFFFFFF) break;
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0);
                if (id == 0x11111234) {
                    dev->vendor = GPU_VENDOR_BOCHS;
                    dev->device_id = 0x1111;
                    dev->bus = bus;
                    dev->slot = slot;
                    dev->func = func;
                    dev->bar0 = pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0;
                    dev->bar2 = 0;
                    dev->name = "Bochs VBE Linear Framebuffer";
                    dev->active = true;
                    return true;
                }
                if ((id & 0xFFFF) == 0xFFFF) break;
            }
        }
    }
    return false;
}

void gpu_init(void) {
    memset(&g_active_gpu, 0, sizeof(g_active_gpu));

    /* Attempt Intel Graphics Probing */
    if (intel_gpu_probe(&g_active_gpu)) {
        return;
    }

    /* Attempt AMD Radeon Graphics Probing */
    if (amd_gpu_probe(&g_active_gpu)) {
        return;
    }

    /* Fallback to Bochs VBE LFB */
    if (bochs_gpu_probe(&g_active_gpu)) {
        return;
    }

    /* Generic default */
    g_active_gpu.name = "Standard VGA / Framebuffer Display";
    g_active_gpu.active = true;
}

const gpu_device_t* gpu_get_active_device(void) {
    return &g_active_gpu;
}

const char* gpu_get_name(void) {
    return g_active_gpu.name ? g_active_gpu.name : "Unknown GPU";
}
