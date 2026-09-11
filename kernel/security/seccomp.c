/* ============================================================================
 * AzamiOS — seccomp(2) SECCOMP_MODE_FILTER: classic-BPF syscall filtering
 * File: kernel/security/seccomp.c
 *
 * SECCOMP_MODE_STRICT (security.c: security_seccomp_check()) is a fixed
 * five-syscall allowlist. This is the real sandboxing primitive: a process
 * attaches one or more classic-BPF programs (the same instruction set 20-
 * year-old packet filters use, repurposed by Linux for this), each of which
 * is run against every syscall the process makes and returns an action —
 * allow it, deny it with a chosen errno, kill the process, trap into a
 * SIGSYS handler, or just log it.
 *
 * Three pieces:
 *   - a classic-BPF interpreter (bpf_run) and a validator (bpf_validate)
 *     that rejects anything the interpreter can't safely execute *before*
 *     it's ever attached — a program is proven in-bounds once, at load
 *     time, rather than bounds-checked on every syscall
 *   - the filter chain itself: an immutable, refcounted singly-linked list
 *     (seccomp_attach_filter() prepends, never mutates a node in place),
 *     because that's what makes fork() sharing (seccomp_filters_share())
 *     and teardown (seccomp_filters_put()) safe without taking a lock on
 *     the hot path — seccomp_filter_run() below never touches the chain's
 *     structure, only walks it
 *   - seccomp_filter_run(), called from syscall_dispatch() for every
 *     syscall once seccomp_mode == SECCOMP_MODE_FILTER
 * ============================================================================ */

#include "security.h"
#include "../mm/kmalloc.h"
#include "../uaccess.h"
#include "../lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

/* One attached program. Immutable after seccomp_attach_filter() builds it:
 * every field below is set once and never changed again, which is what lets
 * seccomp_filter_run() walk the chain with no lock. `prev` is the next-
 * oldest filter (newest-attached is the process's proc->seccomp_filters
 * head); `refcount` counts how many things point at this node — the
 * process(es) whose seccomp_filters can reach it directly or through a
 * newer node's `prev`. */
typedef struct seccomp_filter {
    struct seccomp_filter *prev;
    u32     refcount;
    u16     len;
    sock_filter_t insns[];
} seccomp_filter_t;

/* Every attach/share/put touches refcounts and prev-links across possibly
 * many processes' chains at once (a shared node's refcount is common state),
 * so one global lock guards all of it. Attaching a filter is a rare,
 * cold-path operation — this is never taken on the syscall-dispatch hot
 * path, which only reads already-published, immutable nodes. */
static spinlock_t g_seccomp_lock = SPINLOCK_INIT;

#define BPF_MAXINSNS 4096

/* ── Classic BPF opcode fields (linux/bpf_common.h — stable, unchanged since
 * BSD packet filter) ────────────────────────────────────────────────────── */
#define BPF_CLASS(code)  ((code) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07

#define BPF_MODE(code)   ((code) & 0xe0)
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_MEM   0x60

#define BPF_OP(code)     ((code) & 0xf0)
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0

#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40

#define BPF_SRC(code)    ((code) & 0x08)
#define BPF_K     0x00
#define BPF_X     0x08

#define BPF_RVAL(code)   ((code) & 0x18)
#define BPF_A     0x10

#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX   0x00
#define BPF_TXA   0x80

#define BPF_SCRATCH_MEMWORDS 16

/* bpf_load_abs() — the only addressing mode seccomp programs actually use
 * (BPF_LD|BPF_ABS): read one 4-byte word out of the seccomp_data at byte
 * offset @k. bpf_validate() has already confirmed @k names one of the 16
 * words below, so this never runs off the struct. */
static u32 bpf_load_abs(const seccomp_data_t *sd, u32 k)
{
    const u8 *base = (const u8 *)sd;
    u32 word;
    __builtin_memcpy(&word, base + k, sizeof(word));
    return word;
}

