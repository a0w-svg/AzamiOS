/* ============================================================================
 * AzamiOS — ACPI Subsystem
 * File: drivers/acpi/acpi.h
 * ============================================================================ */
#pragma once
#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

/* ── ACPI Generic Table Header ───────────────────────────────────────────── */
typedef struct {
    char signature[4];
    u32  length;
    u8   revision;
    u8   checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32  oem_revision;
    u32  creator_id;
    u32  creator_revision;
} __packed acpi_sdt_header_t;

/* ── RSDP ────────────────────────────────────────────────────────────────── */
typedef struct {
    char signature[8];
    u8   checksum;
    char oem_id[6];
    u8   revision;
    u32  rsdt_address;
    
    /* ACPI 2.0+ fields */
    u32  length;
    u64  xsdt_address;
    u8   extended_checksum;
    u8   reserved[3];
} __packed acpi_rsdp_t;

/* ── Generic Address Structure (GAS) ─────────────────────────────────────── */
typedef struct {
    u8  address_space;
    u8  bit_width;
    u8  bit_offset;
    u8  access_size;
    u64 address;
} __packed acpi_gas_t;

/* ── FADT (Fixed ACPI Description Table) ─────────────────────────────────── */
typedef struct {
    acpi_sdt_header_t header;
    u32 firmware_ctrl;
    u32 dsdt;
    
    u8  reserved;
    u8  preferred_pm_profile;
    u16 sci_int;
    u32 smi_cmd;
    u8  acpi_enable;
    u8  acpi_disable;
    u8  s4bios_req;
    u8  pstate_cnt;
    
    u32 pm1a_evt_blk;
    u32 pm1b_evt_blk;
    u32 pm1a_cnt_blk;
    u32 pm1b_cnt_blk;
    u32 pm2_cnt_blk;
    u32 pm_tmr_blk;
    u32 gpe0_blk;
    u32 gpe1_blk;
    
    u8  pm1_evt_len;
    u8  pm1_cnt_len;
    u8  pm2_cnt_len;
    u8  pm_tmr_len;
    u8  gpe0_blk_len;
    u8  gpe1_blk_len;
    u8  gpe1_base;
    u8  cst_cnt;
    u16 p_lvl2_lat;
    u16 p_lvl3_lat;
    u16 flush_size;
    u16 flush_stride;
    u8  duty_offset;
    u8  duty_width;
    u8  day_alrm;
    u8  mon_alrm;
    u8  century;
    
    /* ACPI 2.0+ fields */
    u16 iapc_boot_arch;
    u8  reserved2;
    u32 flags;
    acpi_gas_t reset_reg;
    u8  reset_value;
    u8  reserved3[3];
    u64 x_firmware_ctrl;
    u64 x_dsdt;
    
    /* We don't strictly need the rest of the 64-bit addresses for reboot/shutdown right now */
} __packed acpi_fadt_t;

/* ── MADT (Multiple APIC Description Table) ──────────────────────────────── */
typedef struct {
    acpi_sdt_header_t header;
    u32 lapic_addr;
    u32 flags;
} __packed acpi_madt_t;

typedef struct {
    u8 type;
    u8 length;
} __packed acpi_madt_record_t;

typedef struct {
    acpi_madt_record_t header;
    u8  acpi_processor_id;
    u8  apic_id;
    u32 flags;
} __packed acpi_madt_lapic_t;

typedef struct {
    acpi_madt_record_t header;
    u8  ioapic_id;
    u8  reserved;
    u32 ioapic_addr;
    u32 gsi_base;
} __packed acpi_madt_ioapic_t;

typedef struct {
    acpi_madt_record_t header;
    u8  bus;
    u8  source_irq;
    u32 global_system_interrupt;
    u16 flags;
} __packed acpi_madt_iso_t;

/* MADT Record Types */
#define ACPI_MADT_TYPE_LAPIC   0
#define ACPI_MADT_TYPE_IOAPIC  1
#define ACPI_MADT_TYPE_ISO     2

/* ── Public API ──────────────────────────────────────────────────────────── */

/** acpi_init() — Parse RSDP, map tables, and configure power management. */
void acpi_init(void);

/** acpi_find_table(sig) — Find an ACPI table by its 4-char signature. */
void *acpi_find_table(const char *signature);

/** acpi_reboot() — Hard reboot the system via FADT ResetReg. */
void acpi_reboot(void);

/** acpi_shutdown() — Power off the system via ACPI \_S5_ sleep state. */
void acpi_shutdown(void);
