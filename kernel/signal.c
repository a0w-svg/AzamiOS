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
 * next returns to ring 3 runs the handler. Per-thread masks and
 * directed delivery remain incomplete; process-wide masks are a known limit.
 * ============================================================================ */

#include "signal.h"
#include "../arch/x86_64/cpu/cpu.h"
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
#include "security/security.h"

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
    if (r->rsp && (uintptr_t)r->rsp < TASK_SIZE_MAX) {
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

#define FP_XSTATE_MAGIC1 0x46505853U
#define FP_XSTATE_MAGIC2 0x46505845U

struct _fpx_sw_bytes {
    u32 magic1;
    u32 extended_size;
    u64 xfeatures;
    u32 xstate_size;
    u32 padding[7];
};

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

#define SIG_FPSTATE_BUF_SIZE 4096

/* RFLAGS bits a signal handler / sigreturn frame may set. Everything else the
 * CPU consumes on the return (IOPL 12-13, NT 14, RF 16, VM 17, VIF/VIP, and the
 * reserved bits) is forced clear; IF and the reserved-1 bit are forced set.
 * TF (bit 8) is intentionally excluded: a forged sigreturn frame with TF=1
 * would self-arm single-step tracing into the ring-0 SYSRET path. */
#define SIG_RFLAGS_USER_MASK  0x00240CD5ULL   /* CF PF AF ZF SF DF OF AC ID */

/* Snapshot the live user FP/SIMD state into a signal frame.
 * @dst must be 64-byte aligned. */
static void sig_fpstate_save(u8 *dst, u32 dst_size)
{
    __builtin_memset(dst, 0, dst_size);
    if (g_osxsave_enabled) {
        xsave_save_asm(dst, g_xcr0_mask);
        u32 xstate_size = g_xsave_area_size ? g_xsave_area_size : 832;
        u32 ext_size = xstate_size + 4;
        struct _fpx_sw_bytes *sw = (struct _fpx_sw_bytes *)(dst + 464);
        sw->magic1 = FP_XSTATE_MAGIC1;
        sw->extended_size = ext_size;
        sw->xfeatures = g_xcr0_mask;
        sw->xstate_size = xstate_size;
        for (int i = 0; i < 7; i++) sw->padding[i] = 0;
        if (ext_size <= dst_size) {
            *(u32 *)(dst + ext_size - 4) = FP_XSTATE_MAGIC2;
        }
    } else {
        fpu_save_asm(dst);
        struct _fpx_sw_bytes *sw = (struct _fpx_sw_bytes *)(dst + 464);
        sw->magic1 = 0;
        sw->extended_size = 512;
        sw->xfeatures = 0;
        sw->xstate_size = 0;
        for (int i = 0; i < 7; i++) sw->padding[i] = 0;
    }
}

/* Restore FP/SIMD state from a sigreturn frame. @buf is the kernel's own copy
 * of a fully user-controlled image, sanitised in place: MXCSR reserved bits
 * (→ #GP on fxrstor/xrstor) and, for XSAVE, the 64-byte header (a forged
 * XSTATE_BV / XCOMP_BV also faults xrstor64 in ring 0). */
static void sig_fpstate_restore(u8 *buf)
{
    *(u32 *)(buf + 24) &= 0x0000FFBFu;          /* MXCSR: keep only writable bits */
    struct _fpx_sw_bytes *sw = (struct _fpx_sw_bytes *)(buf + 464);
    if (g_osxsave_enabled && sw->magic1 == FP_XSTATE_MAGIC1) {
        u64 *xhdr = (u64 *)(buf + 512);
        xhdr[0] = g_xcr0_mask & sw->xfeatures;  /* XSTATE_BV: restore valid managed features */
        for (int i = 1; i < 8; i++) xhdr[i] = 0;/* XCOMP_BV=0 (standard) + reserved */
        xsave_restore_asm(buf, g_xcr0_mask);
    } else if (g_osxsave_enabled) {
        u64 *xhdr = (u64 *)(buf + 512);
        xhdr[0] = g_xcr0_mask;
        for (int i = 1; i < 8; i++) xhdr[i] = 0;
        xsave_restore_asm(buf, g_xcr0_mask);
    } else {
        fpu_restore_asm(buf);
    }
}

typedef struct {
    u64 ss_sp;
    s32 ss_flags;
    s32 __pad;
    u64 ss_size;
} stack_uapi_t;

typedef union {
    struct {
        u64 r8, r9, r10, r11, r12, r13, r14, r15;
        u64 rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
        u64 eflags;
        u16 cs, gs, fs, ss;
        u64 err, trapno, oldmask, cr2;
        u64 fpstate;
        u64 reserved1[8];
    };
    struct {
        u64 gregs[23];
        u64 fpregs;
        u64 __reserved1[8];
    };
} sigcontext_uapi_t;

typedef struct {
    u64               uc_flags;
    u64               uc_link;
    stack_uapi_t      uc_stack;
    sigcontext_uapi_t uc_mcontext;
    u64               uc_sigmask;
} ucontext_uapi_t;

/* Linux x86-64 struct rt_sigframe layout:
 * - pretcode: 8 bytes at (%rsp)
 * - uc: struct ucontext (304 bytes, 64-byte aligned when (rsp+8)%64 == 0)
 * - siginfo: struct siginfo (128 bytes)
 * - __pad: 16 bytes (padding so that (rsp + 456) % 64 == 0)
 * fpstate (XSAVE/FXSAVE) follows immediately on the user stack at (rsp + 456).
 */
typedef struct {
    u64             pretcode;
    ucontext_uapi_t uc;
    u8              siginfo[128];
    u8              __pad[16];
} sigframe_t;

static inline u32 lowest_signal(sigset_t s)
{
    for (u32 i = 1; i < _NSIG; i++)
        if (s & (SIG_MASK(i))) return i;
    return 0;
}

int signal_deliver_pending(pt_regs_t *r, s64 syscall_nr)
{
    /* Only when we are actually about to return to ring 3. */
    if ((r->cs & 3) != 3) return 0;

    thread_t *thread = sched_current_thread();
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

    sigset_t blocked = thread ? thread->sig_blocked : proc->sig_blocked;
    sigset_t t_pend = thread ? thread->sig_pending : 0;
    sigset_t p_pend = proc->sig_pending;

    if (!t_pend && !p_pend) return 0;

    /* Check thread-directed deliverable signals first */
    sigset_t t_deliv = t_pend & ~blocked;
    t_deliv &= ~((SIG_MASK(SIGKILL)) | (SIG_MASK(SIGSTOP)));

    /* Then process-directed deliverable signals */
    sigset_t p_deliv = p_pend & ~blocked;
    p_deliv &= ~((SIG_MASK(SIGKILL)) | (SIG_MASK(SIGSTOP)));

    u32 sig = 0;
    bool is_thread_sig = false;

    if (t_deliv) {
        sig = lowest_signal(t_deliv);
        is_thread_sig = true;
    } else if (p_deliv) {
        sig = lowest_signal(p_deliv);
        is_thread_sig = false;
    }

    if (!sig) return 0;

    /* Signal-delivery-stop. The tracer gets to see the signal before its
     * disposition runs, and answers with the signal it wants delivered — the
     * same one, a substitute, or none at all. Consuming the pending bit first
     * is what makes suppression mean something. */
    if (unlikely(ptrace_traced(proc))) {
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }
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
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }

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
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }
        return 0;
    }

    /* Interrupted syscall: restart it if the handler asked for SA_RESTART,
     * otherwise leave the -EINTR the syscall already returned. The `syscall`
     * instruction is 2 bytes, and r->rip points just past it. */
    if (syscall_nr >= 0 && syscall_nr != SYS_epoll_wait &&
        syscall_nr != SYS_epoll_pwait && syscall_nr != SYS_epoll_pwait2 &&
        (s64)r->rax == -(s64)EINTR && (act.sa_flags & SA_RESTART)) {
        r->rip -= 2;
        r->rax  = (u64)syscall_nr;
    }

    /* Determine target stack: if SA_ONSTACK is set and an alternate stack
     * is registered and not already active, deliver onto it. Otherwise use the
     * interrupted stack below the 128-byte red zone. */
    void *ss_sp = thread ? thread->sas_ss_sp : proc->sas_ss_sp;
    size_t ss_size = thread ? thread->sas_ss_size : proc->sas_ss_size;
    int ss_flags = thread ? thread->sas_ss_flags : proc->sas_ss_flags;

    u64 sp = r->rsp;
    bool on_altstack = (ss_sp && !(ss_flags & 2 /* SS_DISABLE */) &&
                        r->rsp >= (u64)(uintptr_t)ss_sp &&
                        r->rsp < (u64)(uintptr_t)ss_sp + ss_size);

    /* The frame plus its alignment padding has to fit inside whatever stack we
     * pick. On the alternate stack that is a hard bound: writing below ss_sp
     * would silently scribble over whatever the program put there, and the
     * frame is not small — it carries a full XSAVE image. A stack too small for
     * it is the same error as a stack that cannot be written at all, so it
     * takes the same exit: a fatal SIGSEGV, never a corrupted process. */
    u32 fp_size = g_osxsave_enabled ? (g_xsave_area_size ? g_xsave_area_size + 4 : 836) : 512;
    if (fp_size > SIG_FPSTATE_BUF_SIZE) fp_size = SIG_FPSTATE_BUF_SIZE;

    const u64 frame_need = sizeof(sigframe_t) + fp_size + 128;   /* + alignment and red zone slack */

    if ((act.sa_flags & SA_ONSTACK) && ss_sp &&
        !(ss_flags & 2 /* SS_DISABLE */) && !on_altstack) {
        if (ss_size < frame_need) {
            sdbg("[SIG] altstack too small (%llu < %llu)\n",
                 (unsigned long long)ss_size,
                 (unsigned long long)frame_need);
            if (is_thread_sig && thread) {
                __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
            } else {
                __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
            }
            sched_kill_process(proc->pid, SIGSEGV);
            return 0;
        }
        sp = (u64)(uintptr_t)ss_sp + ss_size;
    } else {
        sp -= 128;
    }

    if (sp < frame_need || sp >= TASK_SIZE_MAX) {
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }
        sched_kill_process(proc->pid, SIGSEGV);
        return 0;
    }

    sp -= (sizeof(sigframe_t) + fp_size);
    sp = (sp & ~63ULL) - 8;

    if (sp < g_mmap_min_addr || sp >= TASK_SIZE_MAX) {
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }
        sched_kill_process(proc->pid, SIGSEGV);
        return 0;
    }

    /* 64-byte aligned kernel buffer for XSAVE/FXSAVE */
    u8 fp_buf[SIG_FPSTATE_BUF_SIZE] __attribute__((aligned(64)));
    sig_fpstate_save(fp_buf, sizeof(fp_buf));

    sigframe_t f;
    memset(&f, 0, sizeof(f));
    f.pretcode = (u64)(uintptr_t)act.sa_restorer;

    u64 saved_mask = thread && thread->restore_signal_mask
                 ? thread->saved_signal_mask : (thread ? thread->sig_blocked : proc->sig_blocked);

    f.uc.uc_flags = 0;
    f.uc.uc_link  = 0;
    f.uc.uc_stack.ss_sp = (u64)(uintptr_t)ss_sp;
    f.uc.uc_stack.ss_flags = on_altstack ? 1 /* SS_ONSTACK */ : ((ss_flags & 2) ? 2 /* SS_DISABLE */ : 0);
    f.uc.uc_stack.ss_size = ss_size;
    f.uc.uc_sigmask = saved_mask;

    f.uc.uc_mcontext.r8  = r->r8;
    f.uc.uc_mcontext.r9  = r->r9;
    f.uc.uc_mcontext.r10 = r->r10;
    f.uc.uc_mcontext.r11 = r->r11;
    f.uc.uc_mcontext.r12 = r->r12;
    f.uc.uc_mcontext.r13 = r->r13;
    f.uc.uc_mcontext.r14 = r->r14;
    f.uc.uc_mcontext.r15 = r->r15;
    f.uc.uc_mcontext.rdi = r->rdi;
    f.uc.uc_mcontext.rsi = r->rsi;
    f.uc.uc_mcontext.rbp = r->rbp;
    f.uc.uc_mcontext.rbx = r->rbx;
    f.uc.uc_mcontext.rdx = r->rdx;
    f.uc.uc_mcontext.rax = r->rax;
    f.uc.uc_mcontext.rcx = r->rcx;
    f.uc.uc_mcontext.rsp = r->rsp;
    f.uc.uc_mcontext.rip = r->rip;
    f.uc.uc_mcontext.eflags = r->rflags;
    f.uc.uc_mcontext.cs  = r->cs;
    f.uc.uc_mcontext.gs  = 0;
    f.uc.uc_mcontext.fs  = 0;
    f.uc.uc_mcontext.ss  = r->ss;
    f.uc.uc_mcontext.err = 0;
    f.uc.uc_mcontext.trapno = 0;
    f.uc.uc_mcontext.oldmask = 0;
    f.uc.uc_mcontext.cr2 = 0;
    f.uc.uc_mcontext.fpstate = sp + sizeof(sigframe_t);

    /* Minimal Linux-compatible siginfo: si_signo, si_errno, si_code. */
    ((s32 *)f.siginfo)[0] = (s32)sig;
    ((s32 *)f.siginfo)[1] = 0;                       /* si_errno */
    if (sig == SIGSEGV || sig == SIGBUS) {
        u64 cr2 = 0;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        f.uc.uc_mcontext.cr2 = cr2;
        *(u64 *)(f.siginfo + 16) = cr2;             /* si_addr */
        ((s32 *)f.siginfo)[2] = 1;                   /* SEGV_MAPERR / BUS_ADRERR */
    } else {
        ((s32 *)f.siginfo)[2] = 0;                   /* SI_USER */
    }

    if (copy_to_user((void *)(uintptr_t)sp, &f, sizeof(f)) != 0 ||
        copy_to_user((void *)(uintptr_t)(sp + sizeof(sigframe_t)), fp_buf, fp_size) != 0) {
        /* Bad user stack — turn this into a fatal SIGSEGV. */
        if (is_thread_sig && thread) {
            __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        } else {
            __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
        }
        sched_kill_process(proc->pid, SIGSEGV);
        return 0;
    }

    /* The handler runs under the temporary mask, but sigreturn must restore
     * the mask from before epoll_pwait/sigsuspend. Hand ownership to the saved frame. */
    if (thread) thread->restore_signal_mask = false;

    /* Enter the handler. */
    u64 uctx = sp + __builtin_offsetof(sigframe_t, uc);
    u64 sinfo = sp + __builtin_offsetof(sigframe_t, siginfo);

    r->rip = (u64)(uintptr_t)act.sa_handler;
    r->rsp = sp;
    r->rdi = sig;
    r->rsi = sinfo;      /* meaningful only with SA_SIGINFO */
    r->rdx = uctx;       /* meaningful only with SA_SIGINFO */
    r->rax = 0;
    r->rflags &= ~(RFL_TF | RFL_DF | RFL_RF);   /* handler starts DF=0, no single-step */

    /* Block this signal (unless SA_NODEFER) plus the handler's sa_mask for
     * the duration of the handler; rt_sigreturn restores saved_mask. */
    if (thread) {
        __atomic_or_fetch(&thread->sig_blocked, act.sa_mask, __ATOMIC_RELAXED);
        if (!(act.sa_flags & SA_NODEFER))
            __atomic_or_fetch(&thread->sig_blocked, (SIG_MASK(sig)), __ATOMIC_RELAXED);
        __atomic_store_n(&proc->sig_blocked, thread->sig_blocked, __ATOMIC_RELAXED);
    } else {
        __atomic_or_fetch(&proc->sig_blocked, act.sa_mask, __ATOMIC_RELAXED);
        if (!(act.sa_flags & SA_NODEFER))
            __atomic_or_fetch(&proc->sig_blocked, (SIG_MASK(sig)), __ATOMIC_RELAXED);
    }

    if (is_thread_sig && thread) {
        __atomic_and_fetch(&thread->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
    } else {
        __atomic_and_fetch(&proc->sig_pending, ~(SIG_MASK(sig)), __ATOMIC_SEQ_CST);
    }

    if (act.sa_flags & SA_RESETHAND)
        /* BUG-T fix: use atomic store so a concurrent rt_sigaction() on a
         * sibling thread cannot see a torn write to sa_handler. */
        __atomic_store_n(&proc->sigactions[sig].sa_handler,
                         (sighandler_t)SIG_DFL, __ATOMIC_RELEASE);

    sdbg("[SIG]  -> handler=%llx restorer=%llx newrsp=%llx\n",
         (unsigned long long)r->rip, (unsigned long long)f.pretcode,
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
    if (copy_from_user(&f, (const void *)(uintptr_t)base, sizeof(f)) != 0) {
        sched_kill_process(proc->pid, SIGSEGV);
        return -(s64)EFAULT;
    }

    /* f.uc.uc_mcontext is entirely user-controlled. Sanitise every field the CPU consumes
     * on the return path (this syscall returns via IRETQ — see syscall_dispatch):
     *  - a forged RFLAGS with IOPL=3 would give the process ring-3 port I/O, and
     *    IRETQ from CPL0 loads IOPL straight from the stack image;
     *  - a bad CS/SS/DS selector or a non-canonical RIP/RSP faults in ring 0.
     * Force the flat user selectors, clamp RFLAGS to the benign set (+ IF), and
     * range-check RIP/RSP; a corrupt frame kills the process, never the kernel. */
    u64 rip = f.uc.uc_mcontext.rip;
    u64 rsp = f.uc.uc_mcontext.rsp;
    u64 rflags = f.uc.uc_mcontext.eflags;

    if (rip >= TASK_SIZE_MAX || rip < g_mmap_min_addr ||
        rsp >= TASK_SIZE_MAX || rsp < g_mmap_min_addr) {
        sched_kill_process(proc->pid, SIGSEGV);
        return -(s64)EFAULT;
    }

    r->r8  = f.uc.uc_mcontext.r8;
    r->r9  = f.uc.uc_mcontext.r9;
    r->r10 = f.uc.uc_mcontext.r10;
    r->r11 = f.uc.uc_mcontext.r11;
    r->r12 = f.uc.uc_mcontext.r12;
    r->r13 = f.uc.uc_mcontext.r13;
    r->r14 = f.uc.uc_mcontext.r14;
    r->r15 = f.uc.uc_mcontext.r15;
    r->rdi = f.uc.uc_mcontext.rdi;
    r->rsi = f.uc.uc_mcontext.rsi;
    r->rbp = f.uc.uc_mcontext.rbp;
    r->rbx = f.uc.uc_mcontext.rbx;
    r->rdx = f.uc.uc_mcontext.rdx;
    r->rax = f.uc.uc_mcontext.rax;
    r->rcx = f.uc.uc_mcontext.rcx;
    r->rsp = rsp;
    r->rip = rip;

    r->cs = 0x23;
    r->ss = 0x1B;
    r->ds = 0x1B;
    r->rflags = (rflags & SIG_RFLAGS_USER_MASK) | 0x202ULL;   /* + IF, bit1 */

    u64 new_mask = f.uc.uc_sigmask & ~((SIG_MASK(SIGKILL)) | (SIG_MASK(SIGSTOP)));
    thread_t *thread = sched_current_thread();
    if (thread) {
        __atomic_store_n(&thread->sig_blocked, new_mask, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&proc->sig_blocked, new_mask, __ATOMIC_RELEASE);

    /* Restore the FPU/SSE/AVX state the handler ran on top of (sanitised). */
    if (f.uc.uc_mcontext.fpstate) {
        u64 fp_ptr = f.uc.uc_mcontext.fpstate;
        u32 fp_size = g_osxsave_enabled ? (g_xsave_area_size ? g_xsave_area_size + 4 : 836) : 512;
        if (fp_size > SIG_FPSTATE_BUF_SIZE) fp_size = SIG_FPSTATE_BUF_SIZE;

        if (fp_ptr >= TASK_SIZE_MAX || fp_ptr < g_mmap_min_addr || (fp_ptr & 63) != 0) {
            sched_kill_process(proc->pid, SIGSEGV);
            return -(s64)EFAULT;
        }

        u8 fp_buf[SIG_FPSTATE_BUF_SIZE] __attribute__((aligned(64)));
        if (copy_from_user(fp_buf, (const void *)(uintptr_t)fp_ptr, fp_size) != 0) {
            sched_kill_process(proc->pid, SIGSEGV);
            return -(s64)EFAULT;
        }
        sig_fpstate_restore(fp_buf);
    }

    /* Return the value the interrupted context had in RAX so the dispatcher's
     * `regs->rax = <ret>` is a no-op on the restored frame. */
    return (s64)r->rax;
}

/* A masked wait interrupted without a delivered handler still restores its
 * original mask before returning to user mode. Called after signal selection. */
void signal_restore_wait_mask(void)
{
    thread_t *thread = sched_current_thread();
    process_t *proc = sched_current_process();
    if (thread && thread->restore_signal_mask) {
        __atomic_store_n(&thread->sig_blocked, thread->saved_signal_mask, __ATOMIC_RELEASE);
        if (proc) __atomic_store_n(&proc->sig_blocked, thread->saved_signal_mask, __ATOMIC_RELEASE);
        thread->restore_signal_mask = false;
    }
}