/* Every valid BPF_LD|BPF_ABS offset into seccomp_data: nr, arch, the two
 * halves of instruction_pointer, then the two halves of each of args[0..5].
 * sizeof(seccomp_data_t) == 64, so k must be a multiple of 4 in [0, 60]. */
static bool bpf_offset_valid(u32 k)
{
    return (k < sizeof(seccomp_data_t)) && ((k & 3) == 0);
}

/* bpf_validate() — proves @insns is safe to interpret *before* it is ever
 * attached to a process: every jump target lands inside the program, every
 * absolute load addresses a real seccomp_data field, and control flow can't
 * fall off the end without hitting a BPF_RET. Anything this doesn't
 * recognise is rejected rather than guessed at — an unsupported instruction
 * failing the filter's own load (-EINVAL to the caller) is far better than
 * it being silently ignored and the filter enforcing less than the caller
 * asked for. */
static bool bpf_validate(const sock_filter_t *insns, u16 len)
{
    if (len == 0 || len > BPF_MAXINSNS) return false;

    for (u16 pc = 0; pc < len; pc++) {
        const sock_filter_t *ins = &insns[pc];
        switch (BPF_CLASS(ins->code)) {
        case BPF_LD:
            if (BPF_MODE(ins->code) == BPF_ABS) {
                if (!bpf_offset_valid(ins->k)) return false;
            } else if (BPF_MODE(ins->code) == BPF_IMM) {
                /* k is a plain immediate; nothing to check. */
            } else if (BPF_MODE(ins->code) == BPF_MEM) {
                if (ins->k >= BPF_SCRATCH_MEMWORDS) return false;
            } else {
                return false;
            }
            break;

        case BPF_LDX:
            if (BPF_MODE(ins->code) == BPF_IMM) {
                /* ok */
            } else if (BPF_MODE(ins->code) == BPF_MEM) {
                if (ins->k >= BPF_SCRATCH_MEMWORDS) return false;
            } else {
                return false;
            }
            break;

        case BPF_ALU: {
            u8 op = BPF_OP(ins->code);
            if (op != BPF_ADD && op != BPF_SUB && op != BPF_MUL &&
                op != BPF_DIV && op != BPF_OR  && op != BPF_AND &&
                op != BPF_LSH && op != BPF_RSH && op != BPF_NEG &&
                op != BPF_MOD && op != BPF_XOR)
                return false;
            break;
        }

        case BPF_JMP: {
            u8 op = BPF_OP(ins->code);
            if (op == BPF_JA) {
                /* JA's displacement is the full 32-bit k, not jt/jf. */
                u32 target = (u32)pc + 1 + ins->k;
                if (target >= len) return false;
            } else if (op == BPF_JEQ || op == BPF_JGT || op == BPF_JGE || op == BPF_JSET) {
                u32 t_target = (u32)pc + 1 + ins->jt;
                u32 f_target = (u32)pc + 1 + ins->jf;
                if (t_target >= len || f_target >= len) return false;
            } else {
                return false;
            }
            break;
        }

        case BPF_RET:
            if (BPF_RVAL(ins->code) != BPF_K && BPF_RVAL(ins->code) != BPF_A)
                return false;
            break;

        case BPF_MISC:
            if (BPF_MISCOP(ins->code) != BPF_TAX && BPF_MISCOP(ins->code) != BPF_TXA)
                return false;
            break;

        default:
            return false;
        }
    }

    /* Must end in a return on every fallthrough path. Proving that in
     * general means tracing all reachable paths; requiring the literal
     * last instruction to be a BPF_RET is the same rule real BPF verifiers
     * apply and is what every real filter-generator (libseccomp, Chrome's
     * sandbox, syscall-filter compilers) already emits, since a program
     * that could fall off the end is invalid on Linux too. */
    return BPF_CLASS(insns[len - 1].code) == BPF_RET;
}

/* bpf_run() — interpret an already-validated program against @sd. Every
 * branch bpf_validate() accepted stays inside [0, len), so this never needs
 * its own bounds check on pc. */
