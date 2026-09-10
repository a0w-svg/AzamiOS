/* ============================================================================
 * AzamiOS — Cross-CPU TLB Shootdown (x86_64)
 * File: arch/x86_64/mm/tlb.h
 *
 * The page tables are shared: the kernel half is literally the same PDPTs in
 * every address space, and a user address space is live on every CPU running
 * one of its threads. INVLPG and a CR3 reload only touch the CPU that executes
 * them, so tearing down or narrowing a mapping has to reach the other cores as
 * well — otherwise a remote CPU keeps writing through a stale translation into
 * a page the allocator has already handed to someone else.
 *
 * Adding a mapping needs no shootdown: the architecture does not cache
 * not-present entries, so a remote CPU that faults will simply walk the tables
 * we just updated.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/**
 * tlb_shootdown_all() — make every other CPU drop its cached translations.
 *
 * Call it *after* releasing whatever lock guarded the page-table edit: it can
 * spin waiting for remote acknowledgement, and a CPU spinning for that lock
 * with interrupts disabled would never get around to acknowledging.
 *
 * With interrupts enabled the wait is synchronous — on return, no other CPU
 * holds a translation predating the edit, so the caller may free the frame.
 * With interrupts disabled waiting is not safe (this CPU cannot service an
 * incoming shootdown, so two CPUs could wait on each other), and the request
 * is issued without waiting; the remote flush still happens, just not before
 * this function returns.
 */
void tlb_shootdown_all(void);

/**
 * tlb_shootdown_ipi() — vector-251 handler. Flushes this CPU and publishes the
 * acknowledgement. Takes no locks, so it can always make progress.
 */
void tlb_shootdown_ipi(void);

/**
 * tlb_flush_local_global() — drop every translation on *this* CPU, global
 * (kernel) pages included. A plain CR3 reload deliberately preserves entries
 * mapped with PTE.G, which is exactly wrong when the kernel mapping is the one
 * that changed.
 */
void tlb_flush_local_global(void);
