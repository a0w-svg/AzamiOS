/* ============================================================================
 * AzamiOS — IDT Initialisation & Exception Dispatch
 * File: arch/x86_64/cpu/idt.c
 * ============================================================================ */

#include "idt.h"
#include "gdt.h"     /* SEL_KERNEL_CODE */
#include "smp.h"     /* smp_get_cpu */
#include "pic.h"     /* pic_eoi() */
#include "../../../drivers/char/console.h"
#include "../../../include/azami/defs.h"
#include "../../../hal/irq.h"
#include "../../../kernel/uaccess.h"
#include "../../../kernel/sched/sched.h"
#include "../../../kernel/mm/pmm.h"
#include "../../../kernel/mm/vma.h"
#include "../../../kernel/signal.h"
#include "../../../kernel/ptrace.h"
#include "../mm/vmm.h"
#include "hwaccel.h"
#include "cpu.h"
#include "mce.h"

/* ── IDT storage (256 entries × 16 bytes = 4 KB, page-aligned) ───────────── */
static idt_entry_t g_idt[256] __aligned(4096);
static idt_ptr_t   g_idt_ptr;

/* ── Forward declarations of all ISR stubs (defined in isr.asm) ──────────── */
#define DECL_ISR(n)  extern void isr_##n(void)

/* Exceptions 0–31 */
DECL_ISR(0);  DECL_ISR(1);  DECL_ISR(2);  DECL_ISR(3);
DECL_ISR(4);  DECL_ISR(5);  DECL_ISR(6);  DECL_ISR(7);
DECL_ISR(8);  DECL_ISR(9);  DECL_ISR(10); DECL_ISR(11);
DECL_ISR(12); DECL_ISR(13); DECL_ISR(14); DECL_ISR(15);
DECL_ISR(16); DECL_ISR(17); DECL_ISR(18); DECL_ISR(19);
DECL_ISR(20); DECL_ISR(21); DECL_ISR(22); DECL_ISR(23);
DECL_ISR(24); DECL_ISR(25); DECL_ISR(26); DECL_ISR(27);
DECL_ISR(28); DECL_ISR(29); DECL_ISR(30); DECL_ISR(31);

/* IRQs 32–47 */
DECL_ISR(32);  DECL_ISR(33);  DECL_ISR(34);  DECL_ISR(35);
DECL_ISR(36);  DECL_ISR(37);  DECL_ISR(38);  DECL_ISR(39);
DECL_ISR(40);  DECL_ISR(41);  DECL_ISR(42);  DECL_ISR(43);
DECL_ISR(44);  DECL_ISR(45);  DECL_ISR(46);  DECL_ISR(47);

/* LAPIC timer (48), TLB shootdown (251), spurious (255) */
DECL_ISR(48);
DECL_ISR(49);  /* SMP reschedule IPI — also declared explicitly below */
DECL_ISR(251);
DECL_ISR(255);

/* BUG-D fix: dedicated stub for unhandled vectors that issues an EOI and
 * returns without touching the exception path.  isr_0 (#DE) was the previous
 * fallback, which caused spurious interrupts to be dispatched as
 * divide-by-zero and kill user processes with SIGFPE. */
extern void isr_spurious(void);

/* ── Exception name table ─────────────────────────────────────────────────── */
static const char *const g_exc_names[32] = {
    "#DE Divide Error",              "#DB Debug",
    "NMI Interrupt",                 "#BP Breakpoint",
    "#OF Overflow",                  "#BR BOUND Range Exceeded",
    "#UD Invalid Opcode",            "#NM Device Not Available",
    "#DF Double Fault",              "Coprocessor Segment Overrun",
    "#TS Invalid TSS",               "#NP Segment Not Present",
    "#SS Stack-Segment Fault",       "#GP General Protection Fault",
    "#PF Page Fault",                "Reserved",
    "#MF x87 FPU Error",             "#AC Alignment Check",
    "#MC Machine Check",             "#XM SIMD FP Exception",
    "#VE Virtualization Exception",  "#CP Control Protection",
    "Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved",           "#HV Hypervisor Injection",
    "#VC VMM Communication",         "#SX Security Exception",
    "Reserved"
};

/* ── APIC vector → handler table ─────────────────────────────────────────── */

typedef struct {
    irq_handler_fn_t fn;
    void            *ctx;
} irq_slot_t;

static irq_slot_t g_irq_table[224];  /* vectors 32–255 */

