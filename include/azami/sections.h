/* ============================================================================
 * AzamiOS — Kernel Section Attributes
 * File: include/azami/sections.h
 *
 * Placement attributes for data whose write lifetime is shorter than the
 * system's. The kernel's own writable data is the largest attack surface it
 * has that nothing else defends: SMEP, SMAP, NX and the stack canary all
 * assume the attacker cannot simply overwrite a function pointer the kernel
 * is about to call. A syscall dispatch table sitting in ordinary .data makes
 * every one of them irrelevant to anyone who lands a single controlled write.
 *
 * __ro_after_init says "this is written during boot and never again". The
 * linker gathers everything so marked into one page-aligned section (see
 * scripts/kernel.ld), and kprotect_seal() — called from kernel_main() once
 * every subsystem has registered itself, before the first ring-3 instruction
 * runs — makes those pages read-only in the page tables, through every alias
 * they have. After that a stray or hostile write to one faults instead of
 * succeeding, and the hardware, not an audit, is what enforces it.
 *
 * Rules for using it:
 *   - The variable must be written only from boot-time initialisation, on the
 *     path that runs before kprotect_seal(). A write afterwards is a hard
 *     page fault, not a warning.
 *   - Per-CPU state updated by AP bring-up is fine: APs come up long before
 *     the seal. Anything a *running* system updates — counters, caches,
 *     policy that /proc can change — is not, and belongs in plain .data.
 *   - It costs nothing at runtime. The bits are ordinary loads; only the
 *     page permission differs.
 * ============================================================================ */
#pragma once

#define __ro_after_init  __attribute__((section(".data..ro_after_init")))
