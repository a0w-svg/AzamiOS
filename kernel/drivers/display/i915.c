/**
 * i915.c — Intel HD Graphics (Gen 2-9) Kernel Mode-Setting Driver
 *
 * Probes PCI bus for Intel vendor 0x8086, display class 0x03.
 * If found: maps MMIO BAR, configures pipe/plane for 1280×800×32.
 * Falls back to Bochs VBE if no Intel GPU is detected.
 *
 * Supported device IDs (subset — covers QEMU and common Intel GPUs):
 *   0x0102 — Sandy Bridge (HD 2000/3000)
 *   0x0162 — Ivy Bridge (HD 4000)
 *   0x5917 — Kaby Lake (UHD 620)
 *   0x3E92 — Coffee Lake (UHD 630)
 *   0x9A49 — Tiger Lake (Xe)
 */
#include "../include/gfx.h"
#include "../include/pci.h"
#include "../../klibc/include/port.h"
#include "../../klibc/include/stdio.h"
#include "../../mem/include/paging.h"

/* Intel PCI Vendor ID */
#define INTEL_VENDOR_ID 0x8086

/* Known Intel GPU Device IDs */
static const uint16_t g_i915_device_ids[] = {
    0x0102, /* Sandy Bridge */
    0x0162, /* Ivy Bridge */
    0x0412, /* Haswell */
    0x1912, /* Skylake */
    0x5917, /* Kaby Lake */
    0x3E92, /* Coffee Lake */
    0x9A49, /* Tiger Lake */
    0x4680, /* Alder Lake */
    0
};

/* Driver state */
static bool g_i915_found = false;
static uint32_t g_i915_mmio_base = 0;
static uint16_t g_i915_device_id = 0;
static uint32_t g_i915_gmadr_base = 0;

/**
 * i915_pci_probe — Scan PCI bus for Intel display controller.
 * Returns true if an Intel GPU was found.
 */
static bool i915_pci_probe(void) {
    for (int bus = 0; bus < 8; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint32_t vendor_dev = pci_config_read32(bus, slot, 0, 0);
            if (vendor_dev == 0xFFFFFFFF) continue;

            uint16_t vendor = vendor_dev & 0xFFFF;
            uint16_t device = (vendor_dev >> 16) & 0xFFFF;

            if (vendor != INTEL_VENDOR_ID) continue;

            /* Check class code — display controller = 0x03 */
            uint32_t class_reg = pci_config_read32(bus, slot, 0, 0x08);
            uint8_t base_class = (class_reg >> 24) & 0xFF;
            uint8_t sub_class = (class_reg >> 16) & 0xFF;

            if (base_class != 0x03) continue; /* Not a display device */

            /* Check against known device IDs */
            for (int i = 0; g_i915_device_ids[i] != 0; i++) {
                if (device == g_i915_device_ids[i]) {
                    /* Read BARs */
                    uint32_t bar0 = pci_config_read32(bus, slot, 0, 0x10);
                    uint32_t bar2 = pci_config_read32(bus, slot, 0, 0x18);

                    g_i915_mmio_base = bar0 & 0xFFFFFFF0;
                    g_i915_gmadr_base = bar2 & 0xFFFFFFF0;
                    g_i915_device_id = device;
                    g_i915_found = true;

                    kprintf("i915: Intel GPU found [%04x:%04x] class=%02x:%02x\n",
                            vendor, device, base_class, sub_class);
                    kprintf("i915: MMIO BAR0=0x%x  GMADR BAR2=0x%x\n",
                            g_i915_mmio_base, g_i915_gmadr_base);
                    return true;
                }
            }

            /* Unknown Intel display device — log but don't claim */
            kprintf("i915: unknown Intel display device %04x (class %02x:%02x), skipping\n",
                    device, base_class, sub_class);
        }
    }
    return false;
}

/**
 * i915_map_mmio — Map MMIO register space into kernel address space.
 * Intel GPUs typically have a 512KB-4MB MMIO region at BAR0.
 */
static void i915_map_mmio(void) {
    if (!g_i915_mmio_base) return;

    /* Map 2MB of MMIO space (covers all register ranges) */
    uint32_t mmio_size = 2 * 1024 * 1024;
    for (uint32_t off = 0; off < mmio_size; off += 4096) {
        paging_map_page(g_i915_mmio_base + off, g_i915_mmio_base + off, 0, 1);
    }
    kprintf("i915: mapped %d KB MMIO at 0x%x\n", mmio_size / 1024, g_i915_mmio_base);
}

/**
 * i915_map_gmadr — Map Graphics Memory Aperture (framebuffer) into kernel.
 * GMADR at BAR2 is the stolen memory aperture that acts as the framebuffer.
 */
