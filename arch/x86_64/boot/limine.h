/* ============================================================================
 * Limine Boot Protocol Header — Revision 3 (v8+ compatible)
 * File: arch/x86_64/boot/limine.h
 *
 * Self-contained subset of the official Limine protocol header.
 * Only the requests used by AzamiOS are declared here.
 * Source: https://github.com/limine-bootloader/limine/blob/v9.x-branch-binary/limine.h
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Base revision ────────────────────────────────────────────────────────── */
#define LIMINE_BASE_REVISION(n) \
    { 0xf9562b2d5c95a6c8ULL, 0x6a7b384944536bdcULL, (n) }

#define LIMINE_BASE_REVISION_SUPPORTED \
    (g_limine_base_rev.revision[2] == 0)

struct limine_base_revision {
    uint64_t revision[3];
};

/* ── UUID / magic helper ──────────────────────────────────────────────────── */
#define LIMINE_COMMON_MAGIC  0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

/* ── Framebuffer ─────────────────────────────────────────────────────────── */
#define LIMINE_FRAMEBUFFER_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL }

struct limine_framebuffer {
    void     *address;
    uint64_t  width;
    uint64_t  height;
    uint64_t  pitch;
    uint16_t  bpp;
    uint8_t   memory_model;
    uint8_t   red_mask_size;
    uint8_t   red_mask_shift;
    uint8_t   green_mask_size;
    uint8_t   green_mask_shift;
    uint8_t   blue_mask_size;
    uint8_t   blue_mask_shift;
    uint8_t   unused[7];
    uint64_t  edid_size;
    void     *edid;
    /* v2+ */
    uint64_t  mode_count;
    void    **modes;
};

struct limine_framebuffer_response {
    uint64_t                  revision;
    uint64_t                  framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

/* ── HHDM ─────────────────────────────────────────────────────────────────── */
#define LIMINE_HHDM_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL }

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

/* ── Memory map ───────────────────────────────────────────────────────────── */
#define LIMINE_MEMMAP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL }

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_KERNEL_AND_MODULES     6
#define LIMINE_MEMMAP_FRAMEBUFFER            7

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t                   revision;
    uint64_t                   entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

/* ── RSDP ─────────────────────────────────────────────────────────────────── */
#define LIMINE_RSDP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL }

struct limine_rsdp_response {
    uint64_t  revision;
    void     *address;
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response *response;
};

/* ── Kernel address ───────────────────────────────────────────────────────── */
#define LIMINE_KERNEL_ADDRESS_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x71ba76863cc55f63ULL, 0xb2644a48c516a487ULL }

struct limine_kernel_address_response {
    uint64_t revision;
    uint64_t physical_base;
    uint64_t virtual_base;
};

struct limine_kernel_address_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_kernel_address_response *response;
};

/* ── SMP ──────────────────────────────────────────────────────────────────── */
#define LIMINE_SMP_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x95a67b819a1b857eULL, 0xa0b61b723b6a73e0ULL }

struct limine_smp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    void   (*goto_address)(struct limine_smp_info *);
    uint64_t extra_argument;
};

struct limine_smp_response {
    uint64_t                revision;
    uint32_t                flags;
    uint32_t                bsp_lapic_id;
    uint64_t                cpu_count;
    struct limine_smp_info **cpus;
};

struct limine_smp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_smp_response *response;
    uint64_t flags;
};

/* ── Modules ──────────────────────────────────────────────────────────────── */
#define LIMINE_MODULE_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x3e7e279702be32afULL, 0xca1c4f3bd1280ceeULL }

struct limine_file {
    uint64_t  revision;
    void     *address;
    uint64_t  size;
    char     *path;
    char     *cmdline;
    uint32_t  media_type;
    uint32_t  unused;
    uint32_t  tftp_ip;
    uint32_t  tftp_port;
    uint32_t  partition_index;
    uint32_t  mbr_disk_id;
    uint8_t   gpt_disk_uuid[16];
    uint8_t   gpt_part_uuid[16];
    uint8_t   part_uuid[16];
};

struct limine_module_response {
    uint64_t           revision;
    uint64_t           module_count;
    struct limine_file **modules;
};

struct limine_internal_module {
    const char *path;
    const char *cmdline;
    uint64_t    flags;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_module_response *response;
    uint64_t                       internal_module_count;
    struct limine_internal_module **internal_modules;
};

/* ── Boot time ────────────────────────────────────────────────────────────── */
#define LIMINE_BOOT_TIME_REQUEST \
    { LIMINE_COMMON_MAGIC, 0x502746e184c088aaULL, 0xfbc5ec83e6327893ULL }

struct limine_boot_time_response {
    uint64_t revision;
    int64_t  boot_time;
};

struct limine_boot_time_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_boot_time_response *response;
};
