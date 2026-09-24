/* ============================================================================
 * AzamiOS — Kernel Image Self-Protection (W^X)
 * File: arch/x86_64/mm/kprotect.h
 *
 * The kernel's own mappings are the last ones anybody checks. SMEP stops ring
 * 0 executing a user page, SMAP stops it reading one by accident, NX stops a
 * user process executing its own stack — and none of them say anything about
 * the kernel's own image, which by default is as writable as the bootloader
 * left it and reachable twice over: once at -2 GB through the kernel image
 * mapping, and once more through the HHDM, which aliases every byte of
 * physical RAM including the pages the kernel is executing out of.
 *
 * That second alias is the part that catches people out. Marking .text
 * read-only at 0xFFFFFFFF80... achieves nothing while the same frames are
 * still writable at 0xFFFF8000...; an attacker with a controlled kernel write
 * simply uses the other address. The same goes the other way round: a
 * writable-and-executable HHDM turns every kernel heap allocation into a
 * ret2dir landing pad, because the heap comes from the PMM and the PMM hands
 * out frames that are permanently mapped there.
 *
 * So this module enforces one policy over *both* views:
 *
 *   .text                        R-X   executable, never writable
 *   .rodata, .extable            R--   never writable, never executable
 *   .data..ro_after_init         R--   writable during boot, sealed here
 *   .data, .bss                  RW-   writable, never executable
 *   HHDM (all of physical RAM)   RW-   never executable
 *   HHDM alias of the three
 *   read-only regions above      R--   closes the second-address bypass
 *
 * kprotect_seal() is called once from kernel_main(), after every subsystem
 * has registered itself and before the first ring-3 instruction runs. It is
 * not reversible: there is no unseal, by design.
 *
 * Two constraints fall out of this and are worth stating plainly, because
 * breaking either one produces a page fault rather than a warning:
 *
 *   - Nothing may write a __ro_after_init object after kernel_main() reaches
 *     the seal. See include/azami/sections.h.
 *   - Nothing may execute out of the heap or any other HHDM address. Kernel
 *     code that generates code at runtime — the BPF JIT is the only one here
 *     — must allocate from kmod_alloc_exec() in kernel/mm/kmodmem.c, which
 *     hands out a separate mapping that is never writable and executable at
 *     the same moment.
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"

/**
 * kprotect_seal() — apply the policy above and report what it found.
 *
 * Idempotent and safe to call once; subsequent calls are no-ops. Logs a
 * one-line summary plus, for anything that was *already* correct, says so
 * rather than claiming credit for it.
 */
void kprotect_seal(void);

/**
 * kprotect_format(buf, max) — multi-line status for /proc and the boot log.
 * Returns the number of bytes that would have been written (scnprintf rules).
 */
size_t kprotect_format(char *buf, size_t max);

/** True once kprotect_seal() has run to completion. */
bool kprotect_is_sealed(void);
