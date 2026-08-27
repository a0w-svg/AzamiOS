/* ============================================================================
 * AzamiOS — Userspace signal delivery
 * File: kernel/signal.c
 *
 * Builds the classic x86-64 signal frame on the interrupted thread's user
 * stack and redirects execution into the handler; rt_sigreturn(2) pops it and
 * resumes. Handlers are entered with the System V "as if called" contract
 * (RSP % 16 == 8, [RSP] = return address = the libc restorer trampoline).
 *
 * Signals are process-wide here (process_t::sig_pending); whichever thread
 * next returns to ring 3 runs the handler. That matches single-threaded
 * programs exactly and is a legal POSIX choice for multi-threaded ones.
 * ============================================================================ */

#include "signal.h"
#include "uaccess.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "../include/azami/defs.h"
#include "../include/azami/types.h"
#include "lib/string.h"
#include "../drivers/char/console.h"

#define SIGDBG 0
#if SIGDBG
#define sdbg(...) kprintf(__VA_ARGS__)
#else
#define sdbg(...) do {} while (0)
#endif

#define SIGFRAME_MAGIC   0x5349474652414d45ULL   /* "SIGFRAME" */

/* Linux mcontext greg indices we populate (enough for backtracing handlers). */
enum {
    MREG_R8 = 0, MREG_R9, MREG_R10, MREG_R11, MREG_R12, MREG_R13, MREG_R14,
    MREG_R15, MREG_RDI, MREG_RSI, MREG_RBP, MREG_RBX, MREG_RDX, MREG_RAX,
    MREG_RCX, MREG_RSP, MREG_RIP, MREG_EFL, MREG_CSGSFS, MREG_ERR,
    MREG_TRAPNO, MREG_OLDMASK, MREG_CR2, MREG_COUNT
};

typedef struct {
    u64       restorer;                 /* [rsp] at handler entry            */
    u64       magic;
    u64       saved_mask;               /* sig_blocked to restore           */
    pt_regs_t regs;                     /* full interrupted context         */

    /* Blocks pointed at by RSI / RDX for SA_SIGINFO handlers. Plain
     * void(int) handlers never look at them, but keeping them valid and
     * populated means a 3-arg handler won't fault either. */
    u8        siginfo[128];             /* {si_signo, si_errno, si_code, ...} */
    u64       uc_flags;
    u64       uc_link;
    u64       uc_stack[3];              /* ss_sp, ss_flags, ss_size          */
    u64       uc_gregs[MREG_COUNT];
    u64       uc_fpregs;               /* NULL — no FP state saved          */
    u64       uc_sigmask;
    u64       _reserved[8];
} sigframe_t;

static inline u32 lowest_signal(sigset_t s)
{
    for (u32 i = 1; i < _NSIG; i++)
        if (s & (1ULL << i)) return i;
    return 0;
}

