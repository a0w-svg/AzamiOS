/* ============================================================================
 * AzamiOS — kexec: load and jump into a fresh copy of the same kernel build
 * File: kernel/kexec.h
 *
 * Scope (see kernel/kexec.c for the full design rationale):
 *   - Only supports kexec'ing into another ELF build of *this same kernel*
 *     (or a byte-for-byte compatible one — same linker layout, same
 *     KERNEL_BASE). Arbitrary Linux bzImage-style kernels are out of scope:
 *     this is a Limine-booted kernel with no bzImage/16-bit boot support.
 *   - No initrd: this kernel has no initrd boot mechanism at all (root fs is
 *     a real disk partition), so kexec_load() rejects a real initrd_fd.
 *   - BSP-only after the jump: Application Processors are parked (cli;hlt)
 *     before the jump and stay parked forever — the new kernel instance is
 *     told (via a forged "no SMP" Limine response) to believe it is running
 *     single-CPU. Waking them back up would need a second, from-scratch AP
 *     wake protocol (Limine's own one is a single-use handshake from the
 *     very first real boot) and is explicitly out of scope for this pass.
 * ============================================================================ */
#pragma once

#include "../include/azami/types.h"
#include "../include/azami/defs.h"

struct file;

/* ── AP park IPI ──────────────────────────────────────────────────────────
 * Vector 60 sits in the generic isr_49..isr_250 stub range that isr.asm
 * already generates (see arch/x86_64/cpu/isr.asm); idt_init() points it at
 * a dedicated gate instead of letting the "any unclaimed vector gets
 * isr_spurious" fallback claim it (arch/x86_64/cpu/idt.c). Its handler
 * (also in idt.c, isr_dispatch_inner()) does the absolute minimum: bump
 * g_kexec_park_count, then spin cli;hlt forever. No EOI, no scheduler touch,
 * nothing that could deadlock or dereference memory kexec_execute() is
 * about to remap out from under it.
 * ---------------------------------------------------------------------- */
#define KEXEC_PARK_VECTOR   60

/* Incremented (once, exactly) by every AP's park handler as it parks.
 * kexec_execute() spin-waits on this (with a timeout) after sending the
 * park IPI to every other online CPU. Defined in kernel/kexec.c. */
extern volatile u32 g_kexec_park_count;

/**
 * kexec_load(kernel_file, cmdline, cmdline_len, flags) — validate and stage
 * a new kernel image for a later kexec_execute().
 *
 * @kernel_file   Already-referenced open file for the candidate kernel ELF
 *                (caller owns the fget()/fput() pair; kexec_load() does not
 *                take an extra reference and does not close it).
 * @cmdline       Kernel-space buffer already copied in from userspace by the
 *                caller (copy_str_from_user() lives in syscall.c); may be
 *                NULL/zero-length.
 * @cmdline_len   Length of @cmdline in bytes, NUL not required.
 * @flags         Must be 0 — no KEXEC_FILE_* behaviour is implemented, and
 *                claiming to honour a flag that does nothing would be the
 *                same silent-success dishonesty this replaces.
 *
 * A second call replaces (and fully frees) any previously staged image
 * rather than leaking it.
 *
 * Returns 0 on success, a real negative errno otherwise.
 */
int kexec_load(struct file *kernel_file, const char *cmdline, size_t cmdline_len,
               unsigned long flags);

/**
 * kexec_execute() — perform the actual kexec: park every other CPU, remap
 * this kernel's own virtual address range onto the staged image's physical
 * pages, and jump into it.
 *
 * Must be called with CAP_SYS_BOOT already checked by the caller (it is
 * invoked from inside sys_reboot_impl(), which checks it once for every
 * reboot(2) command).
 *
 * On any failure *before* the point of no return (no staged image, OOM
 * while building the jump plan, an AP failing to park in time, ...) this
 * returns a real negative errno and the system is completely unaffected —
 * still running as the original kernel, free to retry. Once every other CPU
 * has confirmed it is parked, this function does not return: control passes
 * to the freshly staged kernel image.
 *
 * Known limitation: if an AP fails to park before the timeout, this still
 * returns an error rather than jumping — but any AP that *did* park before
 * the timeout expired has no way to be un-parked (cli;hlt has no safe
 * "cancel"), so a failed attempt can permanently lose CPUs even though it is
 * reported as a clean failure. In practice APs park within a handful of
 * spin iterations (they are idle, not doing anything that defers an IPI).
 */
s64 kexec_execute(void);
