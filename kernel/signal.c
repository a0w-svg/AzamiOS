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
#include "ptrace.h"
#include "uaccess.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "../include/azami/defs.h"
#include "../include/azami/types.h"
#include "lib/string.h"
#include "../drivers/char/console.h"
#include "../fs/vfs.h"
#include "ipc/ipc.h"

static void crash_report(process_t *proc, int sig, pt_regs_t *r)
{
    if (!proc || !r) return;
    const char *sname = "UNKNOWN";
    const char *sdesc = "Fatal Exception";
    if (sig == SIGSEGV) { sname = "SIGSEGV"; sdesc = "Segmentation Fault"; }
    else if (sig == SIGILL) { sname = "SIGILL"; sdesc = "Illegal Instruction"; }
    else if (sig == SIGFPE) { sname = "SIGFPE"; sdesc = "Floating Point Exception"; }
    else if (sig == SIGBUS) { sname = "SIGBUS"; sdesc = "Bus Error"; }
    else if (sig == SIGABRT) { sname = "SIGABRT"; sdesc = "Aborted"; }
    else if (sig == SIGTRAP) { sname = "SIGTRAP"; sdesc = "Trace/Breakpoint Trap"; }

    u64 cr2 = 0;
    if (sig == SIGSEGV || sig == SIGBUS) {
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    kprintf("\n===================================================================\n");
    kprintf("[CRASH] Process '%s' (PID %u) terminated by %s (%s)\n",
            proc->name, proc->pid, sname, sdesc);
    if (sig == SIGSEGV || sig == SIGBUS) {
        kprintf("[CRASH] RIP: 0x%016llx  RSP: 0x%016llx  CR2: 0x%016llx\n",
                (unsigned long long)r->rip, (unsigned long long)r->rsp, (unsigned long long)cr2);
    } else {
        kprintf("[CRASH] RIP: 0x%016llx  RSP: 0x%016llx\n",
                (unsigned long long)r->rip, (unsigned long long)r->rsp);
    }
    kprintf("[CRASH] RAX: 0x%016llx  RBX: 0x%016llx  RCX: 0x%016llx  RDX: 0x%016llx\n",
            (unsigned long long)r->rax, (unsigned long long)r->rbx, (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    kprintf("[CRASH] RSI: 0x%016llx  RDI: 0x%016llx  RBP: 0x%016llx  RFL: 0x%016llx\n",
            (unsigned long long)r->rsi, (unsigned long long)r->rdi, (unsigned long long)r->rbp, (unsigned long long)r->rflags);
    kprintf("[CRASH] R08: 0x%016llx  R09: 0x%016llx  R10: 0x%016llx  R11: 0x%016llx\n",
            (unsigned long long)r->r8, (unsigned long long)r->r9, (unsigned long long)r->r10, (unsigned long long)r->r11);
    kprintf("[CRASH] R12: 0x%016llx  R13: 0x%016llx  R14: 0x%016llx  R15: 0x%016llx\n",
            (unsigned long long)r->r12, (unsigned long long)r->r13, (unsigned long long)r->r14, (unsigned long long)r->r15);
    if (r->rsp && (uintptr_t)r->rsp < 0x8000000000000000ULL) {
        u64 stk[4] = {0};
        if (copy_from_user(stk, (const void *)r->rsp, sizeof(stk)) == 0) {
            kprintf("[CRASH] STK: [0] 0x%016llx  [1] 0x%016llx  [2] 0x%016llx\n",
                    (unsigned long long)stk[0], (unsigned long long)stk[1], (unsigned long long)stk[2]);
        }
    }
    kprintf("===================================================================\n\n");

    /* Also write to /tmp/crash_<pid>.log */
    char logpath[64];
    snprintf(logpath, sizeof(logpath), "/tmp/crash_%u.log", proc->pid);
    file_t *f = vfs_open(logpath, 0x40 | 0x01 | 0x200 /* O_CREAT | O_WRONLY | O_TRUNC */, 0644);
    if (f) {
        char buf[512];
        int len;
        if (sig == SIGSEGV || sig == SIGBUS) {
            len = snprintf(buf, sizeof(buf),
                "Process: %s\nPID: %u\nSignal: %s (%s)\nRIP: 0x%016llx\nRSP: 0x%016llx\nCR2: 0x%016llx\nRAX: 0x%016llx\n",
                proc->name, proc->pid, sname, sdesc,
                (unsigned long long)r->rip, (unsigned long long)r->rsp, (unsigned long long)cr2, (unsigned long long)r->rax);
        } else {
            len = snprintf(buf, sizeof(buf),
                "Process: %s\nPID: %u\nSignal: %s (%s)\nRIP: 0x%016llx\nRSP: 0x%016llx\nRAX: 0x%016llx\n",
                proc->name, proc->pid, sname, sdesc,
                (unsigned long long)r->rip, (unsigned long long)r->rsp, (unsigned long long)r->rax);
        }
        vfs_write(f, buf, len);
        vfs_close(f);
    }

    /* Broadcast desktop toast alert via IPC channel 1 if azwm is alive */
    ipc_channel_t *wm_chan = ipc_channel_find(1);
    if (wm_chan) {
        ipc_msg_t notif_msg;
        memset(&notif_msg, 0, sizeof(notif_msg));
        notif_msg.sender_pid = 0;
        notif_msg.msg_type = 63; /* AZ_WM_NOTIFY */
        notif_msg.length = 256;
        unsigned int *mtype = (unsigned int *)notif_msg.data;
        *mtype = 63; /* AZ_WM_NOTIFY */
        char *title = (char *)(notif_msg.data + 16);
        char *body  = (char *)(notif_msg.data + 16 + 48); /* AZ_WM_NOTIFY_TITLE_MAX is 48 */
        snprintf(title, 48, "Application Crash (%s)", sname);
        snprintf(body, 144, "%s [PID %u] crashed at 0x%llx", proc->name, proc->pid, (unsigned long long)r->rip);
        ipc_channel_send(wm_chan, &notif_msg, false);
        ipc_channel_put(wm_chan);
    }
}

#define SIGDBG 0
#if SIGDBG
#define sdbg(...) kprintf(__VA_ARGS__)
#else
#define sdbg(...) do {} while (0)
#endif

#define SIGFRAME_MAGIC   0x5349474652414d45ULL   /* "SIGFRAME" */

/* RFLAGS bits the CPU/Linux convention says a signal handler must not inherit. */
#define RFL_TF   (1ULL << 8)    /* trap flag — don't single-step the handler   */
#define RFL_DF   (1ULL << 10)   /* direction flag — handler expects it clear   */
#define RFL_RF   (1ULL << 16)   /* resume flag                                 */

/* fxsave64 / fxrstor64 / xsave64 / xrstor64 helpers (defined in switch_to.asm).
 * The FP state the kernel sees here belongs to the interrupted user code — the
 * kernel itself is built -mno-sse and never touches it. We must save/restore the
 * SAME width the context switcher uses (XSAVE incl. AVX when available), or a
 * handler that touches YMM corrupts the interrupted code's upper halves. */
extern void fpu_save_asm(void *area);
extern void fpu_restore_asm(const void *area);
extern void xsave_save_asm(void *area, u64 mask);
extern void xsave_restore_asm(const void *area, u64 mask);
extern u8   g_osxsave_enabled;
extern u64  g_xcr0_mask;

/* Full XSAVE area reserved in the signal frame. Same budget as fpu_state_t, so
 * whatever XCR0 the boot settled on always fits (see FPU_STATE_MAX_SIZE). */
#define SIG_FPSTATE_SIZE FPU_STATE_MAX_SIZE

/* RFLAGS bits a signal handler / sigreturn frame may set. Everything else the
 * CPU consumes on the return (IOPL 12-13, NT 14, RF 16, VM 17, VIF/VIP, and the
 * reserved bits) is forced clear; IF and the reserved-1 bit are forced set.
 * TF (bit 8) is intentionally excluded: a forged sigreturn frame with TF=1
 * would self-arm single-step tracing into the ring-0 SYSRET path. */
#define SIG_RFLAGS_USER_MASK  0x00240CD5ULL   /* CF PF AF ZF SF DF OF AC ID */

/* Snapshot the live user FP/SIMD state into a signal frame.
 *
 * @dst points straight at sigframe_t::fpstate, which the struct declares
 * 64-byte aligned so XSAVE64 can write it in place. Bouncing through a second
 * stack buffer would cost another FPU_STATE_MAX_SIZE bytes of the 16 KB ring-0
 * stack on a path that already carries the whole frame. We always use plain
 * XSAVE (never XSAVEC) here: the restore below rebuilds the header assuming
 * the standard, non-compacted layout. */
static void sig_fpstate_save(u8 *dst)
{
    __builtin_memset(dst, 0, SIG_FPSTATE_SIZE);
    if (g_osxsave_enabled) xsave_save_asm(dst, g_xcr0_mask);
    else                   fpu_save_asm(dst);
}

/* Restore FP/SIMD state from a sigreturn frame. @buf is the kernel's own copy
 * of a fully user-controlled image, sanitised in place: MXCSR reserved bits
 * (→ #GP on fxrstor/xrstor) and, for XSAVE, the 64-byte header (a forged
 * XSTATE_BV / XCOMP_BV also faults xrstor64 in ring 0). */
static void sig_fpstate_restore(u8 *buf)
{
    *(u32 *)(buf + 24) &= 0x0000FFBFu;          /* MXCSR: keep only writable bits */
    if (g_osxsave_enabled) {
        u64 *xhdr = (u64 *)(buf + 512);
        xhdr[0] = g_xcr0_mask;                  /* XSTATE_BV: restore what we manage */
        for (int i = 1; i < 8; i++) xhdr[i] = 0;/* XCOMP_BV=0 (standard) + reserved */
        xsave_restore_asm(buf, g_xcr0_mask);
    } else {
        fpu_restore_asm(buf);
    }
}

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
    u64       uc_fpregs;               /* not exposed to the handler        */
    u64       uc_sigmask;
    u64       _reserved[6];
    /* 64-byte aligned so XSAVE64/XRSTOR64 can work on it in place, both here on
     * the kernel stack and in the copy the frame leaves on the user stack. */
    u8        fpstate[SIG_FPSTATE_SIZE] __attribute__((aligned(64)));
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
    if (!proc || proc->is_zombie) return 0;

    /* A ptrace- or job-control stop is taken here, ahead of any signal: this
     * is the one point on the way back to ring 3 where the thread's pt_regs
     * frame is complete, which is exactly what a stopped process must present
     * to whoever stopped it. */
    if (unlikely(proc->stop_state != PROC_STOP_NONE)) {
        ptrace_check_stop(r);
        if (proc->is_zombie) return 0;
    }

    if (!proc->sig_pending) return 0;

    sigset_t deliverable = proc->sig_pending & ~proc->sig_blocked;
    /* SIGKILL / SIGSTOP are never caught. */
    deliverable &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    if (!deliverable) return 0;

    u32 sig = lowest_signal(deliverable);
    if (!sig) return 0;

    /* Signal-delivery-stop. The tracer gets to see the signal before its
     * disposition runs, and answers with the signal it wants delivered — the
     * same one, a substitute, or none at all. Consuming the pending bit first
     * is what makes suppression mean something. */
    if (unlikely(ptrace_traced(proc))) {
        __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
        int newsig = ptrace_pre_signal(r, (int)sig);
        if (newsig <= 0 || newsig >= _NSIG) return 0;
        sig = (u32)newsig;
        if (proc->is_zombie) return 0;
    }

    sdbg("[SIG] deliver sig=%u pid=%u rsp=%llx rip=%llx nr=%lld\n",
         sig, proc->pid, (unsigned long long)r->rsp,
         (unsigned long long)r->rip, (long long)syscall_nr);

    sigaction_t act = proc->sigactions[sig];
    if (act.sa_handler == SIG_DFL || act.sa_handler == SIG_IGN) {
        __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);

        /*
         * A signal that was blocked when it arrived reaches here once the
         * mask is lifted, and its default action still has to be taken.
         * SIGCHLD, SIGCONT, SIGURG and SIGWINCH default to ignore, and the
         * job-control stop signals are treated the same way because there is
         * no stopped process state to enter; everything else terminates.
         */
        if (act.sa_handler == SIG_DFL) {
            bool ignored = (sig == SIGCHLD || sig == 18 /* SIGCONT */ ||
                            sig == 23 /* SIGURG */ || sig == 28 /* SIGWINCH */);
            bool stops   = (sig == SIGSTOP || sig == 20 /* SIGTSTP */ ||
                            sig == 21 /* SIGTTIN */ || sig == 22 /* SIGTTOU */);
            if (stops) {
                /* This is the delayed half of job control: a stop signal that
                 * was blocked when it arrived takes its action now. The
                 * immediate case is handled in sched_kill_process(). */
                sched_request_stop(proc, (int)sig, PROC_STOP_JOB);
                sched_stop_current();
                return 0;
            }
            if (!ignored) {
                if (sig == SIGSEGV || sig == SIGILL || sig == SIGBUS ||
                    sig == SIGFPE || sig == SIGABRT || sig == SIGTRAP) {
                    crash_report(proc, (int)sig, r);
                }
                sched_kill_process(proc->pid, (int)sig);
                return 0;
            }
        }
        return 0;
    }
    if (!act.sa_restorer) {
        /* Without a restorer we cannot get control back cleanly. Drop the
         * signal rather than corrupt the thread. */
        __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
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

    /* Determine target stack: if SA_ONSTACK is set and an alternate stack
     * is registered and not already active, deliver onto it. Otherwise use the
     * interrupted stack below the 128-byte red zone. */
    u64 sp = r->rsp;
    bool on_altstack = (proc->sas_ss_sp && !(proc->sas_ss_flags & 2 /* SS_DISABLE */) &&
                        r->rsp >= (u64)(uintptr_t)proc->sas_ss_sp &&
                        r->rsp < (u64)(uintptr_t)proc->sas_ss_sp + proc->sas_ss_size);

    /* The frame plus its alignment padding has to fit inside whatever stack we
     * pick. On the alternate stack that is a hard bound: writing below ss_sp
     * would silently scribble over whatever the program put there, and the
     * frame is not small — it carries a full XSAVE image. A stack too small for
     * it is the same error as a stack that cannot be written at all, so it
     * takes the same exit: a fatal SIGSEGV, never a corrupted process. */
    const u64 frame_need = sizeof(sigframe_t) + 24;   /* + alignment slack */

    if ((act.sa_flags & SA_ONSTACK) && proc->sas_ss_sp &&
        !(proc->sas_ss_flags & 2 /* SS_DISABLE */) && !on_altstack) {
        if (proc->sas_ss_size < frame_need) {
            sdbg("[SIG] altstack too small (%llu < %llu)\n",
                 (unsigned long long)proc->sas_ss_size,
                 (unsigned long long)frame_need);
            __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
            sched_kill_process(proc->pid, SIGSEGV);
            return 0;
        }
        sp = (u64)(uintptr_t)proc->sas_ss_sp + proc->sas_ss_size;
    } else {
        sp -= 128;
    }

    if (sp < frame_need) {
        __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
        sched_kill_process(proc->pid, SIGSEGV);
        return 0;
    }

    sp -= sizeof(sigframe_t);
    sp &= ~15ULL;
    sp -= 8;

    sigframe_t f;
    memset(&f, 0, sizeof(f));
    f.restorer   = (u64)(uintptr_t)act.sa_restorer;
    f.magic      = SIGFRAME_MAGIC;
    f.saved_mask = proc->sig_blocked;
    f.regs       = *r;

    /* Snapshot the interrupted FPU/SSE/AVX state so a handler that uses floating
     * point, XMM or YMM registers doesn't corrupt the code it interrupted. */
    sig_fpstate_save(f.fpstate);

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
        __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);
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
    r->rflags &= ~(RFL_TF | RFL_DF | RFL_RF);   /* handler starts DF=0, no single-step */

    /* Block this signal (unless SA_NODEFER) plus the handler's sa_mask for
     * the duration of the handler; rt_sigreturn restores saved_mask.
     * BUG-4: use atomic OR — a concurrent rt_sigprocmask on another thread of
     * the same process does a non-atomic RMW on sig_blocked; plain |= would
     * produce lost mask bits under SMP. */
    __atomic_or_fetch(&proc->sig_blocked, act.sa_mask, __ATOMIC_RELAXED);
    if (!(act.sa_flags & SA_NODEFER))
        __atomic_or_fetch(&proc->sig_blocked, (1ULL << sig), __ATOMIC_RELAXED);

    __atomic_and_fetch(&proc->sig_pending, ~(1ULL << sig), __ATOMIC_SEQ_CST);

    if (act.sa_flags & SA_RESETHAND)
        /* BUG-T fix: use atomic store so a concurrent rt_sigaction() on a
         * sibling thread cannot see a torn write to sa_handler. */
        __atomic_store_n(&proc->sigactions[sig].sa_handler,
                         (sighandler_t)SIG_DFL, __ATOMIC_RELEASE);

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

    /* f.regs is entirely user-controlled. Sanitise every field the CPU consumes
     * on the return path (this syscall returns via IRETQ — see syscall_dispatch):
     *  - a forged RFLAGS with IOPL=3 would give the process ring-3 port I/O, and
     *    IRETQ from CPL0 loads IOPL straight from the stack image;
     *  - a bad CS/SS/DS selector or a non-canonical RIP/RSP faults in ring 0.
     * Force the flat user selectors, clamp RFLAGS to the benign set (+ IF), and
     * range-check RIP/RSP; a corrupt frame kills the process, never the kernel. */
    pt_regs_t nr = f.regs;
    if (nr.rip >= 0x0000800000000000ULL || nr.rsp >= 0x0000800000000000ULL) {
        sched_kill_process(proc->pid, SIGSEGV);
        return -(s64)EFAULT;
    }
    nr.cs = 0x23;
    nr.ss = 0x1B;
    nr.ds = 0x1B;
    nr.rflags = (nr.rflags & SIG_RFLAGS_USER_MASK) | 0x202ULL;   /* + IF, bit1 */

    *r = nr;
    /* Plain store of sig_blocked (process-wide, owner-thread only). Do NOT touch
     * sig_pending here — a concurrent kill() on another CPU may be OR-ing into
     * it; a read-modify-write store would clobber that. */
    /* BUG-P fix: use an atomic RELEASE store so a concurrent rt_sigprocmask()
     * on a sibling thread that does an atomic OR into sig_blocked cannot have
     * its update silently overwritten by our plain store.  We do NOT use
     * fetch_and/fetch_or here because sigreturn must set an exact mask, not
     * merge one — but the store must be atomic to avoid a torn write. */
    u64 new_mask = f.saved_mask & ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    __atomic_store_n(&proc->sig_blocked, new_mask, __ATOMIC_RELEASE);

    /* Restore the FPU/SSE/AVX state the handler ran on top of (sanitised). */
    sig_fpstate_restore(f.fpstate);

    /* Return the value the interrupted context had in RAX so the dispatcher's
     * `regs->rax = <ret>` is a no-op on the restored frame. */
    return (s64)r->rax;
}
