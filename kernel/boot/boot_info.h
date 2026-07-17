/**
 * kernel/boot/boot_info.h — Robust, Protocol-Compliant Boot Information Parsing & Validation
 *
 * Enforces exact memory alignment and bounds checking for Multiboot 1, Multiboot 2,
 * PVH (Xen/QEMU direct ELF note), UEFI, and Limine bootloader protocol tags.
 */
#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Magic Numbers */
#define MULTIBOOT1_BOOTLOADER_MAGIC  0x2BADB002UL
#define MULTIBOOT2_BOOTLOADER_MAGIC  0x36D76289UL
#define PVH_BOOTLOADER_MAGIC         0x336ec578UL
#define UEFI_BOOTLOADER_MAGIC        0xEF1B0072UL

/* ============================================================================
 * Multiboot 1 Specifications (All structs aligned(4), packed)
 * ============================================================================ */
#ifndef MULTIBOOT_INFO_MEMORY
#define MULTIBOOT_INFO_MEMORY          (1 << 0)
#define MULTIBOOT_INFO_BOOTDEV         (1 << 1)
#define MULTIBOOT_INFO_CMDLINE         (1 << 2)
#define MULTIBOOT_INFO_MODS            (1 << 3)
#define MULTIBOOT_INFO_MEM_MAP         (1 << 6)
#define MULTIBOOT_INFO_FRAMEBUFFER_INFO (1 << 12)
#endif

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint16_t vbe_mode;
    uint16_t vbe_interface_seg;
    uint16_t vbe_interface_off;
    uint16_t vbe_interface_len;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint8_t  color_info[6];
} __attribute__((aligned(4), packed)) multiboot1_info_t;

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((aligned(4), packed)) multiboot1_mmap_entry_t;

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t pad;
} __attribute__((aligned(4), packed)) multiboot1_module_t;


/* ============================================================================
 * Multiboot 2 Specifications (Header and Tags aligned(8))
 * ============================================================================ */
#define MULTIBOOT2_TAG_TYPE_END           0
#define MULTIBOOT2_TAG_TYPE_CMDLINE       1
#define MULTIBOOT2_TAG_TYPE_BOOT_LOADER   2
#define MULTIBOOT2_TAG_TYPE_MODULE        3
#define MULTIBOOT2_TAG_TYPE_BASIC_MEMINFO 4
#define MULTIBOOT2_TAG_TYPE_BOOTDEV       5
#define MULTIBOOT2_TAG_TYPE_MMAP          6
#define MULTIBOOT2_TAG_TYPE_FRAMEBUFFER   8

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((aligned(8))) multiboot2_info_header_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((aligned(8))) multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
    char cmdline[0];
} __attribute__((aligned(8))) multiboot2_tag_module_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mem_lower;
    uint32_t mem_upper;
} __attribute__((aligned(8))) multiboot2_tag_basic_meminfo_t;

typedef struct {
    uint64_t addr;
    uint64_t len;
    uint32_t type;
    uint32_t reserved;
} __attribute__((aligned(8))) multiboot2_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    multiboot2_mmap_entry_t entries[0];
} __attribute__((aligned(8))) multiboot2_tag_mmap_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t  framebuffer_bpp;
    uint8_t  framebuffer_type;
    uint16_t reserved;
} __attribute__((aligned(8))) multiboot2_tag_framebuffer_t;


/* ============================================================================
 * PVH Specifications (Xen / PE32+ / QEMU -kernel Direct 64-bit Boot)
 * ============================================================================ */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t pad;
} __attribute__((aligned(8), packed)) hvm_start_info_t;

typedef struct {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdline_paddr;
    uint64_t reserved;
} __attribute__((aligned(8), packed)) hvm_modlist_entry_t;

typedef struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
} __attribute__((aligned(8), packed)) hvm_memmap_table_entry_t;


/* ============================================================================
 * Limine Specifications (aligned(8))
 * ============================================================================ */
typedef struct {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint16_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
} __attribute__((aligned(8))) limine_framebuffer_t;

typedef struct {
    uint64_t revision;
    uint64_t framebuffer_count;
    limine_framebuffer_t **framebuffers;
} __attribute__((aligned(8))) limine_framebuffer_response_t;


/* ============================================================================
 * Unified Normalized Boot Information Structure
 * ============================================================================ */
typedef enum {
    BOOT_PROTO_UNKNOWN = 0,
    BOOT_PROTO_MULTIBOOT1,
    BOOT_PROTO_MULTIBOOT2,
    BOOT_PROTO_PVH,
    BOOT_PROTO_UEFI,
    BOOT_PROTO_LIMINE
} boot_protocol_t;

typedef struct {
    boot_protocol_t protocol;
    unsigned long raw_magic;
    unsigned long raw_addr;

    /* Memory boundaries */
    uint64_t mem_lower_kb;
    uint64_t mem_upper_kb;
    uint64_t total_mem_kb;
    uint64_t highest_phys_addr;

    /* Initrd / Ramdisk */
    uint64_t initrd_paddr;
    uint64_t initrd_size;

    /* Framebuffer */
    bool has_framebuffer;
    uint64_t fb_addr;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;
    uint32_t fb_bpp;

    /* Safe physical boundary after kernel image and all loaded modules */
    uint64_t safe_free_mem_start;
} azami_boot_info_t;


/* Early Serial and Validation API */
void early_serial_init(void);
void early_serial_puts(const char* str);
void early_serial_puthex(uint64_t val);
void early_panic(const char* reason);
bool boot_info_validate_and_parse(unsigned long magic, unsigned long addr, azami_boot_info_t *out_info);

#endif /* BOOT_INFO_H */
