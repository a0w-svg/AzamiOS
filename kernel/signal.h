/* ============================================================================
 * AzamiOS — Userspace signal delivery
 * File: kernel/signal.h
 *
 * The mechanism that actually transfers control to a user-registered signal
 * handler. sched_kill_process() only marks a signal pending in
 * process_t::sig_pending; signal_deliver_pending() is what consumes it on the
 * way back out to ring 3 — it pushes a signal frame onto the user stack,
 * points RIP at the handler, and arranges for rt_sigreturn(2) to restore the
 * interrupted context.
 * ============================================================================ */
#pragma once

#include "../arch/x86_64/cpu/idt.h"   /* pt_regs_t */

/**
 * signal_deliver_pending(r, syscall_nr)
 *
 * Call on every return path to ring 3 (syscall exit and IRQ/exception exit).
 * If the current process has a deliverable (pending & !blocked) signal with a
 * user handler installed, @r is rewritten in place so the pending restore
 * lands in the handler instead of the interrupted code.
 *
 * @syscall_nr : the syscall number just executed (for SA_RESTART handling), or
 *               -1 when called from the interrupt path.
 *
 * Returns 1 if a handler frame was set up, 0 otherwise.
 */
int signal_deliver_pending(pt_regs_t *r, s64 syscall_nr);

/**
 * sys_rt_sigreturn_impl() — restore the context saved by the frame that
 * signal_deliver_pending() built. Registered as SYS_rt_sigreturn.
 */
s64 sys_rt_sigreturn_impl(pt_regs_t *r);
