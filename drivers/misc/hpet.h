/* ============================================================================
 * AzamiOS — HPET (High Precision Event Timer) driver
 * File: drivers/misc/hpet.h
 *
 * Provides a monotonic nanosecond time source for clock_gettime(CLOCK_MONOTONIC)
 * and clock_getres(). Falls back gracefully (hpet_available() == false) on
 * machines with no HPET, in which case callers keep using the 100 Hz tick.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** hpet_init() — Locate the HPET via ACPI, map its MMIO, start the counter.
 *  Safe to call once after acpi_init(). */
void hpet_init(void);

/** hpet_available() — true once hpet_init() has successfully started the timer. */
bool hpet_available(void);

/** hpet_now_ns() — Nanoseconds since hpet_init() (monotonic, ~ns resolution).
 *  Returns 0 if the HPET is not available. */
u64 hpet_now_ns(void);

/** hpet_resolution_ns() — Counter period in nanoseconds (rounded up, min 1). */
u64 hpet_resolution_ns(void);

/** hpet_read_counter() — Raw main-counter value, masked to its real width. */
u64 hpet_read_counter(void);

/** hpet_counter_mask() — ~0 for a 64-bit counter, 0xFFFFFFFF for 32-bit. */
u64 hpet_counter_mask(void);

/** hpet_frequency_hz() — Main-counter frequency, 0 if unavailable. */
u64 hpet_frequency_hz(void);

/** hpet_counter_phys() — Physical address of the main-counter register, so
 *  the vDSO can map its page read-only into user space. 0 if unavailable. */
phys_addr_t hpet_counter_phys(void);