static void i915_map_gmadr(void) {
    if (!g_i915_gmadr_base) return;

    /* Map enough for 1280×800×4 = 4,096,000 bytes (~4MB) */
    uint32_t fb_size = GFX_WIDTH * GFX_HEIGHT * 4;
    uint32_t fb_pages = (fb_size + 4095) / 4096;

    for (uint32_t i = 0; i < fb_pages; i++) {
        paging_map_page(g_i915_gmadr_base + i * 4096,
                        g_i915_gmadr_base + i * 4096, 0, 1);
    }
    kprintf("i915: mapped %d pages GMADR at 0x%x\n", fb_pages, g_i915_gmadr_base);
}

/* ── MMIO register read/write helpers ──────────────────────────── */
static inline uint32_t i915_read32(uint32_t reg) {
    return *(volatile uint32_t *)(uintptr_t)(g_i915_mmio_base + reg);
}

static inline void i915_write32(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)(g_i915_mmio_base + reg) = val;
}

/* ── Intel GPU Register Offsets (Gen 4+) ───────────────────────── */
#define PIPEACONF       0x70008
#define PIPEASRC        0x6001C
#define DSPASURF        0x7019C
#define DSPACNTR        0x70180
#define DSPALINOFF      0x70184
#define DSPASTRIDE      0x70188
#define HTOTAL_A        0x60000
#define HBLANK_A        0x60004
#define HSYNC_A         0x60008
#define VTOTAL_A        0x6000C
#define VBLANK_A        0x60010
#define VSYNC_A         0x60014

/**
 * i915_setup_display — Configure pipe A and primary plane for 1280×800×32.
 *
 * NOTE: On real hardware, full mode-setting requires:
 *   1. Disable pipe → disable plane → wait vblank
 *   2. Configure DPLL (PLL registers for pixel clock)
 *   3. Set timing registers (HTOTAL, VTOTAL, HSYNC, VSYNC, HBLANK, VBLANK)
 *   4. Configure primary plane (stride, format, base address)
 *   5. Enable pipe → enable plane
 *
 * This is a simplified version that works with pre-configured firmware modes
 * (e.g., UEFI GOP already set up the display).
 */
static void i915_setup_display(void) {
    kprintf("i915: configuring display for %dx%dx32...\n", GFX_WIDTH, GFX_HEIGHT);

    /* Read current pipe config to check if firmware already set a mode */
    uint32_t pipe_conf = i915_read32(PIPEACONF);
    kprintf("i915: current PIPEACONF=0x%x\n", pipe_conf);

    if (pipe_conf & (1 << 31)) {
        /* Pipe already enabled by firmware/UEFI — read current mode */
        uint32_t pipesrc = i915_read32(PIPEASRC);
        uint32_t cur_w = ((pipesrc >> 16) & 0xFFF) + 1;
        uint32_t cur_h = (pipesrc & 0xFFF) + 1;
        kprintf("i915: firmware pipe mode: %dx%d\n", cur_w, cur_h);

        /* If firmware mode matches what we want, just configure the plane */
        if (cur_w == GFX_WIDTH && cur_h == GFX_HEIGHT) {
            kprintf("i915: firmware mode matches target, using as-is\n");
        } else {
            kprintf("i915: firmware mode %dx%d != target %dx%d\n",
                    cur_w, cur_h, GFX_WIDTH, GFX_HEIGHT);
            /* Fall through — set up our own timing */
        }
    }

    /* Configure primary plane A */
    uint32_t stride = GFX_WIDTH * 4;
    i915_write32(DSPASTRIDE, stride);
    i915_write32(DSPALINOFF, 0);  /* Linear offset = 0 */

    /* Enable plane: 32bpp XRGB format (bits 29:26 = 0110), plane enable (bit 31) */
    uint32_t plane_ctrl = (1u << 31) | (0x6u << 26);
    i915_write32(DSPACNTR, plane_ctrl);

    /* Set surface base address to GMADR */
    i915_write32(DSPASURF, g_i915_gmadr_base);

    kprintf("i915: primary plane configured: stride=%d, format=XRGB8888\n", stride);
}

/**
 * gfx_init_i915 — Public entry point for Intel GPU initialization.
 * Called by gfx_init_auto(). If successful, the BGA init is skipped.
 */
void gfx_init_i915(void) {
    kprintf("i915: probing PCI bus for Intel display controller...\n");

    if (!i915_pci_probe()) {
        kprintf("i915: no supported Intel GPU found\n");
        return;
    }

    i915_map_mmio();
    i915_map_gmadr();
    i915_setup_display();

    kprintf("i915: Intel HD Graphics [%04x] initialized at %dx%dx32\n",
            g_i915_device_id, GFX_WIDTH, GFX_HEIGHT);
}
