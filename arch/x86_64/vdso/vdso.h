/* ============================================================================
 * AzamiOS — vDSO image management and per-process mapping
 * File: arch/x86_64/vdso/vdso.h
 * ============================================================================ */
#pragma once

#include "../../../include/azami/types.h"
#include "../mm/vmm.h"

struct process;

/** vdso_init() — copy the embedded linux-vdso.so.1 into shared frames.
 *  After timekeeping_init() (it maps the timekeeper's vvar page). */
void vdso_init(void);

/** vdso_map(proc, space) — map [vvar] + [vdso] into a fresh address space.
 *  Returns the image base for AT_SYSINFO_EHDR, or 0 if there is no vDSO
 *  (libc then simply falls back to the syscalls). */
u64 vdso_map(struct process *proc, vmm_space_t space);
