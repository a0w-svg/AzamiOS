/* ============================================================================
 * AzamiOS — ACPI Power Management Timer (pm_timer) Driver Header
 * File: drivers/acpi/pm_timer.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../drivers/base/platform.h"

/* Standard ACPI PM_TMR frequency: 3.579545 MHz */
#define PM_TIMER_FREQ_HZ         3579545

/* Standard I/O port for QEMU / PIIX4 / ICH9 */
#define PM_TIMER_DEFAULT_PORT    0x0408

/** pm_timer_read() — Read raw tick counter (24-bit or 32-bit). */
u32 pm_timer_read(void);

/** pm_timer_ticks_to_ns(ticks) — Convert ticks to nanoseconds. */
u64 pm_timer_ticks_to_ns(u32 ticks);

/** pm_timer_init() — Initialize ACPI PM timer and register platform driver. */
int pm_timer_init(void);
