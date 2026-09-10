/* ============================================================================
 * AzamiOS — ptrace(2) and the stops it drives
 * File: kernel/ptrace.h
 *
 * Everything a debugger needs from the kernel: attach to a process, park it at
 * a point of interest, read and write its registers and memory, and let it go
 * again. strace(1) needs the syscall stops; gdb(1) needs the signal stops,
 * single-step and the register sets.
 *
 * Where the stops actually happen
 * ------------------------------
 * A tracee never stops "wherever it is". It stops at one of four places, all
 * of them on a path that is about to return to ring 3, where the thread's
 * pt_regs frame is complete and safe to hand out:
 *
 *   syscall-entry / syscall-exit  syscall_dispatch()  (PTRACE_SYSCALL)
 *   signal-delivery               signal_deliver_pending()
 *   trap (#BP / #DB)              the ring-3 exception path in idt.c
 *   event (exec, exit)            the syscall that causes the event
 *
 * The park itself is the job-control stop from kernel/sched/sched.c: the
 * thread blocks in sched_stop_current() with process_t::stop_state set to
 * PROC_STOP_PTRACE. That is deliberate — a ptrace-stop and a SIGSTOP differ in
 * who is allowed to end them, not in what being stopped means, so they share
 * one mechanism and one wait4(2) reporting path.
 *
 * Scope, stated plainly
 * ---------------------
 * Tracing is per *process*, not per thread: process_t carries the tracer and
 * the stop, and a stop parks every thread of the tracee. For the single- and
 * few-threaded programs a debugger is usually pointed at this is right; for a
 * heavily threaded one a tracer cannot single out one thread, and PTRACE_ATTACH
 * on a tid stops its whole process. That matches how signals already work in
 * this kernel (process-wide sig_pending), so it is a consistent limitation
 * rather than a surprise, but it is a limitation.
 *
 * Hardware debug registers (DR0-DR3) are not exposed: PTRACE_PEEKUSER over the
 * u_debugreg area reads as zero and writes are accepted and dropped, because a
 * kernel that let a tracer arm a real breakpoint register would also have to
 * context-switch DR7, and half of that is worse than none of it. Single-step
 * (RFLAGS.TF) is fully implemented and is what a debugger reaches for anyway.
 * ============================================================================ */
#pragma once

#include "sched/sched.h"
#include "../arch/x86_64/cpu/idt.h"

/* ── ptrace(2) requests (Linux numbering) ─────────────────────────────────── */
#define PTRACE_TRACEME       0
#define PTRACE_PEEKTEXT      1
#define PTRACE_PEEKDATA      2
#define PTRACE_PEEKUSER      3
#define PTRACE_POKETEXT      4
#define PTRACE_POKEDATA      5
#define PTRACE_POKEUSER      6
#define PTRACE_CONT          7
#define PTRACE_KILL          8
#define PTRACE_SINGLESTEP    9
#define PTRACE_GETREGS       12
#define PTRACE_SETREGS       13
#define PTRACE_GETFPREGS     14
#define PTRACE_SETFPREGS     15
#define PTRACE_ATTACH        16
#define PTRACE_DETACH        17
#define PTRACE_SYSCALL       24
#define PTRACE_SETOPTIONS    0x4200
#define PTRACE_GETEVENTMSG   0x4201
#define PTRACE_GETSIGINFO    0x4202
#define PTRACE_SETSIGINFO    0x4203
#define PTRACE_GETREGSET     0x4204
#define PTRACE_SETREGSET     0x4205
#define PTRACE_SEIZE         0x4206
#define PTRACE_INTERRUPT     0x4207
#define PTRACE_LISTEN        0x4208

/* ── PTRACE_SETOPTIONS bits ───────────────────────────────────────────────── */
#define PTRACE_O_TRACESYSGOOD   0x00000001
#define PTRACE_O_TRACEFORK      0x00000002
#define PTRACE_O_TRACEVFORK     0x00000004
#define PTRACE_O_TRACECLONE     0x00000008
#define PTRACE_O_TRACEEXEC      0x00000010
#define PTRACE_O_TRACEVFORKDONE 0x00000020
#define PTRACE_O_TRACEEXIT      0x00000040
#define PTRACE_O_EXITKILL       0x00100000
#define PTRACE_O_MASK           0x0010007f

