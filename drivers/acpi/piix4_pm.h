/* ============================================================================
 * AzamiOS — Intel PIIX4 Power Management / ACPI Controller Driver Header
 * File: drivers/acpi/piix4_pm.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../hal/pci.h"
#include "../base/pci_bus.h"

/* ── PIIX4 PCI Configuration Registers ───────────────────────────────────── */
#define PIIX4_PCI_PMBASE        0x40  /* 32-bit Power Management Base Address */
#define PIIX4_PCI_PMREGMISC     0x80  /* 8-bit PM Misc Control (bit 0 = PMIOSE) */
#define PIIX4_PMIOSE_ENABLE     0x01

/* ── ACPI PM I/O Registers (offsets from PMBASE) ─────────────────────────── */
#define PIIX4_PM_PM1_STS        0x00  /* 16-bit PM1a Status */
#define PIIX4_PM_PM1_EN         0x02  /* 16-bit PM1a Enable */
#define PIIX4_PM_PM1_CNT        0x04  /* 16-bit PM1a Control */
#define PIIX4_PM_TMR            0x08  /* 32-bit PM Timer (3.579545 MHz) */
#define PIIX4_PM_GPE0_STS       0x0C  /* 32-bit General Purpose Event 0 Status */
#define PIIX4_PM_GPE0_EN        0x10  /* 32-bit General Purpose Event 0 Enable */
#define PIIX4_PM_PCNTRL         0x14  /* 32-bit Processor Control */
#define PIIX4_PM_PLVL2          0x14  /* 8-bit Processor Level 2 (C2) */
#define PIIX4_PM_PLVL3          0x15  /* 8-bit Processor Level 3 (C3) */
#define PIIX4_PM_GLBCTL         0x1C  /* 32-bit Global Control */
#define PIIX4_PM_DEVACTA        0x20  /* 32-bit Device Activity A */
#define PIIX4_PM_DEVACTB        0x24  /* 32-bit Device Activity B */

/* ── PM1_CNT Bits ────────────────────────────────────────────────────────── */
#define PIIX4_PM1_SCI_EN        (1 << 0)  /* SCI Enable */
#define PIIX4_PM1_BM_RLD        (1 << 1)  /* Bus Master Reload */
#define PIIX4_PM1_GBL_RLS       (1 << 2)  /* Global Release */
#define PIIX4_PM1_SLP_TYP_S5    (5 << 10) /* Sleep Type S5 (Soft Off) */
#define PIIX4_PM1_SLP_EN        (1 << 13) /* Sleep Enable */

/* ── IOCTL Commands for /dev/acpi_pm ─────────────────────────────────────── */
#define PM_IOC_GET_TIMER        0x5001
#define PM_IOC_POWEROFF         0x5002
#define PM_IOC_GET_STATUS       0x5003

typedef struct piix4_pm_status {
    u16 pm_base;
    u16 pm1_sts;
    u16 pm1_en;
    u16 pm1_cnt;
    u32 pm_tmr;
    u32 gpe0_sts;
} piix4_pm_status_t;

/** piix4_pm_init() — Register the Intel PIIX4 PM PCI driver. */
int piix4_pm_init(void);

/** piix4_pm_read_timer() — Read current 3.579545 MHz ACPI timer count. */
u32 piix4_pm_read_timer(void);

/** piix4_pm_poweroff() — Trigger ACPI soft poweroff via PIIX4 PM1a_CNT. */
void piix4_pm_poweroff(void);
