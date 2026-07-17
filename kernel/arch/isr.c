#include "./include/isr.h"
#include "./include/smp.h"
#include "./include/spinlock.h"
#include "./include/apic.h"

#include "../klibc/include/stdio.h"
#include "./include/pic.h"
#include "./include/idt.h"
#include "../klibc/include/port.h"
#include "../klibc/include/kpanic.h"
#include "../mem/include/access_control.h"
#include <stdint.h>
#include <stdbool.h>
#include "../mem/include/paging.h"
extern page_directory_entry_t page_directory[];

#define MAX_HANDLERS_PER_VEC 4
isr_t interrupt_handlers[256][MAX_HANDLERS_PER_VEC];


//interrupts code messages
char *exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",

    "Double Fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Unknown Interrupt",

    "Coprocessor Fault",
    "Alignment Check",
    "Machine Check",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",

    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

extern void isr_0();
extern void isr_1();
extern void isr_2();
extern void isr_3();
extern void isr_4();
extern void isr_5();
extern void isr_6();
extern void isr_7();
extern void isr_8();
extern void isr_9();
extern void isr_10();
extern void isr_11();
extern void isr_12();
extern void isr_13();
extern void isr_14();
extern void isr_15();
extern void isr_16();
extern void isr_17();
extern void isr_18();
extern void isr_19();
extern void isr_20();
extern void isr_21();
extern void isr_22();
extern void isr_23();
extern void isr_24();
extern void isr_25();
extern void isr_26();
extern void isr_27();
extern void isr_28();
extern void isr_29();
extern void isr_30();
extern void isr_31();

// syscall
extern void isr_128();
extern void isr_252();

extern volatile uintptr_t g_tlb_shootdown_addr;
extern volatile uint32_t g_tlb_shootdown_ack_count;

static void tlb_shootdown_isr(registers_t *r) {
    UNUSED(r);
    uintptr_t addr = g_tlb_shootdown_addr;
    if (addr == ~0UL) {
        uintptr_t cr3;
        asm volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3));
    } else {
        asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
    }
    apic_send_eoi();
    atomic_inc(&g_tlb_shootdown_ack_count);
}


// IRQ
extern void irq_0();
extern void irq_1();
extern void irq_2();
extern void irq_3();
extern void irq_4();
extern void irq_5();
extern void irq_6();
extern void irq_7();
extern void irq_8();
extern void irq_9();
extern void irq_10();
extern void irq_11();
extern void irq_12();
extern void irq_13();
extern void irq_14();
extern void irq_15();

void page_fault_handler(registers_t *r);

/*
    Initialize Iterrupt Service Routine;
*/
extern bool g_is_uefi;