static u32 bpf_run(const sock_filter_t *insns, u16 len, const seccomp_data_t *sd)
{
    u32 A = 0, X = 0;
    u32 mem[BPF_SCRATCH_MEMWORDS] = {0};

    for (u32 pc = 0; pc < len; ) {
        const sock_filter_t *ins = &insns[pc];
        u16 code = ins->code;

        switch (BPF_CLASS(code)) {
        case BPF_LD:
            switch (BPF_MODE(code)) {
            case BPF_ABS: A = bpf_load_abs(sd, ins->k); break;
            case BPF_IMM: A = ins->k; break;
            case BPF_MEM: A = mem[ins->k]; break;
            }
            pc++;
            break;

        case BPF_LDX:
            switch (BPF_MODE(code)) {
            case BPF_IMM: X = ins->k; break;
            case BPF_MEM: X = mem[ins->k]; break;
            }
            pc++;
            break;

        case BPF_ALU: {
            u32 operand = (BPF_SRC(code) == BPF_X) ? X : ins->k;
            switch (BPF_OP(code)) {
            case BPF_ADD: A += operand; break;
            case BPF_SUB: A -= operand; break;
            case BPF_MUL: A *= operand; break;
            case BPF_DIV: A = operand ? (A / operand) : 0; break;
            case BPF_MOD: A = operand ? (A % operand) : 0; break;
            case BPF_OR:  A |= operand; break;
            case BPF_AND: A &= operand; break;
            case BPF_XOR: A ^= operand; break;
            case BPF_LSH: A <<= (operand & 31); break;
            case BPF_RSH: A >>= (operand & 31); break;
            case BPF_NEG: A = (u32)(-(s32)A); break;
            }
            pc++;
            break;
        }

        case BPF_JMP: {
            u8 op = BPF_OP(code);
            if (op == BPF_JA) {
                pc += 1 + ins->k;
                break;
            }
            u32 operand = (BPF_SRC(code) == BPF_X) ? X : ins->k;
            bool taken;
            switch (op) {
            case BPF_JEQ:  taken = (A == operand); break;
            case BPF_JGT:  taken = (A >  operand); break;
            case BPF_JGE:  taken = (A >= operand); break;
            case BPF_JSET: taken = (A &  operand) != 0; break;
            default:       taken = false; break;
            }
            pc += 1 + (taken ? ins->jt : ins->jf);
            break;
        }

        case BPF_RET:
            return (BPF_RVAL(code) == BPF_A) ? A : ins->k;

        case BPF_MISC:
            if (BPF_MISCOP(code) == BPF_TAX) X = A;
            else                             A = X;
            pc++;
            break;

        default:
            /* Unreachable: bpf_validate() already rejected this program. */
            return SECCOMP_RET_KILL_PROCESS;
        }
    }

    /* Unreachable for a validated program (bpf_validate requires the last
     * instruction to be BPF_RET). Fail closed if it somehow is. */
    return SECCOMP_RET_KILL_PROCESS;
}

/* ── Attaching a filter ──────────────────────────────────────────────────── */

s64 seccomp_attach_filter(process_t *proc, const sock_fprog_t *ufprog)
{
    if (!proc || !ufprog) return -(s64)EFAULT;

    sock_fprog_t prog;
    if (copy_from_user(&prog, ufprog, sizeof(prog)) != 0) return -(s64)EFAULT;
    if (prog.len == 0 || prog.len > BPF_MAXINSNS || !prog.filter) return -(s64)EINVAL;

    size_t insns_size = (size_t)prog.len * sizeof(sock_filter_t);
    seccomp_filter_t *node = kmalloc(sizeof(seccomp_filter_t) + insns_size);
    if (!node) return -(s64)ENOMEM;

    if (copy_from_user(node->insns, prog.filter, insns_size) != 0) {
        kfree(node);
        return -(s64)EFAULT;
    }

    if (!bpf_validate(node->insns, prog.len)) {
        kfree(node);
        return -(s64)EINVAL;
    }

    node->len      = prog.len;
    node->refcount = 1;   /* the reference proc->seccomp_filters is about to hold */

    irqflags_t irqf = spinlock_lock_irqsave(&g_seccomp_lock);
    node->prev = proc->seccomp_filters;
    proc->seccomp_filters = node;
    spinlock_unlock_irqrestore(&g_seccomp_lock, irqf);

    /* Same rationale as SECCOMP_SET_MODE_STRICT (security.c): once a
     * process is filtering its own syscalls, an execve() into a setuid
     * binary must not be able to shed that confinement by way of gained
     * privilege it didn't have a moment ago. */
    proc->no_new_privs = true;
    proc->seccomp_mode = SECCOMP_MODE_FILTER;
    return 0;
}

