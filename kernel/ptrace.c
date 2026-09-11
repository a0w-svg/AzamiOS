/* ============================================================================
 * AzamiOS — ptrace(2)
 * File: kernel/ptrace.c
 *
 * See kernel/ptrace.h for where the stops live and what this deliberately does
 * not implement. This file is the request dispatcher plus the four hooks the
 * rest of the kernel calls into.
 * ========================================================================= */

#define DEBUG 0
#include <azami/debug.h>

#include "ptrace.h"
#include "uaccess.h"
#include "signal.h"
#include "sched/sched.h"
#include "security/security.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "../arch/x86_64/mm/vmm.h"
#include "../arch/x86_64/mm/tlb.h"
#include "../include/azami/defs.h"
#include "../include/azami/types.h"
#include "../drivers/char/console.h"

#define USER_ADDR_MAX  0x0000800000000000ULL
#define RFL_TF         (1ULL << 8)

/* ── Access control ──────────────────────────────────────────────────────── */

/* Linux's PTRACE_MODE_ATTACH_REALCREDS, same rule do_process_vm() applies:
 * CAP_SYS_PTRACE, or a tracer whose *real* uid/gid match every one of the
 * target's real, effective and saved ids. Anything weaker would let a process
 * attach to a setuid peer that dropped euid but still holds suid == 0. */
static bool may_trace(process_t *tracer, process_t *target)
{
    if (!tracer || !target || tracer == target) return false;
    if (target->is_zombie) return false;
    if (target->pid <= 1) return false;             /* never the kernel or init */

    if (security_check_permission(tracer, CAP_SYS_PTRACE)) return true;
    if (tracer->euid == 0) return true;

    if (tracer->uid != target->uid  || tracer->uid != target->euid ||
        tracer->uid != target->suid || tracer->gid != target->gid  ||
        tracer->gid != target->egid || tracer->gid != target->sgid)
        return false;

    /* A process that set PR_SET_NO_NEW_PRIVS or entered seccomp is asking not
     * to be steered from outside; only a capable tracer may attach to it. */
    if (target->no_new_privs || target->seccomp_mode != SECCOMP_MODE_DISABLED)
        return false;
    return true;
}

/* Take a counted reference to the process named by @pid so a reaper sweep or a
 * wait4() reap on another CPU cannot free its process_t — its threads, or its
 * address space — while a ptrace request is still walking it. proc_get_by_pid()
 * raises the count under g_sched_lock, the same lock the teardown paths hold to
 * decide a free, so the two can never interleave. Released with proc_put().
 * Returns NULL for a negative, unknown or already-dead pid. */
static process_t *ptrace_pin_pid(s32 pid)
{
    if (pid <= 0) return NULL;
    return proc_get_by_pid((u32)pid);
}

/* Re-apply the checks the old tracee_of() ran on a target that is now already
 * referenced: it must still be alive, this caller must be its tracer, and — for
 * every request but SETOPTIONS/INTERRUPT/KILL — it must be parked. */
static process_t *tracee_of(process_t *tracer, process_t *pinned, bool need_stopped)
{
    process_t *t = pinned;
    if (!t || t->is_zombie) return NULL;
    if (t->tracer_pid != tracer->pid) return NULL;
    if (need_stopped && t->stop_state == PROC_STOP_NONE) return NULL;
    return t;
}

/* The frame a stopped tracee will return to ring 3 on. thread_t::user_regs is
 * set once at thread creation to kernel_stack_top - sizeof(pt_regs_t), which is
 * exactly where both the SYSCALL stub and the ISR stubs build their frame, so
 * it stays valid for the life of the thread. */
static pt_regs_t *tracee_regs(process_t *t)
{
    return (t && t->threads) ? t->threads->user_regs : NULL;
}

/* ── Tracee memory ───────────────────────────────────────────────────────── */

