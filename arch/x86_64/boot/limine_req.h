/* ============================================================================
 * AzamiOS — Limine Boot Protocol Wrapper
 * File: arch/x86_64/boot/limine_req.h
 *
 * Provides a thin wrapper around the official Limine header.
 * The kernel reads boot information through the Limine "request" mechanism:
 * static volatile structures placed in the .limine_requests section that the
 * bootloader fills in before transferring control to az_boot_entry().
 *
 * Supported requests used by AzamiOS:
 *   - Framebuffer      — linear framebuffer address, width, height, pitch, bpp
 *   - HHDM             — higher-half direct map base virtual address
 *   - Memory Map       — system memory layout (used by PMM)
 *   - RSDP             — ACPI Root System Description Pointer
 *   - Boot Time        — real-time clock value at boot
 *   - Kernel Address   — physical and virtual base of the loaded kernel ELF
 *   - SMP              — number of CPUs and their LAPIC IDs
 *   - Modules          — boot modules (initrd.tar)
 * ============================================================================ */
#pragma once

/* The official Limine header is included verbatim as a freestanding file.
 * It lives in arch/x86_64/boot/limine.h (copied from the Limine release). */
#include "limine.h"

/* --------------------------------------------------------------------------
 * Helper macro: declare a Limine request in the correct linker section with
 * the correct alignment so the bootloader can find it.
 * NOTE: NOT static — the objects are declared extern in this header so other
 * TUs can reference them without causing multiple-definition errors.
 * -------------------------------------------------------------------------- */
#define LIMINE_REQUEST(name)  \
    __attribute__((used, section(".limine_requests"))) \
    volatile name

/* --------------------------------------------------------------------------
 * Extern declarations — actual request objects live in limine_req.c
 * (one translation unit owns the storage; everyone else uses these externs).
 * -------------------------------------------------------------------------- */
extern volatile struct limine_framebuffer_request    g_limine_fb_req;
extern volatile struct limine_hhdm_request           g_limine_hhdm_req;
extern volatile struct limine_memmap_request         g_limine_memmap_req;
extern volatile struct limine_rsdp_request           g_limine_rsdp_req;
extern volatile struct limine_kernel_address_request g_limine_kaddr_req;
extern volatile struct limine_smp_request            g_limine_smp_req;
extern volatile struct limine_module_request         g_limine_module_req;
extern volatile struct limine_boot_time_request      g_limine_btime_req;

/* Convenience accessors — return NULL on failure (bootloader did not fill). */
static inline struct limine_framebuffer *az_boot_framebuffer(void) {
    if (!g_limine_fb_req.response || g_limine_fb_req.response->framebuffer_count == 0)
        return NULL;
    return g_limine_fb_req.response->framebuffers[0];
}

static inline uint64_t az_boot_hhdm_base(void) {
    if (!g_limine_hhdm_req.response) return 0;
    return g_limine_hhdm_req.response->offset;
}

static inline struct limine_memmap_response *az_boot_memmap(void) {
    return (struct limine_memmap_response *)g_limine_memmap_req.response;
}

static inline void *az_boot_rsdp(void) {
    if (!g_limine_rsdp_req.response) return NULL;
    return g_limine_rsdp_req.response->address;
}

static inline struct limine_smp_response *az_boot_smp(void) {
    return (struct limine_smp_response *)g_limine_smp_req.response;
}

static inline struct limine_file *az_boot_initrd(void) {
    struct limine_module_response *r =
        (struct limine_module_response *)g_limine_module_req.response;
    if (!r || r->module_count == 0) return NULL;
    return r->modules[0];
}
