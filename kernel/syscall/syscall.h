/* ============================================================================
 * AzamiOS — System Call Dispatcher
 * File: kernel/syscall/syscall.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../arch/x86_64/cpu/idt.h"   /* pt_regs_t */

struct process;

/* Canonical syscall-number / uapi-struct definitions, shared verbatim with
 * the native libc (userland/libc/include/sys/syscall.h -> a build-time copy
 * at userland/libc/include/azami/uapi/syscall_nr.h). See that header's top
 * comment and scripts/check_uapi_sync.sh — never redefine any SYS_* name or
 * uapi struct anywhere else; add it to the canonical header instead. */
#include "../../include/azami/uapi/syscall_nr.h"

/**
 * syscall_dispatch() — Main C-side syscall handler.
 * Called from syscall_entry.asm with the full register frame.
 * The syscall result is placed in regs->rax by this function.
 * Returns 1 if the caller must return to ring 3 via IRETQ instead of SYSRETQ
 * (rt_sigreturn), 0 otherwise.
 */
int syscall_dispatch(pt_regs_t *regs);

/**
 * sys_exit_impl() — Exit current process and free resources.
 */
s64 sys_exit_impl(pt_regs_t *r);

/**
 * syscall_init() — Register all syscall handlers.
 * Called from kernel_main() after the scheduler is up.
 */
void syscall_init(void);

/**
 * syscall_install_fd(proc, file, fd_flags) — claim the lowest free descriptor
 * for @file in @proc's table, under the fd-table lock.
 *
 * Exported so a subsystem that mints an anonymous file (perf_event_open(2))
 * does not have to open-code the "scan for a NULL slot" loop — two threads
 * racing on an unlocked scan both take the same descriptor.
 *
 * Returns the descriptor, or -EMFILE when the table is full.
 */
s64 syscall_install_fd(struct process *proc, void *file, u8 fd_flags);

/**
 * fd_table_release(proc) — detach and release every descriptor @proc still
 * holds, as part of tearing the process down.
 *
 * Each slot is cleared under the fd-table lock, so a cross-process fget() on
 * another CPU (pidfd_getfd(2) reaching into this table) either takes a
 * reference to the live file before it is closed here, or sees an empty slot —
 * never a pointer that is mid-free. The vfs_close()/object dereference for each
 * detached entry then runs outside the lock. Idempotent and safe to call from
 * more than one teardown path racing on the same table.
 */
void fd_table_release(struct process *proc);