/* ── Running the chain ───────────────────────────────────────────────────── */

/* Lower rank = more restrictive = wins when multiple filters disagree,
 * exactly matching Linux's seccomp_run_filters(): every attached filter is
 * evaluated and the single most restrictive result across all of them is
 * what actually happens, so no filter can be undermined by a laxer one
 * stacked on top of it later. */
static int seccomp_action_rank(u32 action)
{
    switch (action & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_KILL_PROCESS: return 0;
    case SECCOMP_RET_KILL_THREAD:  return 1;
    case SECCOMP_RET_TRAP:         return 2;
    case SECCOMP_RET_ERRNO:        return 3;
    case SECCOMP_RET_TRACE:        return 4;
    case SECCOMP_RET_LOG:          return 5;
    case SECCOMP_RET_ALLOW:        return 6;
    default:                       return 0; /* unrecognised: fail closed */
    }
}

u32 seccomp_filter_run(process_t *proc, u64 syscall_nr, const pt_regs_t *regs)
{
    if (!proc || !proc->seccomp_filters) return SECCOMP_RET_ALLOW;

    seccomp_data_t sd;
    sd.nr   = (s32)syscall_nr;
    sd.arch = AUDIT_ARCH_X86_64;
    sd.instruction_pointer = regs->rip;
    sd.args[0] = regs->rdi;
    sd.args[1] = regs->rsi;
    sd.args[2] = regs->rdx;
    sd.args[3] = regs->r10;
    sd.args[4] = regs->r8;
    sd.args[5] = regs->r9;

    /* No lock: every node reachable from proc->seccomp_filters at this
     * instant is immutable and refcounted alive for at least as long as
     * this process can still be making syscalls (see the file banner). */
    u32 best = SECCOMP_RET_ALLOW;
    int best_rank = seccomp_action_rank(best);
    for (seccomp_filter_t *f = proc->seccomp_filters; f; f = f->prev) {
        u32 result = bpf_run(f->insns, f->len, &sd);
        int rank = seccomp_action_rank(result);
        if (rank < best_rank) {
            best = result;
            best_rank = rank;
        }
    }
    return best;
}

/* ── Sharing across fork() and releasing on exit ────────────────────────── */

void seccomp_filters_share(process_t *child, const process_t *parent)
{
    if (!child || !parent) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_seccomp_lock);
    child->seccomp_filters = parent->seccomp_filters;
    if (child->seccomp_filters) child->seccomp_filters->refcount++;
    spinlock_unlock_irqrestore(&g_seccomp_lock, irqf);
}

void seccomp_filters_put(process_t *proc)
{
    if (!proc) return;
    irqflags_t irqf = spinlock_lock_irqsave(&g_seccomp_lock);
    seccomp_filter_t *f = proc->seccomp_filters;
    proc->seccomp_filters = NULL;
    while (f) {
        seccomp_filter_t *prev = f->prev;
        if (--f->refcount != 0) {
            /* Still referenced elsewhere (a sibling that forked off the
             * same chain, or this chain's own `prev` is shared alongside
             * being extended). Whatever's after this node in the chain is
             * therefore held live by that other reference too — stop. */
            break;
        }
        kfree(f);
        f = prev;
    }
    spinlock_unlock_irqrestore(&g_seccomp_lock, irqf);
}
