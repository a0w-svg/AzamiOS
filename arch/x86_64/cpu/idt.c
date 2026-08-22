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
#include "../mm/vmm.h"

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
DECL_ISR(251);
DECL_ISR(255);

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
typedef void (*irq_handler_fn_t)(pt_regs_t *r, void *ctx);

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

    /* Fill any remaining gates with isr_0 as a safe fallback */
    for (int v = 50; v <= 250; v++) {
        if (g_idt[v].offset_low == 0)
            idt_set_gate((u8)v, (uintptr_t)isr_0, IST_NONE, 0, IDT_TYPE_INT_GATE);
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
void isr_dispatch(pt_regs_t *r)
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
        const char *name = (vec < 32) ? g_exc_names[vec] : "Unknown";

        if (vec == 14) {
            /* First, check if this fault happened inside a known safe routine (like copy_from_user) */
            u64 fixup = search_extable(r->rip);
            if (fixup) {
                /* Extable entry found! We can safely recover from this fault. */
                r->rip = fixup;
                return;
            }
        }

        /* Isolate ring-3 userspace faults: kill process, do NOT panic kernel */
        if ((r->cs & 3) != 0) {
            extern process_t *sched_current_process(void);
            extern s64 sys_exit_impl(pt_regs_t *r);
            process_t *proc = sched_current_process();
            uintptr_t fault_addr = 0;
            if (vec == 14) {
                __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

                /* Demand-paged User Stack Expansion (Linux-standard 8MB user stack region) */
                if ((r->err_code & 1) == 0 && fault_addr >= 0x00007ff000000000ULL && fault_addr < 0x00007fffffffe000ULL && proc) {
                    phys_addr_t new_page = pmm_alloc_page();
                    if (new_page) {
                        __builtin_memset((void *)PHYS_TO_VIRT(new_page), 0, PAGE_SIZE);
                        vmm_map(proc->pml4_phys, ALIGN_DOWN(fault_addr, PAGE_SIZE), new_page, VMM_USER_RW);
                        return; /* Resumed user thread with expanded stack */
                    }
                }

                u64 ucr3;
                __asm__ volatile("mov %%cr3, %0" : "=r"(ucr3));
                u64 *upml4 = (u64 *)PHYS_TO_VIRT(ucr3 & ~0xFFFULL);
                u64 upml4e = upml4[(fault_addr >> 39) & 0x1FF];
                u64 updpte = 0, upde = 0, upte = 0;
                if (upml4e & 1) {
                    u64 *updpt = (u64 *)PHYS_TO_VIRT(upml4e & 0x000FFFFFFFFFF000ULL);
                    updpte = updpt[(fault_addr >> 30) & 0x1FF];
                    if (updpte & 1) {
                        u64 *upd = (u64 *)PHYS_TO_VIRT(updpte & 0x000FFFFFFFFFF000ULL);
                        upde = upd[(fault_addr >> 21) & 0x1FF];
                        if (upde & 1) {
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
            sys_exit_impl(r);
            return;
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

        if (vec == 14) {
            /* Kernel-mode #PF: read CR2 and dump page table walk */
            uintptr_t fault_addr;
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_addr));

            u64 cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
            u64 *pml4 = (u64 *)PHYS_TO_VIRT(cr3 & ~0xFFFULL);
            u64 pml4e = pml4[(fault_addr >> 39) & 0x1FF];
            u64 pdpte = 0, pde = 0, pte = 0;
            if (pml4e & 1) {
                u64 *pdpt = (u64 *)PHYS_TO_VIRT(pml4e & 0x000FFFFFFFFFF000ULL);
                pdpte = pdpt[(fault_addr >> 30) & 0x1FF];
                if (pdpte & 1) {
                    u64 *pd = (u64 *)PHYS_TO_VIRT(pdpte & 0x000FFFFFFFFFF000ULL);
                    pde = pd[(fault_addr >> 21) & 0x1FF];
                    if (pde & 1) {
                        u64 *pt = (u64 *)PHYS_TO_VIRT(pde & 0x000FFFFFFFFFF000ULL);
                        pte = pt[(fault_addr >> 12) & 0x1FF];
                    }
                }
            }

            kernel_panic("#PF: unhandled kernel page fault at 0x%016llx (err=0x%llx)\n"
                         "  CR3=0x%llx PML4E=0x%llx PDPTE=0x%llx PDE=0x%llx PTE=0x%llx",
                         (unsigned long long)fault_addr,
                         (unsigned long long)r->err_code,
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
        if (s && ((uintptr_t)s >= 0xffff800000000000ULL)) {
            for (int i = 0; i < 8; i++) {
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
        sched_check_reschedule();
    }
}