void init_isr(void)
{


    idt_set_gate(0, (uintptr_t)isr_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(1, (uintptr_t)isr_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(2, (uintptr_t)isr_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(3, (uintptr_t)isr_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(4, (uintptr_t)isr_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(5, (uintptr_t)isr_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(6, (uintptr_t)isr_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(7, (uintptr_t)isr_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(8, (uintptr_t)isr_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(9, (uintptr_t)isr_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(10, (uintptr_t)isr_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(11, (uintptr_t)isr_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(12, (uintptr_t)isr_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(13, (uintptr_t)isr_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(14, (uintptr_t)isr_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(15, (uintptr_t)isr_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(16, (uintptr_t)isr_16, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(17, (uintptr_t)isr_17, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(18, (uintptr_t)isr_18, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(19, (uintptr_t)isr_19, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(20, (uintptr_t)isr_20, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(21, (uintptr_t)isr_21, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(22, (uintptr_t)isr_22, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(23, (uintptr_t)isr_23, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(24, (uintptr_t)isr_24, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(25, (uintptr_t)isr_25, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(26, (uintptr_t)isr_26, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(27, (uintptr_t)isr_27, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(28, (uintptr_t)isr_28, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(29, (uintptr_t)isr_29, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(30, (uintptr_t)isr_30, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(31, (uintptr_t)isr_31, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    
    // register syscall for userspace (DPL = 3)
    idt_set_gate(128, (uintptr_t)isr_128, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE_USER);
    idt_set_gate(252, (uintptr_t)isr_252, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    register_interrupt_handler(252, tlb_shootdown_isr);

    
    // initialize and remap PIC 
    init_PIC();
    
    // registrate hardware interrupts IRQ
    idt_set_gate(32, (uintptr_t)irq_0, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(33, (uintptr_t)irq_1, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(34, (uintptr_t)irq_2, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(35, (uintptr_t)irq_3, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(36, (uintptr_t)irq_4, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(37, (uintptr_t)irq_5, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(38, (uintptr_t)irq_6, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(39, (uintptr_t)irq_7, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(40, (uintptr_t)irq_8, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(41, (uintptr_t)irq_9, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(42, (uintptr_t)irq_10, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(43, (uintptr_t)irq_11, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(44, (uintptr_t)irq_12, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(45, (uintptr_t)irq_13, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(46, (uintptr_t)irq_14, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    idt_set_gate(47, (uintptr_t)irq_15, KERNEL_CODE_SEGMENT, BITS_32_INTERRUPT_GATE);
    register_interrupt_handler(14, page_fault_handler);
    // load IDT table and enable interrupts
    idt_init();
}

/*
    Interrupt handler; Gets called from asm interrupt handler stub;
*/
static void __attribute__((unused)) serial_hex_isr(uint64_t val) {
    for (int i = 60; i >= 0; i -= 4) {
        int d = (val >> i) & 0xF;
        outb(0x3F8, d < 10 ? '0' + d : 'a' + (d - 10));
    }
}

uintptr_t exception_handler(registers_t *r)
{
    cpu_data_t *cpu = smp_get_current_cpu();
    cpu->active_context = (uintptr_t)r;

    if (r->int_no < 32) {
        kprintf("\n=== EXCEPTION %d AT EIP=0x%x ===\n", (uint32_t)r->int_no, (uint32_t)r->eip);
        kprintf("  RAX=0x%x RBX=0x%x RCX=0x%x RDX=0x%x\n", (uint32_t)r->eax, (uint32_t)r->ebx, (uint32_t)r->ecx, (uint32_t)r->edx);
        kprintf("  RSI=0x%x RDI=0x%x RBP=0x%x RSP=0x%x\n", (uint32_t)r->esi, (uint32_t)r->edi, (uint32_t)r->ebp, (uint32_t)r->esp);
        if (r->esp && r->esp >= 0x400000 && r->esp < 0xC0000000) {
            uint64_t *sp = (uint64_t*)r->esp;
            kprintf("  USER STACK AT 0x%x: [0]=0x%x [1]=0x%x [2]=0x%x [3]=0x%x\n",
                    (uint32_t)r->esp, (uint32_t)sp[0], (uint32_t)sp[1], (uint32_t)sp[2], (uint32_t)sp[3]);
        }
        if (r->ebp && r->ebp >= 0x400000 && r->ebp < 0xC0000000) {
            uint64_t *bp = (uint64_t*)r->ebp;
            kprintf("  USER RBP AT 0x%x: [0]=0x%x [1]=0x%x [2]=0x%x [3]=0x%x\n",
                    (uint32_t)r->ebp, (uint32_t)bp[0], (uint32_t)bp[1], (uint32_t)bp[2], (uint32_t)bp[3]);
        }
    }
    bool handled = false;
    for (int i = 0; i < MAX_HANDLERS_PER_VEC; i++) {
        if (interrupt_handlers[r->int_no][i] != 0) {
            interrupt_handlers[r->int_no][i](r);
            handled = true;
        }
    }
    if (!handled) {
        if (r->int_no < 32) {
            PANIC(exception_messages[r->int_no]);
        } else {
            PANIC("Unhandled Hardware Interrupt");
        }
    }
    return cpu->active_context;
}

/*
    Registers the interrupt handler.
*/
void register_interrupt_handler(uint8_t num, isr_t handler)
{
    for (int i = 0; i < MAX_HANDLERS_PER_VEC; i++) {
        if (interrupt_handlers[num][i] == 0 || interrupt_handlers[num][i] == handler) {
            interrupt_handlers[num][i] = handler;
            return;
        }
    }
    kprintf("isr: warning: max handlers reached for vector %d\n", num);
}
/*
IRQ handler
*/
uintptr_t irq_handler(registers_t *r)
{
    cpu_data_t *cpu = smp_get_current_cpu();
    cpu->active_context = (uintptr_t)r;

    /*
     * Send EOI before calling handlers so that if a handler triggers a context
     * switch the PIC/APIC In-Service bit is already cleared — no deadlock.
     */
    PIC_send_EOI(r->int_no - 32);
    apic_send_eoi();

    for (int i = 0; i < MAX_HANDLERS_PER_VEC; i++) {
        if (interrupt_handlers[r->int_no][i] != 0) {
            interrupt_handlers[r->int_no][i](r);
        }
    }
    return cpu->active_context;
}

void page_fault_handler(registers_t *r) {
    uintptr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));

    /*
     * Decode the error code pushed by the CPU (Intel SDM Vol.3 §6.15):
     *   bit 0 (P)   : 0=not-present, 1=protection violation
     *   bit 1 (W/R) : 0=read,        1=write
     *   bit 2 (U/S) : 0=kernel CPL,  1=user CPL
     *   bit 3 (RSVD): reserved bit violation
     *   bit 4 (I/D) : 0=data,        1=instruction fetch (NX)
     */
    int pf_present  = (int)(r->err_code & 0x01); /* 0=missing page, 1=prot */
    int pf_write    = (int)(r->err_code & 0x02);
    int pf_user     = (int)(r->err_code & 0x04);
    int pf_rsvd     = (int)(r->err_code & 0x08);
    int pf_fetch    = (int)(r->err_code & 0x10);
    (void)pf_write; (void)pf_rsvd; (void)pf_fetch;

    if (pf_user) {
        /*
         * ── User-mode fault ───────────────────────────────────────────────
         * Case A: page not present in user range → demand-page it.
         * Case B: protection violation / NX / reserved → SEGFAULT.
         */
        if (!pf_present && ac_demand_page(cr2)) {
            /* Page successfully allocated and mapped — resume the instruction */
            return;
        }

        /* Bad access: log and kill the offending process */
        kprintf("[PF] SEGFAULT: CR2=0x%016llx EIP/RIP=0x%016llx err=0x%x (user %s %s)\n",
                (unsigned long long)cr2,
                (unsigned long long)r->eip,
                (uint32_t)r->err_code,
                pf_present ? "protection" : "not-present",
                pf_fetch   ? "exec"       : "access");
        ac_kill_current_process("illegal memory access (SEGFAULT)");
        /* ac_kill_current_process does not return */
    }

    /*
     * ── Kernel-mode fault ─────────────────────────────────────────────────
     * This is a kernel bug — cannot recover. Dump debug info then PANIC.
     */
    kprintf("\n[PAGE FAULT] kernel CR2=0x%016llx RIP=0x%016llx err=0x%x\n",
            (unsigned long long)cr2,
            (unsigned long long)r->eip,
            (uint32_t)r->err_code);

    /* Walk 4-level page table for debug info */
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t*)(cr3 & ~0xFFFULL);
    uint32_t pml4_idx = (cr2 >> 39) & 0x1FF;
    uint64_t pml4e = pml4[pml4_idx];
    kprintf("  PML4E[%u]=0x%08x%08x\n", pml4_idx,
            (uint32_t)(pml4e >> 32), (uint32_t)pml4e);
    if (pml4e & 1) {
        uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
        uint32_t pdpt_idx = (cr2 >> 30) & 0x1FF;
        uint64_t pdpte = pdpt[pdpt_idx];
        kprintf("  PDPTE[%u]=0x%08x%08x\n", pdpt_idx,
                (uint32_t)(pdpte >> 32), (uint32_t)pdpte);
        if (pdpte & 1) {
            uint64_t *pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
            uint32_t pd_idx = (cr2 >> 21) & 0x1FF;
            uint64_t pde = pd[pd_idx];
            kprintf("  PDE  [%u]=0x%08x%08x\n", pd_idx,
                    (uint32_t)(pde >> 32), (uint32_t)pde);
            if ((pde & 1) && !(pde & 0x80)) {
                uint64_t *pt = (uint64_t*)(uintptr_t)(pde & ~0xFFFULL);
                uint32_t pt_idx = (cr2 >> 12) & 0x1FF;
                uint64_t pte = pt[pt_idx];
                kprintf("  PTE  [%u]=0x%08x%08x\n", pt_idx,
                        (uint32_t)(pte >> 32), (uint32_t)pte);
            }
        }
    }
    PANIC("Kernel Page Fault — memory corruption or kernel bug");
}