/* ── Event codes, reported in bits 16-23 of the wait status ───────────────── */
#define PTRACE_EVENT_FORK        1
#define PTRACE_EVENT_VFORK       2
#define PTRACE_EVENT_CLONE       3
#define PTRACE_EVENT_EXEC        4
#define PTRACE_EVENT_VFORK_DONE  5
#define PTRACE_EVENT_EXIT        6
#define PTRACE_EVENT_STOP        128

/* ── process_t::ptrace_flags ──────────────────────────────────────────────── */
#define PT_TRACED         0x01   /* a tracer is attached                     */
#define PT_SYSCALL_TRACE  0x02   /* stop at the next syscall entry and exit  */
#define PT_SINGLESTEP     0x04   /* resume with RFLAGS.TF set                */
#define PT_IN_SYSCALL     0x08   /* the next syscall stop is an *exit* stop  */
#define PT_SEIZED         0x10   /* attached with PTRACE_SEIZE               */

/* NT_* regset identifiers PTRACE_GETREGSET understands. */
#define NT_PRSTATUS   1
#define NT_PRFPREG    2

/**
 * struct user_regs_struct — the x86-64 register block ptrace exchanges.
 *
 * Field order is the Linux ABI's, not a natural one; PTRACE_PEEKUSER indexes
 * into it by byte offset, so it must not be rearranged.
 */
struct user_regs_struct {
    u64 r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx;
    u64 rsi, rdi, orig_rax, rip, cs, eflags, rsp, ss;
    u64 fs_base, gs_base, ds, es, fs, gs;
};

/** sys_ptrace_impl() — the ptrace(2) entry point, registered as SYS_ptrace. */
s64 sys_ptrace_impl(pt_regs_t *r);

/**
 * ptrace_check_stop(r) — park here if a stop is outstanding.
 *
 * Called from signal_deliver_pending() on every return to ring 3. Does nothing
 * unless process_t::stop_state is set, so the cost on the hot path is one
 * predictable load.
 */
void ptrace_check_stop(pt_regs_t *r);

/**
 * ptrace_pre_signal(r, sig) — signal-delivery-stop.
 *
 * Gives the tracer a look at @sig before it is acted on, and returns the
 * signal that should actually be delivered: @sig unchanged when untraced, a
 * different signal if the tracer substituted one, or 0 if it suppressed the
 * signal entirely. SIGKILL is never stoppable and passes straight through.
 */
int ptrace_pre_signal(pt_regs_t *r, int sig);

/**
 * ptrace_syscall_stop(r, on_entry) — the PTRACE_SYSCALL stops.
 *
 * On entry the tracer may rewrite the syscall number and arguments; setting
 * RAX to -1 makes the dispatcher skip the call, which is how a tracer cancels
 * one. On exit it may rewrite the return value. No-ops unless PT_SYSCALL_TRACE
 * is set.
 */
void ptrace_syscall_stop(pt_regs_t *r, bool on_entry);

/**
 * ptrace_report_trap(r, sig) — a ring-3 #BP or #DB.
 *
 * Returns true when the fault was turned into a ptrace-stop and the thread
 * should simply resume; false when there is no tracer and the caller must fall
 * back to the default action (a fatal SIGTRAP).
 */
bool ptrace_report_trap(pt_regs_t *r, int sig);

/** ptrace_report_event(r, event, msg) — PTRACE_EVENT_* stop (exec, exit, ...). */
void ptrace_report_event(pt_regs_t *r, u32 event, u64 msg);

/**
 * ptrace_release(p) — drop every tracing relationship @p is part of.
 *
 * Call when a process dies: as a tracee it must clear its own tracer link, and
 * as a tracer it must let its tracees run again, otherwise they stay parked
 * forever waiting for a resume that can never come. PTRACE_O_EXITKILL tracees
 * are killed here instead.
 */
void ptrace_release(process_t *p);

/** ptrace_traced(p) — true when @p currently has a tracer attached. */
static inline bool ptrace_traced(const process_t *p)
{
    return p && p->tracer_pid != 0 && (p->ptrace_flags & PT_TRACED) != 0;
}