int signal_deliver_pending(pt_regs_t *r, s64 syscall_nr)
{
    /* Only when we are actually about to return to ring 3. */
    if ((r->cs & 3) != 3) return 0;

    process_t *proc = sched_current_process();
    if (!proc || proc->is_zombie || !proc->sig_pending) return 0;

    sigset_t deliverable = proc->sig_pending & ~proc->sig_blocked;
    /* SIGKILL / SIGSTOP are never caught. */
    deliverable &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    if (!deliverable) return 0;

    u32 sig = lowest_signal(deliverable);
    if (!sig) return 0;

    sdbg("[SIG] deliver sig=%u pid=%u rsp=%llx rip=%llx nr=%lld\n",
         sig, proc->pid, (unsigned long long)r->rsp,
         (unsigned long long)r->rip, (long long)syscall_nr);

    sigaction_t act = proc->sigactions[sig];
    if (act.sa_handler == SIG_DFL || act.sa_handler == SIG_IGN) {
        /* Nothing to run here — clear it and let the default path (already
         * taken in sched_kill_process for fatal signals) stand. */
        proc->sig_pending &= ~(1ULL << sig);
        return 0;
    }
    if (!act.sa_restorer) {
        /* Without a restorer we cannot get control back cleanly. Drop the
         * signal rather than corrupt the thread. */
        proc->sig_pending &= ~(1ULL << sig);
        return 0;
    }

    /* Interrupted syscall: restart it if the handler asked for SA_RESTART,
     * otherwise leave the -EINTR the syscall already returned. The `syscall`
     * instruction is 2 bytes, and r->rip points just past it. */
    if (syscall_nr >= 0 && (s64)r->rax == -(s64)EINTR &&
        (act.sa_flags & SA_RESTART)) {
        r->rip -= 2;
        r->rax  = (u64)syscall_nr;
    }

    /* Carve the frame out of the user stack (below the 128-byte red zone),
     * then bias so RSP % 16 == 8 on handler entry. */
    u64 sp = r->rsp - 128;
    sp -= sizeof(sigframe_t);
    sp &= ~15ULL;
    sp -= 8;

    sigframe_t f;
    memset(&f, 0, sizeof(f));
    f.restorer   = (u64)(uintptr_t)act.sa_restorer;
    f.magic      = SIGFRAME_MAGIC;
    f.saved_mask = proc->sig_blocked;
    f.regs       = *r;

    /* Minimal Linux-compatible siginfo: si_signo, si_errno, si_code. */
    ((s32 *)f.siginfo)[0] = (s32)sig;
    ((s32 *)f.siginfo)[2] = 0;                       /* SI_USER */

    f.uc_sigmask       = proc->sig_blocked;
    f.uc_gregs[MREG_R8]  = r->r8;   f.uc_gregs[MREG_R9]  = r->r9;
    f.uc_gregs[MREG_R10] = r->r10;  f.uc_gregs[MREG_R11] = r->r11;
    f.uc_gregs[MREG_R12] = r->r12;  f.uc_gregs[MREG_R13] = r->r13;
    f.uc_gregs[MREG_R14] = r->r14;  f.uc_gregs[MREG_R15] = r->r15;
    f.uc_gregs[MREG_RDI] = r->rdi;  f.uc_gregs[MREG_RSI] = r->rsi;
    f.uc_gregs[MREG_RBP] = r->rbp;  f.uc_gregs[MREG_RBX] = r->rbx;
    f.uc_gregs[MREG_RDX] = r->rdx;  f.uc_gregs[MREG_RAX] = r->rax;
    f.uc_gregs[MREG_RCX] = r->rcx;  f.uc_gregs[MREG_RSP] = r->rsp;
    f.uc_gregs[MREG_RIP] = r->rip;  f.uc_gregs[MREG_EFL] = r->rflags;

    if (copy_to_user((void *)(uintptr_t)sp, &f, sizeof(f)) != 0) {
        /* Bad user stack — turn this into a fatal SIGSEGV. */
        proc->sig_pending &= ~(1ULL << sig);
        sched_kill_process(proc->pid, SIGSEGV);
        return 0;
    }

    /* Enter the handler. */
    u64 uctx = sp + __builtin_offsetof(sigframe_t, uc_flags);
    u64 sinfo = sp + __builtin_offsetof(sigframe_t, siginfo);

    r->rip = (u64)(uintptr_t)act.sa_handler;
    r->rsp = sp;
    r->rdi = sig;
    r->rsi = sinfo;      /* meaningful only with SA_SIGINFO */
    r->rdx = uctx;       /* meaningful only with SA_SIGINFO */
    r->rax = 0;

    /* Block this signal (unless SA_NODEFER) plus the handler's sa_mask for
     * the duration of the handler; rt_sigreturn restores saved_mask. */
    proc->sig_blocked |= act.sa_mask;
    if (!(act.sa_flags & SA_NODEFER))
        proc->sig_blocked |= (1ULL << sig);

    proc->sig_pending &= ~(1ULL << sig);

    if (act.sa_flags & SA_RESETHAND)
        proc->sigactions[sig].sa_handler = SIG_DFL;

    sdbg("[SIG]  -> handler=%llx restorer=%llx newrsp=%llx\n",
         (unsigned long long)r->rip, (unsigned long long)f.restorer,
         (unsigned long long)r->rsp);
    return 1;
}

s64 sys_rt_sigreturn_impl(pt_regs_t *r)
{
    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* The restorer did `ret` (popping the 8-byte pretcode) before invoking us,
     * so the frame base sits one slot below the current user RSP. */
    u64 base = r->rsp - 8;

    sdbg("[SIG] sigreturn pid=%u base=%llx\n", proc->pid, (unsigned long long)base);

    sigframe_t f;
    if (copy_from_user(&f, (const void *)(uintptr_t)base, sizeof(f)) != 0)
        return -(s64)EFAULT;
    if (f.magic != SIGFRAME_MAGIC) {
        sdbg("[SIG] sigreturn BAD MAGIC %llx\n", (unsigned long long)f.magic);
        return -(s64)EFAULT;
    }

    /* Restore the interrupted register state and the pre-signal mask. */
    sigset_t keep_pending = proc->sig_pending;
    *r = f.regs;
    proc->sig_blocked = f.saved_mask & ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    proc->sig_pending = keep_pending;

    /* Return the value the interrupted context had in RAX so the dispatcher's
     * `regs->rax = <ret>` is a no-op on the restored frame. */
    return (s64)r->rax;
}