/* ── Internal gate installer ─────────────────────────────────────────────── */
void idt_set_gate(u8 vector, uintptr_t handler, u8 ist, u8 dpl, u8 type)
{
    idt_entry_t *e = &g_idt[vector];
    e->offset_low  = (u16)(handler & 0xFFFFU);
    e->selector    = SEL_KERNEL_CODE;
    e->ist         = ist & 0x7U;
    e->type_attr   = (u8)(0x80U              /* P = present */
                         | ((dpl & 3U) << 5) /* DPL */
                         | (type & 0xFU));   /* gate type */
    e->offset_mid  = (u16)((handler >> 16) & 0xFFFFU);
    e->offset_high = (u32)((handler >> 32) & 0xFFFFFFFFU);
    e->reserved    = 0;
}

/* ── IDT initialisation ──────────────────────────────────────────────────── */
void idt_init(void)
{
    /* Install all exception stubs */
#define SET(v, ist, dpl)  idt_set_gate(v, (uintptr_t)isr_##v, ist, dpl, IDT_TYPE_INT_GATE)

    SET(0,  IST_NONE, 0); SET(1,  IST_DBG,  0);
    SET(2,  IST_NMI,  0); SET(3,  IST_DBG,  3); /* BP: DPL=3 (int 3 from user) */
    SET(4,  IST_NONE, 0); SET(5,  IST_NONE, 0);
    SET(6,  IST_NONE, 0); SET(7,  IST_NONE, 0);
    SET(8,  IST_DF,   0); /* #DF: dedicated stack via IST1 */
    SET(9,  IST_NONE, 0); SET(10, IST_NONE, 0); SET(11, IST_NONE, 0);
    SET(12, IST_NONE, 0); SET(13, IST_NONE, 0); SET(14, IST_NONE, 0);
    SET(15, IST_NONE, 0); SET(16, IST_NONE, 0); SET(17, IST_NONE, 0);
    SET(18, IST_MC,   0); /* #MC: dedicated stack via IST3 */
    SET(19, IST_NONE, 0); SET(20, IST_NONE, 0); SET(21, IST_NONE, 0);
    SET(22, IST_NONE, 0); SET(23, IST_NONE, 0); SET(24, IST_NONE, 0);
    SET(25, IST_NONE, 0); SET(26, IST_NONE, 0); SET(27, IST_NONE, 0);
    SET(28, IST_NONE, 0); SET(29, IST_NONE, 0); SET(30, IST_NONE, 0);
    SET(31, IST_NONE, 0);
#undef SET

    /* Hardware IRQ stubs (vectors 32–47) */
    idt_set_gate(32, (uintptr_t)isr_32, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(33, (uintptr_t)isr_33, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(34, (uintptr_t)isr_34, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(35, (uintptr_t)isr_35, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(36, (uintptr_t)isr_36, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(37, (uintptr_t)isr_37, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(38, (uintptr_t)isr_38, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(39, (uintptr_t)isr_39, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(40, (uintptr_t)isr_40, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(41, (uintptr_t)isr_41, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(42, (uintptr_t)isr_42, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(43, (uintptr_t)isr_43, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(44, (uintptr_t)isr_44, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(45, (uintptr_t)isr_45, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(46, (uintptr_t)isr_46, IST_NONE, 0, IDT_TYPE_INT_GATE);
    idt_set_gate(47, (uintptr_t)isr_47, IST_NONE, 0, IDT_TYPE_INT_GATE);

    /* LAPIC timer */
    idt_set_gate(48,  (uintptr_t)isr_48,  IST_NONE, 0, IDT_TYPE_INT_GATE);
    /* SMP Reschedule IPI */
    extern void isr_49(void);
    idt_set_gate(49,  (uintptr_t)isr_49,  IST_NONE, 0, IDT_TYPE_INT_GATE);
    /* TLB shootdown IPI */
    idt_set_gate(251, (uintptr_t)isr_251, IST_NONE, 0, IDT_TYPE_INT_GATE);
    /* LAPIC spurious */
    idt_set_gate(255, (uintptr_t)isr_255, IST_NONE, 0, IDT_TYPE_INT_GATE);

    /* BUG-D fix: fill any remaining gates with a dedicated spurious handler
     * instead of isr_0 (#DE) — unhandled device IRQs now EOI cleanly. */
    for (int v = 50; v <= 250; v++) {
        if (g_idt[v].offset_low == 0)
            idt_set_gate((u8)v, (uintptr_t)isr_spurious, IST_NONE, 0, IDT_TYPE_INT_GATE);
    }

    g_idt_ptr.limit = (u16)(sizeof(g_idt) - 1);
    g_idt_ptr.base  = (u64)(uintptr_t)g_idt;

    __asm__ volatile("lidt %0" : : "m"(g_idt_ptr) : "memory");

    kprintf("[IDT] 256-entry IDT loaded at 0x%016llx\n",
            (unsigned long long)g_idt_ptr.base);
}

/* ── IRQ handler registration ────────────────────────────────────────────── */
void idt_register_irq(u8 vector, irq_handler_fn_t fn, void *ctx)
{
    if (vector < 32) return;  /* Protect exception vectors */
    g_irq_table[vector - 32].fn  = fn;
    g_irq_table[vector - 32].ctx = ctx;
}

/* ── Main C dispatcher (called from isr_common_stub) ────────────────────── */
static void isr_dispatch_inner(pt_regs_t *r);

void isr_dispatch(pt_regs_t *r)
{
    isr_dispatch_inner(r);
    /* On the way back to ring 3 (this frame, or a frame reactivated by a
     * context switch inside the handler), run any pending user signal. */
    signal_deliver_pending(r, -1);
}

/* Resolve user-space demand paging and copy-on-write faults.
 * Returns true if the fault was resolved and execution can resume.
 * Applicable to both ring-3 user faults and ring-0 uaccess faults (copy_to_user/copy_from_user). */
static bool handle_user_page_fault(pt_regs_t *r, uintptr_t fault_addr)
{
    extern process_t *sched_current_process(void);
    process_t *proc = sched_current_process();
    if (!proc || fault_addr >= 0x0000800000000000ULL)
        return false;

    /* ── Copy-on-Write: present page, write fault ──────────── */
    if ((r->err_code & 3) == 3) {
        u64 pte_fl = vmm_query_flags(proc->pml4_phys, fault_addr);
        if (pte_fl & (1ULL << 10) /* VMM_F_COW */) {
            u32 vma_prot = 0;
            bool vma_ok = vma_probe(proc, fault_addr, &vma_prot);
            /* VMM_F_COW is only set on pages that were writable before fork.
             * Accept unless the VMA was deliberately set to PROT_NONE. */
            if (!vma_ok || vma_prot != 0) {
                if (vmm_cow_fault(proc->pml4_phys, fault_addr) == 0) {
                    proc->nr_minor_faults++;
                    return true; /* COW resolved; resume instruction */
                }
            }
            return false;
        }
    }

    /* Demand paging: only grow regions the kernel actually manages lazily */
    if ((r->err_code & 1) == 0) {
        bool write_fault = (r->err_code & 2) != 0;
        bool valid_fault = false;
        u32 region_prot = 0;

        u32 probed = 0;
        bool have_probe = vma_probe(proc, fault_addr, &probed);

        if (have_probe && probed == 0) {
            valid_fault = false;
        }
        else if (have_probe && (probed & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC))) {
            valid_fault = true;
            region_prot = probed;
        }
        /* Stack-region fallback for a fault vma_probe() didn't resolve. This
         * used to accept any address in a hardcoded ~128 GB window
         * (0x00007fe0'00000000..0x00007fffffffe000) — far past the 8 MB
         * stack the ELF loader actually reserves (elf.c: proc->stack_low =
         * stack_top - USER_STACK_MAX_BYTES, registered as a VMA there). A
         * stack pointer that walked off the bottom of a real 8 MB stack —
         * runaway/unbounded recursion, an attacker-controlled recursion
         * depth — kept getting fresh zeroed pages instead of a fault, all
         * the way down through 128 GB of address space, capped only by
         * systemwide free memory (the pmm_get_free_pages() check above).
         * That both defeated the stack-size limit as a guard against
         * unbounded growth and stayed silent about it. Bounding this to the
         * process's own registered stack VMA turns that into an immediate,
         * correctly-attributed SIGSEGV at the 8 MB mark — the same
         * immediate-#PF guarantee the kernel's own per-thread stacks get
         * from their dedicated guard page (see kstack_alloc() in sched.c). */
        else if (proc->stack_low > 0 && fault_addr >= proc->stack_low &&
                 fault_addr < proc->stack_high) {
            valid_fault = true;
            region_prot = VMA_PROT_READ | VMA_PROT_WRITE;
        }
        else if (proc->heap_start > 0 && fault_addr >= proc->heap_start && fault_addr < proc->heap_end) {
            valid_fault = true;
            region_prot = VMA_PROT_READ | VMA_PROT_WRITE;
        }
        else if (!have_probe && proc->mmap_current > 0x0000600000000000ULL &&
                 fault_addr >= 0x0000600000000000ULL && fault_addr < proc->mmap_current) {
            valid_fault = true;
            region_prot = VMA_PROT_READ | VMA_PROT_WRITE;
        }

        if (valid_fault && write_fault && !(region_prot & VMA_PROT_WRITE))
            valid_fault = false;

        if (valid_fault && pmm_get_free_pages() < 512) {
            valid_fault = false;
        }

        if (valid_fault) {
            phys_addr_t new_page = pmm_alloc_page();
            if (new_page) {
                hw_clear_page((void *)PHYS_TO_VIRT(new_page));
                u64 mf = VMM_F_PRESENT | VMM_F_USER;
                if (region_prot & VMA_PROT_WRITE) mf |= VMM_F_WRITE;
                if (!(region_prot & VMA_PROT_EXEC)) mf |= VMM_F_NX;
                vmm_map(proc->pml4_phys, ALIGN_DOWN(fault_addr, PAGE_SIZE), new_page, mf);
                proc->nr_minor_faults++;
                return true; /* Demand page resolved; resume instruction */
            }
        }
    }

    return false;
}

static void isr_dispatch_inner(pt_regs_t *r)
{
    u64 vec = r->int_no;

    /* Handle timer tick (48) BEFORE anything else so scheduler works */
    if (vec == 48) { /* Timer */
        extern void sched_tick(pt_regs_t *r);
        sched_tick(r);
        if ((r->cs & 3) != 0) {
            sched_check_reschedule();
        }
        return;
    }

    /* Handle TLB shootdown IPI (251): flush and acknowledge, nothing else. */
    if (vec == 251) {
        extern void lapic_eoi(void);
        extern void tlb_shootdown_ipi(void);
        tlb_shootdown_ipi();
        lapic_eoi();
        return;
    }

    /* Handle SMP Reschedule IPI (49) */
    if (vec == 49) {
        extern void lapic_eoi(void);
        lapic_eoi();
        if ((r->cs & 3) != 0) {
            sched_check_reschedule();
        }
        return;
    }

    if (vec < 32) {
        /* ── CPU exception ───────────────────────────────────────────────── */
        /* BUG-G fix: removed dead ternary — vec < 32 is always true here. */
        const char *name = g_exc_names[vec];

        /* #MC (18): the CPU reporting its own hardware failure. Decoded before
         * anything else touches the frame — the record in the MCA banks is
         * volatile, and a nested fault while formatting a register dump would
         * lose it. mce_handle() answers whether the context is safe to resume;
         * when it is not, there is nothing to return to and no process to
         * blame, so this is one of the few genuinely unconditional panics. */
        if (vec == 18) {
            if (mce_handle(r)) return;
            kernel_panic("#MC: unrecoverable machine check at RIP=0x%016llx",
                         (unsigned long long)r->rip);
        }

        /* #AC (17): either a userspace alignment check (only possible with
         * CR0.AM and RFLAGS.AC both set, from ring 3) or a split-lock
         * violation, which we deliberately armed. The two are told apart
         * exactly as the architecture allows: anything from ring 0, or from
         * ring 3 without RFLAGS.AC, cannot be an alignment check. */
        if (vec == 17 && g_split_lock_detect) {
            bool from_user = (r->cs & 3) != 0;
            bool ac_flag   = (r->rflags & (1ULL << 18)) != 0;
            if (!from_user || !ac_flag) {
                cpu_split_lock_fault(r->rip, from_user);
                return;   /* detection disarmed here; the insn retries and wins */
            }
        }

        /* Vector 14 (#PF): if CR2 is a user address, check if this is a demand-page
         * or Copy-On-Write write fault. We must service valid user faults BEFORE
         * checking extable so that kernel uaccess (copy_to_user/copy_from_user)
         * writing to a COW page or demand-paged user buffer resolves the page
         * rather than prematurely aborting with EFAULT. */
        uintptr_t fault_addr = 0;
        if (vec == 14) {
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));
            if (fault_addr < 0x0000800000000000ULL) {
                if (handle_user_page_fault(r, fault_addr))
                    return;
            }
        }

        /* Before treating this as fatal, see whether the faulting instruction is
         * one the kernel already declared recoverable (copy_from_user's rep
         * movsb, the XSAVE-variant probe, ...). #PF is the classic case; #GP
         * covers a non-canonical user pointer, and #UD covers an instruction
         * CPUID advertised but the CPU or emulator does not actually implement.
         * A ring-3 RIP can never match a kernel .extable entry, so this is safe
         * to test before the userspace split below. */
        if (vec == 6 || vec == 13 || vec == 14) {
            u64 fixup = search_extable(r->rip);
            if (fixup) {
                r->rip = fixup;
                return;
            }
        }

        /* Isolate ring-3 userspace faults: kill process, do NOT panic kernel */
        if ((r->cs & 3) != 0) {
            extern process_t *sched_current_process(void);
            extern s64 sys_exit_impl(pt_regs_t *r);
            process_t *proc = sched_current_process();

            /* #DB (single-step, vec 1) and #BP (int3, vec 3) are how a debugger
             * gets control. For a traced process they become a ptrace-stop and
             * the thread resumes where the tracer leaves it; only an untraced
             * process falls through to the fatal SIGTRAP below — a program that
             * executes int3 with nobody watching really has crashed. */
            if ((vec == 1 || vec == 3) && ptrace_report_trap(r, SIGTRAP))
                return;

            if (vec == 14) {
                u64 ucr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(ucr3));
                u64 *upml4 = (u64 *)PHYS_TO_VIRT(ucr3 & ~0xFFFULL);
                u64 upml4e = upml4[(fault_addr >> 39) & 0x1FF];
                u64 updpte = 0, upde = 0, upte = 0;
                if (upml4e & 1) {
                    u64 *updpt = (u64 *)PHYS_TO_VIRT(upml4e & 0x000FFFFFFFFFF000ULL);
                    updpte = updpt[(fault_addr >> 30) & 0x1FF];
                    if ((updpte & 1) && !(updpte & 0x80)) {
                        u64 *upd = (u64 *)PHYS_TO_VIRT(updpte & 0x000FFFFFFFFFF000ULL);
                        upde = upd[(fault_addr >> 21) & 0x1FF];
                        if ((upde & 1) && !(upde & 0x80)) {
                            u64 *upt = (u64 *)PHYS_TO_VIRT(upde & 0x000FFFFFFFFFF000ULL);
                            upte = upt[(fault_addr >> 12) & 0x1FF];
                        }
                    }
                }
                kprintf("[FAULT] User process '%s' (PID %u) terminated due to #PF at RIP=0x%016llx, CR2=0x%016llx (err=0x%llx)\n"
                        "        CR3=0x%llx PML4E=0x%llx PDPTE=0x%llx PDE=0x%llx PTE=0x%llx\n",
                        proc ? proc->name : "unknown",
                        proc ? proc->pid : 0,
                        (unsigned long long)r->rip,
                        (unsigned long long)fault_addr,
                        (unsigned long long)r->err_code,
                        (unsigned long long)ucr3,
                        (unsigned long long)upml4e,
                        (unsigned long long)updpte,
                        (unsigned long long)upde,
                        (unsigned long long)upte);
            } else {
                kprintf("[FAULT] User process '%s' (PID %u) terminated due to %s (vec=%llu) at RIP=0x%016llx (err=0x%llx)\n",
                        proc ? proc->name : "unknown",
                        proc ? proc->pid : 0,
                        name, (unsigned long long)vec,
                        (unsigned long long)r->rip,
                        (unsigned long long)r->err_code);
            }

            kprintf("  RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx  RDX=0x%016llx\n"
                    "  RSI=0x%016llx  RDI=0x%016llx  RBP=0x%016llx  RSP=0x%016llx\n"
                    "  R8 =0x%016llx  R9 =0x%016llx  R10=0x%016llx  R11=0x%016llx\n"
                    "  R12=0x%016llx  R13=0x%016llx  R14=0x%016llx  R15=0x%016llx\n",
                    (unsigned long long)r->rax,  (unsigned long long)r->rbx,
                    (unsigned long long)r->rcx,  (unsigned long long)r->rdx,
                    (unsigned long long)r->rsi,  (unsigned long long)r->rdi,
                    (unsigned long long)r->rbp,  (unsigned long long)r->rsp,
                    (unsigned long long)r->r8,   (unsigned long long)r->r9,
                    (unsigned long long)r->r10,  (unsigned long long)r->r11,
                    (unsigned long long)r->r12,  (unsigned long long)r->r13,
                    (unsigned long long)r->r14,  (unsigned long long)r->r15);

            if (r->rsp && (uintptr_t)r->rsp < 0x8000000000000000ULL) {
                u64 stk[32] = {0};
                if (copy_from_user(stk, (const void *)r->rsp, sizeof(stk)) == 0) {
                    for (int s = 0; s < 32; s += 4) {
                        kprintf("  STK[%2d..%2d]: 0x%016llx 0x%016llx 0x%016llx 0x%016llx\n",
                                s, s + 3,
                                (unsigned long long)stk[s], (unsigned long long)stk[s + 1],
                                (unsigned long long)stk[s + 2], (unsigned long long)stk[s + 3]);
                    }
                }
            }

            u8 code_bytes[16];
            if (r->rip && r->rip < 0x8000000000000000ULL && copy_from_user(code_bytes, (const void *)r->rip, sizeof(code_bytes)) == 0) {
                kprintf("  Code at RIP: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                        code_bytes[0], code_bytes[1], code_bytes[2], code_bytes[3],
                        code_bytes[4], code_bytes[5], code_bytes[6], code_bytes[7],
                        code_bytes[8], code_bytes[9], code_bytes[10], code_bytes[11],
                        code_bytes[12], code_bytes[13], code_bytes[14], code_bytes[15]);
            }

            /* Report the fault as a POSIX termination signal so waitpid()
             * observes WIFSIGNALED / WTERMSIG rather than a bogus exit code. */
            if (proc) {
                int term_sig;
                switch (vec) {
                    case 6:  term_sig = SIGILL;  break;  /* #UD */
                    case 0:  term_sig = SIGFPE;  break;  /* #DE */
                    case 16:
                    case 19: term_sig = SIGFPE;  break;  /* x87 / SIMD FP */
                    case 3:  term_sig = SIGTRAP; break;  /* #BP */
                    case 1:  term_sig = SIGTRAP; break;  /* #DB */
                    case 17: term_sig = SIGBUS;  break;  /* #AC */
                    default: term_sig = SIGSEGV; break;  /* #PF, #GP, #SS, ... */
                }
                proc->term_signal = term_sig;
                proc->exit_code = 128 + term_sig;
            }

            sys_exit_impl(r);
            return;
        }

        /* BUG-H fix: removed first duplicate register-dump kprintf that was
         * printing an identical block before the vec==14 check.  The single
         * authoritative dump is kept below, just before the stack walk. */

        if (vec == 14) {
            /* Kernel-mode #PF: read CR2 and dump page table walk */
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

            u64 cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            u64 *pml4 = (u64 *)PHYS_TO_VIRT(cr3 & ~0xFFFULL);
            process_t *panic_proc = sched_current_process();
            thread_t  *panic_thr  = sched_current_thread();
            u32 panic_pid  = panic_proc ? panic_proc->pid : 0;
            u32 panic_tid  = panic_thr ? panic_thr->tid : 0;
            const char *panic_name = panic_proc ? panic_proc->name : "?";

            u64 pml4e = pml4[(fault_addr >> 39) & 0x1FF];
            u64 pdpte = 0, pde = 0, pte = 0;
            if (pml4e & 1) {
                u64 *pdpt = (u64 *)PHYS_TO_VIRT(pml4e & 0x000FFFFFFFFFF000ULL);
                pdpte = pdpt[(fault_addr >> 30) & 0x1FF];
                if ((pdpte & 1) && !(pdpte & 0x80)) {
                    u64 *pd = (u64 *)PHYS_TO_VIRT(pdpte & 0x000FFFFFFFFFF000ULL);
                    pde = pd[(fault_addr >> 21) & 0x1FF];
                    if ((pde & 1) && !(pde & 0x80)) {
                        u64 *pt = (u64 *)PHYS_TO_VIRT(pde & 0x000FFFFFFFFFF000ULL);
                        pte = pt[(fault_addr >> 12) & 0x1FF];
                    }
                }
            }

            /* RIP/RSP and the faulting thread are what actually locate the
             * bug; the page-table dump only says the address was not mapped,
             * which the error code already implied. An err_code with bit 4 set
             * means the fetch itself faulted, so RIP *is* the bad address and
             * the return address at [RSP] names the caller that jumped there. */
            kernel_panic("#PF: unhandled kernel page fault at 0x%016llx (err=0x%llx)\n"
                         "  RIP=0x%016llx RSP=0x%016llx RBP=0x%016llx\n"
                         "  [RSP]=0x%016llx PID=%u TID=%u '%s'\n"
                         "  CR3=0x%llx PML4E=0x%llx PDPTE=0x%llx PDE=0x%llx PTE=0x%llx",
                         (unsigned long long)fault_addr,
                         (unsigned long long)r->err_code,
                         (unsigned long long)r->rip,
                         (unsigned long long)r->rsp,
                         (unsigned long long)r->rbp,
                         (unsigned long long)((r->rsp >= 0xFFFF800000000000ULL)
                                              ? *(u64 *)r->rsp : 0),
                         panic_pid, panic_tid, panic_name,
                         (unsigned long long)cr3,
                         (unsigned long long)pml4e,
                         (unsigned long long)pdpte,
                         (unsigned long long)pde,
                         (unsigned long long)pte);
        }

        kprintf("[ISR] Kernel Exception %llu (%s)  err=0x%016llx\n"
                "  RIP=0x%016llx  CS=0x%llx  RFLAGS=0x%016llx\n"
                "  RAX=0x%016llx  RBX=0x%016llx  RCX=0x%016llx  RDX=0x%016llx\n"
                "  RSI=0x%016llx  RDI=0x%016llx  RBP=0x%016llx  RSP=0x%016llx\n"
                "  R8 =0x%016llx  R9 =0x%016llx  R10=0x%016llx  R11=0x%016llx\n"
                "  R12=0x%016llx  R13=0x%016llx  R14=0x%016llx  R15=0x%016llx\n",
                (unsigned long long)vec, name,
                (unsigned long long)r->err_code,
                (unsigned long long)r->rip,
                (unsigned long long)r->cs,
                (unsigned long long)r->rflags,
                (unsigned long long)r->rax,  (unsigned long long)r->rbx,
                (unsigned long long)r->rcx,  (unsigned long long)r->rdx,
                (unsigned long long)r->rsi,  (unsigned long long)r->rdi,
                (unsigned long long)r->rbp,  (unsigned long long)r->rsp,
                (unsigned long long)r->r8,   (unsigned long long)r->r9,
                (unsigned long long)r->r10,  (unsigned long long)r->r11,
                (unsigned long long)r->r12,  (unsigned long long)r->r13,
                (unsigned long long)r->r14,  (unsigned long long)r->r15);

        kprintf("  Stack dump at RSP=0x%016llx:\n", (unsigned long long)r->rsp);
        u64 *s = (u64 *)r->rsp;
        if (s && (((uintptr_t)s & 7) == 0) && ((uintptr_t)s >= 0xffff800000000000ULL)) {
            /* Stay inside the page RSP points at: a fault near the top of a
             * kernel stack would otherwise walk into the unmapped guard page
             * and turn this diagnostic into a nested #PF panic, hiding the
             * original fault. (If RSP itself is already in an unmapped guard
             * page, s[0] still faults — the #PF IST stack keeps that legible.) */
            uintptr_t page_end = ((uintptr_t)s | 0xFFFULL) + 1;
            if (page_end == 0) page_end = ~0ULL;   /* s in the last page: don't wrap to 0 */
            for (int i = 0; i < 8 && (uintptr_t)&s[i + 1] <= page_end; i++) {
                kprintf("    [RSP+%02x] = 0x%016llx\n", i * 8, (unsigned long long)s[i]);
            }
        }

        kernel_panic("Unhandled CPU exception %llu (%s)", (unsigned long long)vec, name);

    } else {
        /* ── Hardware IRQ or APIC vector ─────────────────────────────────── */
        u64 slot = vec - 32;
        if (slot < 224 && g_irq_table[slot].fn) {
            g_irq_table[slot].fn(r, g_irq_table[slot].ctx);
        }

        /* Send EOI */
        hal_irq_eoi((u8)vec);

        /* Only preempt on the way back to ring 3. A device IRQ that interrupted
         * kernel code must not context-switch here — the interrupted path may
         * hold a spinlock or be mid-critical-section (the kernel is only
         * preemptible at the user boundary). Matches the timer path's gate. */
        if ((r->cs & 3) != 0)
            sched_check_reschedule();
    }
}
