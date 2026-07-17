/**
 * amd_gpu.c  –  AzamiOS Ring-3 AMD Radeon Graphics Driver Implementation
 */
#include "amd_gpu.h"
#include "port.h"
#include <stdio.h>


#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t addr = (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) | ((uint32_t)func << 8) | (offset & 0xFC);
    sys_outl(PCI_CONFIG_ADDRESS, addr);
    return sys_inl(PCI_CONFIG_DATA);
}

static const char* get_amd_device_name(uint16_t dev_id) {
    switch (dev_id) {
        case 0x67DF: return "AMD Radeon RX 580 / 570 (Polaris)";
        case 0x73BF: return "AMD Radeon RX 6800 / 6900 XT (Navi 21)";
        case 0x1636: return "AMD Renoir Radeon Vega APU";
        case 0x5159: return "AMD Radeon HD 4850 / RV770";
        default: return "AMD Radeon Display Adapter";
    }
}

bool amd_gpu_probe(gpu_device_t *dev) {
    if (!dev) return false;
    for (uint8_t bus = 0; bus < 8; bus++) {
        if (bus > 0 && pci_read32(bus, 0, 0, 0) == 0xFFFFFFFF) break;
        for (uint8_t slot = 0; slot < 32; slot++) {
            for (uint8_t func = 0; func < 8; func++) {
                uint32_t id = pci_read32(bus, slot, func, 0);
                uint16_t vendor = (uint16_t)(id & 0xFFFF);
                if (vendor == 0x1002 || vendor == 0x1022) {
                    uint32_t class_rev = pci_read32(bus, slot, func, 0x08);
                    uint8_t base_class = (class_rev >> 24) & 0xFF;
                    uint8_t sub_class  = (class_rev >> 16) & 0xFF;
                    if (base_class == 0x03 || (base_class == 0x00 && sub_class == 0x01)) {
                        uint16_t dev_id = (uint16_t)(id >> 16);
                        dev->vendor = (vendor == 0x1002) ? GPU_VENDOR_AMD : GPU_VENDOR_AMD_APU;
                        dev->device_id = dev_id;
                        dev->bus = bus;
                        dev->slot = slot;
                        dev->func = func;
                        dev->bar0 = pci_read32(bus, slot, func, 0x10) & 0xFFFFFFF0;
                        dev->bar2 = pci_read32(bus, slot, func, 0x18) & 0xFFFFFFF0;
                        dev->name = get_amd_device_name(dev_id);
                        dev->active = true;
                        return true;
                    }
                }
                if ((id & 0xFFFF) == 0xFFFF) break;
            }
        }
    }
    return false;
}