/* Copy @len bytes between kernel @buf and the tracee's virtual @va.
 *
 * Writes go through the kernel's direct map, so a page that is read-only to
 * ring 3 can still be poked — that is what makes it possible to plant an int3
 * in a text page, and it is the same FOLL_FORCE licence Linux grants ptrace.
 * A page shared with another address space (fork() shares read-only frames) is
 * copied first, so a breakpoint in a child never appears in its parent. */
static bool tracee_mem(process_t *tgt, u64 va, void *buf, size_t len, bool write)
{
    if (!tgt || !tgt->pml4_phys) return false;
    if (va >= USER_ADDR_MAX || len > USER_ADDR_MAX - va) return false;

    u8 *kbuf = (u8 *)buf;
    while (len) {
        u64    page  = va & ~(u64)(PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - (size_t)(va - page);
        if (chunk > len) chunk = len;

        u64 fl = vmm_query_flags(tgt->pml4_phys, va);
        if (!(fl & VMM_F_PRESENT) || !(fl & VMM_F_USER)) return false;
        if (fl & VMM_F_HUGE) return false;   /* no user huge pages exist today */

        if (write && ((fl & VMM_F_SHARED) || !(fl & VMM_F_WRITE))) {
            phys_addr_t old = vmm_translate(tgt->pml4_phys, page);
            if (!old) return false;
            phys_addr_t priv = pmm_alloc_page();
            if (!priv) return false;
            memcpy(PHYS_TO_VIRT(priv), PHYS_TO_VIRT(old), PAGE_SIZE);
            /* Keep the tracee's own protection: it must not gain write access
             * to its text just because a debugger patched a byte into it. */
            if (vmm_map(tgt->pml4_phys, page, priv, fl & ~VMM_F_SHARED) != 0) {
                pmm_free_page(priv);
                return false;
            }
            tlb_shootdown_all();
        }

        phys_addr_t phys = vmm_translate(tgt->pml4_phys, va);
        if (!phys) return false;
        u8 *kern = (u8 *)PHYS_TO_VIRT(phys);

        if (write) memcpy(kern, kbuf, chunk);
        else       memcpy(kbuf, kern, chunk);

        va += chunk; kbuf += chunk; len -= chunk;
    }
    return true;
}

/* ── Register marshalling ────────────────────────────────────────────────── */

static void regs_export(const process_t *t, const pt_regs_t *r,
                        struct user_regs_struct *u)
{
    memset(u, 0, sizeof(*u));
    u->r15 = r->r15; u->r14 = r->r14; u->r13 = r->r13; u->r12 = r->r12;
    u->rbp = r->rbp; u->rbx = r->rbx; u->r11 = r->r11; u->r10 = r->r10;
    u->r9  = r->r9;  u->r8  = r->r8;  u->rax = r->rax; u->rcx = r->rcx;
    u->rdx = r->rdx; u->rsi = r->rsi; u->rdi = r->rdi;
    u->orig_rax = t->ptrace_orig_rax;
    u->rip = r->rip; u->cs = r->cs; u->eflags = r->rflags;
    u->rsp = r->rsp; u->ss = r->ss;
    u->ds = r->ds;   u->es = r->ds;  u->fs = 0; u->gs = 0;
    u->fs_base = t->threads && t->threads->has_thread_fs_base ? t->threads->fs_base
                                                              : t->fs_base;
    u->gs_base = t->gs_base;
}

/* Registers come from userspace, and the frame they land in is consumed by
 * SYSRETQ or IRETQ in ring 0. Segment selectors and the non-flag bits of
 * RFLAGS are therefore *not* taken from the tracer: a forged SS, a non-
 * canonical RIP or IOPL=3 in the restored frame is a kernel fault or a
 * privilege escalation, not a debugging feature. TF is the one control bit a
 * tracer may set, because single-stepping is the whole point. */
#define PT_RFLAGS_USER_MASK  0x00240DD5ULL   /* CF PF AF ZF SF TF DF OF AC ID */

static void regs_import(process_t *t, pt_regs_t *r,
                        const struct user_regs_struct *u)
{
    r->r15 = u->r15; r->r14 = u->r14; r->r13 = u->r13; r->r12 = u->r12;
    r->rbp = u->rbp; r->rbx = u->rbx; r->r11 = u->r11; r->r10 = u->r10;
    r->r9  = u->r9;  r->r8  = u->r8;  r->rax = u->rax; r->rcx = u->rcx;
    r->rdx = u->rdx; r->rsi = u->rsi; r->rdi = u->rdi;

    if (u->rip < USER_ADDR_MAX) r->rip = u->rip;
    if (u->rsp < USER_ADDR_MAX) r->rsp = u->rsp;
    r->rflags = (u->eflags & PT_RFLAGS_USER_MASK) | 0x202ULL;   /* + IF, bit 1 */
    r->cs = 0x23; r->ss = 0x1B; r->ds = 0x1B;

    t->ptrace_orig_rax = u->orig_rax;
    if (t->threads && u->fs_base < USER_ADDR_MAX) {
        t->threads->fs_base            = u->fs_base;
        t->threads->has_thread_fs_base = true;
    }
    if (u->gs_base < USER_ADDR_MAX) t->gs_base = u->gs_base;
}

/* ── Entering a stop (tracee side) ───────────────────────────────────────── */

/* Park in a ptrace-stop and return the signal the tracer wants delivered when
 * we resume (0 = none). Runs in the tracee, on a return path to ring 3. */
static int ptrace_stop(pt_regs_t *r, int sig, u32 event)
{
    process_t *p = sched_current_process();
    thread_t  *t = sched_current_thread();
    if (!p || !t) return sig;

    t->user_regs        = r;      /* the frame GETREGS/SETREGS operate on */
    p->ptrace_stop_sig  = sig;
    p->ptrace_event     = event;
    p->ptrace_inject_sig = (event || sig == SIGTRAP) ? 0 : sig;
    p->stop_signal      = sig;
    p->stop_notified    = false;
    p->cont_pending     = false;
    __atomic_store_n(&p->stop_state, PROC_STOP_PTRACE, __ATOMIC_RELEASE);

    sched_stop_current();

    int inject = p->ptrace_inject_sig;
    p->ptrace_inject_sig = 0;
    p->ptrace_event      = 0;
    return inject;
}

/* ── Hooks called from the rest of the kernel ────────────────────────────── */

void ptrace_check_stop(pt_regs_t *r)
{
    process_t *p = sched_current_process();
    if (!p || __atomic_load_n(&p->stop_state, __ATOMIC_ACQUIRE) == PROC_STOP_NONE)
        return;

    thread_t *t = sched_current_thread();
    if (t) t->user_regs = r;
    sched_stop_current();
}

int ptrace_pre_signal(pt_regs_t *r, int sig)
{
    process_t *p = sched_current_process();
    if (!ptrace_traced(p)) return sig;
    if (sig == SIGKILL || sig == SIGSTOP) return sig;   /* never stoppable */

    /* Signal-delivery-stop: the tracer sees the signal before it acts, and its
     * PTRACE_CONT data argument decides what is finally delivered. */
    return ptrace_stop(r, sig, 0);
}

void ptrace_syscall_stop(pt_regs_t *r, bool on_entry)
{
    process_t *p = sched_current_process();
    if (!ptrace_traced(p) || !(p->ptrace_flags & PT_SYSCALL_TRACE)) return;

    if (on_entry) {
        p->ptrace_orig_rax = r->rax;
        p->ptrace_flags |= PT_IN_SYSCALL;
    } else {
        p->ptrace_flags &= ~PT_IN_SYSCALL;
    }

    /* PTRACE_O_TRACESYSGOOD is how a tracer tells a syscall stop apart from a
     * breakpoint without guessing; without it both arrive as a plain SIGTRAP. */
    int sig = (p->ptrace_opts & PTRACE_O_TRACESYSGOOD) ? (SIGTRAP | 0x80) : SIGTRAP;
    int inject = ptrace_stop(r, sig, 0);
    if (inject > 0 && inject < _NSIG)
        __atomic_or_fetch(&p->sig_pending, (1ULL << inject), __ATOMIC_SEQ_CST);
}

bool ptrace_report_trap(pt_regs_t *r, int sig)
{
    process_t *p = sched_current_process();
    if (!ptrace_traced(p)) return false;

    /* A single-step trap leaves TF set in the saved RFLAGS. Clear it so the
     * tracee resumes at full speed unless the tracer steps it again, and clear
     * DR6's sticky status bits so the next #DB is not misread as this one. */
    r->rflags &= ~RFL_TF;
    p->ptrace_flags &= ~PT_SINGLESTEP;
    __asm__ volatile("mov %0, %%dr6" :: "r"(0ULL));

    int inject = ptrace_stop(r, sig, 0);
    if (inject > 0 && inject < _NSIG)
        __atomic_or_fetch(&p->sig_pending, (1ULL << inject), __ATOMIC_SEQ_CST);
    return true;
}

void ptrace_report_event(pt_regs_t *r, u32 event, u64 msg)
{
    process_t *p = sched_current_process();
    if (!ptrace_traced(p)) return;

    u64 want;
    switch (event) {
    case PTRACE_EVENT_EXEC:  want = PTRACE_O_TRACEEXEC;  break;
    case PTRACE_EVENT_EXIT:  want = PTRACE_O_TRACEEXIT;  break;
    case PTRACE_EVENT_FORK:  want = PTRACE_O_TRACEFORK;  break;
    case PTRACE_EVENT_VFORK: want = PTRACE_O_TRACEVFORK; break;
    case PTRACE_EVENT_CLONE: want = PTRACE_O_TRACECLONE; break;
    default: return;
    }
    if (!(p->ptrace_opts & want)) return;

    p->ptrace_msg = msg;
    ptrace_stop(r, SIGTRAP, event);
}

void ptrace_release(process_t *p)
{
    if (!p) return;

    /* As a tracee: forget the tracer, so a later wait4() by it stops matching. */
    p->tracer_pid   = 0;
    p->ptrace_flags = 0;
    p->ptrace_opts  = 0;

    /* As a tracer: a tracee parked in a ptrace-stop is waiting for a resume
     * that is never coming now. Free it (or kill it, if it asked for that). */
    sched_lock();
    process_t *victims[16];
    u32 nvictims = 0, nkill = 0;
    u32 killpids[16];
    for (process_t *q = sched_get_process_list(); q; q = q->next) {
        if (q->tracer_pid != p->pid) continue;
        bool kill_it = (q->ptrace_opts & PTRACE_O_EXITKILL) != 0;
        q->tracer_pid   = 0;
        q->ptrace_flags = 0;
        q->ptrace_opts  = 0;
        /* The kill list travels as pids — sched_kill_process() re-resolves them
         * under the lock. The resume list keeps raw pointers past this unlock,
         * so each one takes a reference that proc_put() drops below. */
        if (kill_it) { if (nkill  < 16) killpids[nkill++] = q->pid; }
        else if (nvictims < 16) { proc_get_locked(q); victims[nvictims++] = q; }
    }
    sched_unlock();

    for (u32 i = 0; i < nvictims; i++) {
        if (victims[i]->stop_state == PROC_STOP_PTRACE)
            sched_resume_process(victims[i], false);
        proc_put(victims[i]);
    }
    for (u32 i = 0; i < nkill; i++)
        sched_kill_process(killpids[i], SIGKILL);
}

/* ── The syscall ─────────────────────────────────────────────────────────── */

/* Shared tail of CONT / SYSCALL / SINGLESTEP / DETACH: install the resume mode,
 * hand the tracee the signal the tracer chose, and let it go. */
static s64 do_resume(process_t *tgt, int sig, u32 set, u32 clear, bool step)
{
    if (sig < 0 || sig >= _NSIG) return -(s64)EIO;

    tgt->ptrace_flags = (tgt->ptrace_flags & ~clear) | set;
    tgt->ptrace_inject_sig = sig;

    pt_regs_t *r = tracee_regs(tgt);
    if (r) {
        if (step) r->rflags |= RFL_TF;
        else      r->rflags &= ~RFL_TF;
    }
    sched_resume_process(tgt, false);
    return 0;
}

/* The request dispatcher. @target is already pinned by the caller for the
 * whole span of this call, so a concurrent free on another CPU cannot turn any
 * dereference of it here into a use-after-free. @pid is kept only for the
 * handful of error paths that still speak in pid terms. */
static s64 ptrace_dispatch(pt_regs_t *r, process_t *me, process_t *target,
                           s64 request, s32 pid, u64 addr, u64 data)
{
    (void)pid;

    switch (request) {

    case PTRACE_ATTACH:
    case PTRACE_SEIZE: {
        process_t *tgt = target;
        if (tgt->tracer_pid) return -(s64)EPERM;
        if (!may_trace(me, tgt)) return -(s64)EPERM;
        if (request == PTRACE_SEIZE) {
            if (addr) return -(s64)EIO;               /* Linux: addr must be 0 */
            if (data & ~(u64)PTRACE_O_MASK) return -(s64)EIO;
            tgt->ptrace_opts = data;
        }
        tgt->tracer_pid   = me->pid;
        tgt->ptrace_flags = PT_TRACED |
                            (request == PTRACE_SEIZE ? PT_SEIZED : 0);
        tgt->ptrace_stop_sig = SIGSTOP;
        tgt->ptrace_event    = 0;
        /* SEIZE attaches without stopping; ATTACH stops as if by SIGSTOP. The
         * tracee parks at its next return to ring 3 and announces itself from
         * there, so the tracer's wait4() sees a genuinely stopped process. */
        if (request == PTRACE_ATTACH)
            sched_request_stop(tgt, SIGSTOP, PROC_STOP_PTRACE);
        return 0;
    }

    case PTRACE_INTERRUPT: {
        process_t *tgt = tracee_of(me, target, false);
        if (!tgt) return -(s64)ESRCH;
        tgt->ptrace_stop_sig = SIGTRAP;
        tgt->ptrace_event    = PTRACE_EVENT_STOP;
        sched_request_stop(tgt, SIGSTOP, PROC_STOP_PTRACE);
        return 0;
    }

    case PTRACE_KILL: {
        process_t *tgt = tracee_of(me, target, false);
        if (!tgt) return -(s64)ESRCH;
        return sched_kill_process(tgt->pid, SIGKILL);
    }

    case PTRACE_DETACH: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        s64 rc = do_resume(tgt, (int)data, 0,
                           PT_TRACED | PT_SYSCALL_TRACE | PT_SINGLESTEP |
                           PT_IN_SYSCALL | PT_SEIZED, false);
        if (rc == 0) {
            tgt->tracer_pid  = 0;
            tgt->ptrace_opts = 0;
        }
        return rc;
    }

    case PTRACE_CONT: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        return do_resume(tgt, (int)data, 0, PT_SYSCALL_TRACE | PT_SINGLESTEP, false);
    }

    case PTRACE_SYSCALL: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        return do_resume(tgt, (int)data, PT_SYSCALL_TRACE, PT_SINGLESTEP, false);
    }

    case PTRACE_SINGLESTEP: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        return do_resume(tgt, (int)data, PT_SINGLESTEP, PT_SYSCALL_TRACE, true);
    }

    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        u64 word = 0;
        if (!tracee_mem(tgt, addr, &word, sizeof(word), false)) return -(s64)EIO;
        /* The raw syscall stores the word at `data`; only glibc's wrapper
         * returns it as the function result. */
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        if (copy_to_user((void *)(uintptr_t)data, &word, sizeof(word)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        u64 word = data;
        if (!tracee_mem(tgt, addr, &word, sizeof(word), true)) return -(s64)EIO;
        return 0;
    }

    case PTRACE_PEEKUSER: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (addr & 7) return -(s64)EIO;
        u64 word = 0;
        if (addr < sizeof(struct user_regs_struct)) {
            pt_regs_t *tr = tracee_regs(tgt);
            if (!tr) return -(s64)EIO;
            struct user_regs_struct u;
            regs_export(tgt, tr, &u);
            word = ((const u64 *)&u)[addr / 8];
        }
        /* Above the register block lies struct user's u_debugreg area, which
         * this kernel does not implement; reading it as zero is what a process
         * with no breakpoints armed would see anyway. */
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        if (copy_to_user((void *)(uintptr_t)data, &word, sizeof(word)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case PTRACE_POKEUSER: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (addr & 7) return -(s64)EIO;
        if (addr >= sizeof(struct user_regs_struct)) return 0;   /* debug regs */
        pt_regs_t *tr = tracee_regs(tgt);
        if (!tr) return -(s64)EIO;
        struct user_regs_struct u;
        regs_export(tgt, tr, &u);
        ((u64 *)&u)[addr / 8] = data;
        regs_import(tgt, tr, &u);
        return 0;
    }

    case PTRACE_GETREGS: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        pt_regs_t *tr = tracee_regs(tgt);
        if (!tr) return -(s64)EIO;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        struct user_regs_struct u;
        regs_export(tgt, tr, &u);
        if (copy_to_user((void *)(uintptr_t)data, &u, sizeof(u)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case PTRACE_SETREGS: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        pt_regs_t *tr = tracee_regs(tgt);
        if (!tr) return -(s64)EIO;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        struct user_regs_struct u;
        if (copy_from_user(&u, (const void *)(uintptr_t)data, sizeof(u)) != 0)
            return -(s64)EFAULT;
        regs_import(tgt, tr, &u);
        return 0;
    }

    case PTRACE_GETFPREGS:
    case PTRACE_SETFPREGS: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!tgt->threads) return -(s64)EIO;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        /* struct user_fpregs_struct is the 512-byte FXSAVE image, which is
         * also the legacy region at the front of an XSAVE area — so the same
         * bytes serve both, whichever variant the boot chose. */
        u8 *area = tgt->threads->fpu_state.buffer;
        if (request == PTRACE_GETFPREGS) {
            if (copy_to_user((void *)(uintptr_t)data, area, 512) != 0)
                return -(s64)EFAULT;
        } else {
            u8 tmp[512];
            if (copy_from_user(tmp, (const void *)(uintptr_t)data, 512) != 0)
                return -(s64)EFAULT;
            /* MXCSR reserved bits would #GP the restore in ring 0. */
            *(u32 *)(tmp + 24) &= 0x0000FFBFu;
            memcpy(area, tmp, 512);
        }
        return 0;
    }

    case PTRACE_GETREGSET:
    case PTRACE_SETREGSET: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;

        struct { void *base; u64 len; } iov;
        if (copy_from_user(&iov, (const void *)(uintptr_t)data, sizeof(iov)) != 0)
            return -(s64)EFAULT;
        if (!iov.base || (u64)(uintptr_t)iov.base >= USER_ADDR_MAX)
            return -(s64)EFAULT;

        u8 buf[512];
        u64 want;
        if (addr == NT_PRSTATUS)      want = sizeof(struct user_regs_struct);
        else if (addr == NT_PRFPREG)  want = 512;
        else                          return -(s64)EINVAL;
        if (iov.len < want) want = iov.len;

        if (request == PTRACE_GETREGSET) {
            if (addr == NT_PRSTATUS) {
                pt_regs_t *tr = tracee_regs(tgt);
                if (!tr) return -(s64)EIO;
                regs_export(tgt, tr, (struct user_regs_struct *)buf);
            } else {
                if (!tgt->threads) return -(s64)EIO;
                memcpy(buf, tgt->threads->fpu_state.buffer, 512);
            }
            if (copy_to_user(iov.base, buf, (size_t)want) != 0) return -(s64)EFAULT;
        } else {
            if (want < (addr == NT_PRSTATUS ? sizeof(struct user_regs_struct) : 512))
                return -(s64)EINVAL;      /* partial writes are not meaningful */
            if (copy_from_user(buf, iov.base, (size_t)want) != 0) return -(s64)EFAULT;
            if (addr == NT_PRSTATUS) {
                pt_regs_t *tr = tracee_regs(tgt);
                if (!tr) return -(s64)EIO;
                regs_import(tgt, tr, (const struct user_regs_struct *)buf);
            } else {
                if (!tgt->threads) return -(s64)EIO;
                *(u32 *)(buf + 24) &= 0x0000FFBFu;
                memcpy(tgt->threads->fpu_state.buffer, buf, 512);
            }
        }
        iov.len = want;
        copy_to_user((void *)(uintptr_t)data, &iov, sizeof(iov));
        return 0;
    }

    case PTRACE_SETOPTIONS: {
        process_t *tgt = tracee_of(me, target, false);
        if (!tgt) return -(s64)ESRCH;
        if (data & ~(u64)PTRACE_O_MASK) return -(s64)EINVAL;
        tgt->ptrace_opts = data;
        return 0;
    }

    case PTRACE_GETEVENTMSG: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        u64 msg = tgt->ptrace_msg;
        if (copy_to_user((void *)(uintptr_t)data, &msg, sizeof(msg)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case PTRACE_GETSIGINFO: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        /* Minimal but honest siginfo_t: the signal that caused the stop, no
         * errno, and SI_USER as the origin. This kernel does not carry a
         * per-signal siginfo through delivery, so si_pid/si_addr would be
         * invented rather than reported — they are left zero. */
        s32 si[32];
        memset(si, 0, sizeof(si));
        si[0] = tgt->ptrace_stop_sig & 0x7f;
        si[2] = 0;                                   /* si_code = SI_USER */
        if (copy_to_user((void *)(uintptr_t)data, si, sizeof(si)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    case PTRACE_SETSIGINFO: {
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!data || data >= USER_ADDR_MAX) return -(s64)EFAULT;
        s32 si[32];
        if (copy_from_user(si, (const void *)(uintptr_t)data, sizeof(si)) != 0)
            return -(s64)EFAULT;
        if (si[0] > 0 && si[0] < _NSIG) tgt->ptrace_stop_sig = si[0];
        return 0;
    }

    case PTRACE_LISTEN: {
        /* Only meaningful for a SEIZEd tracee in a group-stop; there is no
         * separate listening state here, so accept it for the seized case and
         * leave the tracee stopped, which is what LISTEN promises. */
        process_t *tgt = tracee_of(me, target, true);
        if (!tgt) return -(s64)ESRCH;
        if (!(tgt->ptrace_flags & PT_SEIZED)) return -(s64)EIO;
        return 0;
    }

    default:
        return -(s64)EIO;
    }
}

s64 sys_ptrace_impl(pt_regs_t *r)
{
    s64   request = (s64)r->rdi;
    s32   pid     = (s32)r->rsi;
    u64   addr    = r->rdx;
    u64   data    = r->r10;

    process_t *me = sched_current_process();
    if (!me) return -(s64)EPERM;

    /* PTRACE_TRACEME acts on the caller itself — no target to pin. */
    if (request == PTRACE_TRACEME) {
        if (me->tracer_pid) return -(s64)EPERM;
        if (!me->parent)    return -(s64)EPERM;
        me->tracer_pid   = me->parent->pid;
        me->ptrace_flags = PT_TRACED;
        me->ptrace_opts  = 0;
        return 0;
    }

    /* Every other request names a target pid. Pin it for the whole request so
     * a reaper sweep or a wait4() reap on another CPU cannot free it — and its
     * threads and address space — while the dispatcher is still walking it. */
    process_t *target = ptrace_pin_pid(pid);
    if (!target) return -(s64)ESRCH;

    s64 rc = ptrace_dispatch(r, me, target, request, pid, addr, data);

    proc_put(target);
    return rc;
}
