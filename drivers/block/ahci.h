/* ============================================================================
 * AzamiOS — AHCI Controller Definitions
 * File: drivers/block/ahci.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

#define SATA_SIG_ATA    0x00000101  // SATA drive
#define SATA_SIG_ATAPI  0xEB140101  // SATAPI drive
#define SATA_SIG_SEMB   0xC33C0101  // Enclosure management bridge
#define SATA_SIG_PM     0x96690101  // Port multiplier

#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SATAPI 2
#define AHCI_DEV_SEMB   3
#define AHCI_DEV_PM     4

#define HBA_PxCMD_ST    0x0001
#define HBA_PxCMD_FRE   0x0010
#define HBA_PxCMD_FR    0x4000
#define HBA_PxCMD_CR    0x8000

#define HBA_PxIS_TFES   (1U << 30)

// FIS Types
typedef enum {
    FIS_TYPE_REG_H2D    = 0x27, // Register FIS - host to device
    FIS_TYPE_REG_D2H    = 0x34, // Register FIS - device to host
    FIS_TYPE_DMA_ACT    = 0x39, // DMA activate FIS - device to host
    FIS_TYPE_DMA_SETUP  = 0x41, // DMA setup FIS - bidirectional
    FIS_TYPE_DATA       = 0x46, // Data FIS - bidirectional
    FIS_TYPE_BIST       = 0x58, // BIST activate FIS - bidirectional
    FIS_TYPE_PIO_SETUP  = 0x5F, // PIO setup FIS - device to host
    FIS_TYPE_DEV_BITS   = 0xA1, // Set device bits FIS - device to host
} fis_type_t;

// PRDT
typedef struct {
    u32 dba;        // Data base address
    u32 dbau;       // Data base address upper 32 bits
    u32 rsv0;       // Reserved
    u32 dbc;        // Byte count, 4M max, interrupt = bit 31
} ahci_prdt_entry_t;

// Command table
typedef struct {
    u8  cfis[64];   // Command FIS
    u8  acmd[16];   // ATAPI command, 12 or 16 bytes
    u8  rsv[48];    // Reserved
    ahci_prdt_entry_t prdt_entry[8]; // Max 8 entries per command
} ahci_cmd_table_t;

// Command list entry
typedef struct {
    u8  cfl:5;      // Command FIS length in DWORDS, 2 ~ 16
    u8  a:1;        // ATAPI
    u8  w:1;        // Write, 1: H2D, 0: D2H
    u8  p:1;        // Prefetchable
    u8  r:1;        // Reset
    u8  b:1;        // BIST
    u8  c:1;        // Clear busy upon R_OK
    u8  rsv0:1;     // Reserved
    u8  pmp:4;      // Port multiplier port
    u16 prdtl;      // Physical region descriptor table length in entries
    u32 prdbc;      // Physical region descriptor byte count transferred
    u32 ctba;       // Command table descriptor base address
    u32 ctbau;      // Command table descriptor base address upper 32 bits
    u32 rsv1[4];    // Reserved
} ahci_cmd_header_t;

// FIS receive area
typedef struct {
    u8 dsfis[28];    // DMA Setup FIS
    u8 rsv1[4];
    u8 psfis[24];    // PIO Setup FIS
    u8 rsv2[8];
    u8 rfis[24];     // D2H Register FIS
    u8 rsv3[4];
    u8 sdbfis[8];    // Set Device Bits FIS
    u8 ukfis[64];    // Unknown FIS
    u8 rsv4[96];
} ahci_hba_fis_t;

// AHCI HBA Port Memory Registers
typedef volatile struct {
    u32 clb;        // 0x00, command list base address, 1K-byte aligned
    u32 clbu;       // 0x04, command list base address upper 32 bits
    u32 fb;         // 0x08, FIS base address, 256-byte aligned
    u32 fbu;        // 0x0C, FIS base address upper 32 bits
    u32 is;         // 0x10, interrupt status
    u32 ie;         // 0x14, interrupt enable
    u32 cmd;        // 0x18, command and status
    u32 rsv0;       // 0x1C, Reserved
    u32 tfd;        // 0x20, task file data
    u32 sig;        // 0x24, signature
    u32 ssts;       // 0x28, SATA status (SCR0:SStatus)
    u32 sctl;       // 0x2C, SATA control (SCR2:SControl)
    u32 serr;       // 0x30, SATA error (SCR1:SError)
    u32 sact;       // 0x34, SATA active (SCR3:SActive)
    u32 ci;         // 0x38, command issue
    u32 sntf;       // 0x3C, SATA notification (SCR4:SNotification)
    u32 fbs;        // 0x40, FIS-based switch control
    u32 rsv1[11];   // 0x44 ~ 0x6F, Reserved
    u32 vendor[4];  // 0x70 ~ 0x7F, vendor specific
} ahci_port_t;

// AHCI HBA Memory Registers
typedef volatile struct {
    u32 cap;        // 0x00, Host capability
    u32 ghc;        // 0x04, Global host control
    u32 is;         // 0x08, Interrupt status
    u32 pi;         // 0x0C, Port implemented
    u32 vs;         // 0x10, Version
    u32 ccc_ctl;    // 0x14, Command completion coalescing control
    u32 ccc_pts;    // 0x18, Command completion coalescing ports
    u32 em_loc;     // 0x1C, Enclosure management location
    u32 em_ctl;     // 0x20, Enclosure management control
    u32 cap2;       // 0x24, Host capabilities extended
    u32 bohc;       // 0x28, BIOS/OS handoff control and status
    u8  rsv[0xA0-0x2C];
    u8  vendor[0x100-0xA0];
    ahci_port_t ports[32]; // 0x100 ~ 0x10FF, Port control registers
} ahci_hba_t;

typedef struct {
    u8  fis_type;
    u8  pmport:4;
    u8  rsv0:3;
    u8  c:1;        // 1: Command, 0: Control
    u8  command;    // Command register
    u8  featurel;   // Feature register, 7:0
    u8  lba0;       // LBA low register, 7:0
    u8  lba1;       // LBA mid register, 15:8
    u8  lba2;       // LBA high register, 23:16
    u8  device;     // Device register
    u8  lba3;       // LBA register, 31:24
    u8  lba4;       // LBA register, 39:32
    u8  lba5;       // LBA register, 47:40
    u8  featureh;   // Feature register, 15:8
    u8  countl;     // Count register, 7:0
    u8  counth;     // Count register, 15:8
    u8  icc;        // Isochronous command completion
    u8  control;    // Control register
    u8  rsv1[4];    // Reserved
} fis_reg_h2d_t;

#define ATA_CMD_READ_DMA_EX     0x25
#define ATA_CMD_WRITE_DMA_EX    0x35
#define ATA_CMD_IDENTIFY        0xEC
