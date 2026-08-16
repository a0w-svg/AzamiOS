/* ============================================================================
 * AzamiOS — Limine Request Storage
 * File: arch/x86_64/boot/limine_req.c
 *
 * This is the ONLY translation unit that defines the Limine request objects.
 * All other files use the extern declarations in limine_req.h.
 *
 * The Limine bootloader scans the kernel ELF for objects in the
 * .limine_requests section whose magic fields match known request IDs.
 * It then fills in the .response pointer before calling az_boot_entry().
 * ============================================================================ */

#include "limine_req.h"

/* ── Limine base protocol revision request (mandatory) ─────────────────────
 * Tells Limine which revision of the base protocol we expect.
 * Revision 3 supports Limine v7+. */
LIMINE_REQUEST(struct limine_base_revision) g_limine_base_rev = {
    LIMINE_BASE_REVISION(3)
};

/* ── Framebuffer ─────────────────────────────────────────────────────────── */
LIMINE_REQUEST(struct limine_framebuffer_request) g_limine_fb_req = {
    .id       = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0,
};

/* ── Higher-Half Direct Map ──────────────────────────────────────────────── */
LIMINE_REQUEST(struct limine_hhdm_request) g_limine_hhdm_req = {
    .id       = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

/* ── Memory Map ──────────────────────────────────────────────────────────── */
LIMINE_REQUEST(struct limine_memmap_request) g_limine_memmap_req = {
    .id       = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

/* ── RSDP (ACPI) ─────────────────────────────────────────────────────────── */
LIMINE_REQUEST(struct limine_rsdp_request) g_limine_rsdp_req = {
    .id       = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

/* ── Kernel Physical / Virtual Load Address ──────────────────────────────── */
LIMINE_REQUEST(struct limine_kernel_address_request) g_limine_kaddr_req = {
    .id       = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

/* ── SMP (Symmetric Multi-Processing) ───────────────────────────────────── */
LIMINE_REQUEST(struct limine_smp_request) g_limine_smp_req = {
    .id       = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags    = 0,  /* bit 0 = prefer X2APIC; leave 0 for xAPIC compat */
};

/* ── Boot Modules (initrd) ───────────────────────────────────────────────── */
LIMINE_REQUEST(struct limine_module_request) g_limine_module_req = {
    .id            = LIMINE_MODULE_REQUEST,
    .revision      = 0,
    .internal_module_count = 0,
    .internal_modules      = NULL,
};

/* ── Boot Time (UTC seconds since epoch) ─────────────────────────────────── */
LIMINE_REQUEST(struct limine_boot_time_request) g_limine_btime_req = {
    .id       = LIMINE_BOOT_TIME_REQUEST,
    .revision = 0,
};
