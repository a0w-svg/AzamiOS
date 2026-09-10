/* ============================================================================
 * AzamiOS — Machine Check Architecture (x86_64)
 * File: arch/x86_64/cpu/mce.h
 *
 * MCA is the CPU telling the OS that its own hardware failed: an uncorrectable
 * ECC error in a cache line, a parity failure on an internal bus, a TLB
 * datapath fault. Without it the machine either wedges or, worse, keeps running
 * on corrupted data. With it, vector 18 (#MC) delivers a decodable record and
 * the OS gets to choose between logging a corrected error and stopping cleanly.
 *
 * The banks are also readable outside an exception, which is how *corrected*
 * errors surface: hardware fixed them and set a status bit, and nothing tells
 * the OS unless it looks. mce_poll() is that look; /proc/mcelog triggers it.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "idt.h"

/** Set once CR4.MCE is live and the banks have been armed. */
extern u8 g_mce_enabled;

/** mce_init() — enumerate banks, clear stale state, enable #MC. BSP only. */
void mce_init(void);

/** mce_init_ap() — arm the same banks on an application processor.
 *  The bank MSRs and CR4.MCE are per-logical-processor, so a core that skipped
 *  this would take an unhandled #MC or, worse, report nothing at all. */
void mce_init_ap(void);

/**
 * mce_handle(r) — decode vector 18.
 *
 * Returns true when the machine check was survivable and execution may resume,
 * false when the record says the processor context is corrupt or the pushed RIP
 * is not restartable — in which case the caller must not return to it.
 */
bool mce_handle(pt_regs_t *r);

/** mce_poll() — drain corrected-error records from every bank into the log. */
void mce_poll(void);

/** mce_format(buf, max) — render the error log, newest last. */
size_t mce_format(char *buf, size_t max);
