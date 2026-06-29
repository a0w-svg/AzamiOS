#include "./include/isr.h"
#include "../klibc/include/stdio.h"
#include "./include/pic.h"
#include "./include/idt.h"
#include "../klibc/include/port.h"
#include "../klibc/include/kpanic.h"
#include <stdint.h>
#include <stdbool.h>
#include "../mem/include/paging.h"
extern page_directory_entry_t page_directory[];

isr_t interrupt_handlers[256];

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
#if !defined(__x86_64__)
    asm volatile("sti");
#endif
}

/*
    Interrupt handler; Gets called from asm interrupt handler stub;
*/
void exception_handler(registers_t *r)
{
    if(interrupt_handlers[r->int_no] != 0)
    {
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
    else
    {
        if(r->int_no < 32){
            PANIC(exception_messages[r->int_no]);
        }
        else{
            PANIC("Unhandled Hardware Interrupt");
        }
    }
}

/*
    Registers the interrupt handler.
*/
void register_interrupt_handler(uint8_t num, isr_t handler)
{
    interrupt_handlers[num] = handler;
}
/*
IRQ handler
*/
void irq_handler(registers_t *r)
{
    // check if interrupt comes from dedicated handler (for example: keyboard)
    if(interrupt_handlers[r->int_no] != 0){
        isr_t handler = interrupt_handlers[r->int_no];
        handler(r);
    }
    // send EOI signal to PIC
    PIC_send_EOI(r->int_no - 32);
}

void page_fault_handler(registers_t *r) {
    uintptr_t cr2;
    asm volatile("mov %%cr2, %0" : "=r"(cr2));
    
    kprintf("\n[PAGE FAULT] CR2=0x%x at EIP=0x%x (err_code=0x%x)\n", (uint32_t)cr2, r->eip, (uint32_t)r->err_code);

#if defined(__x86_64__)
    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t*)(cr3 & ~0xFFFULL);
    uint32_t pml4_idx = (cr2 >> 39) & 0x1FF;
    uint64_t pml4e = pml4[pml4_idx];
    kprintf("64-bit Walk: PML4E[%d]=0x%x%x\n", pml4_idx, (uint32_t)(pml4e >> 32), (uint32_t)pml4e);
    if (pml4e & 1) {
        uint64_t *pdpt = (uint64_t*)(uintptr_t)(pml4e & ~0xFFFULL);
        uint32_t pdpt_idx = (cr2 >> 30) & 0x1FF;
        uint64_t pdpte = pdpt[pdpt_idx];
        kprintf("  PDPTE[%d]=0x%x%x\n", pdpt_idx, (uint32_t)(pdpte >> 32), (uint32_t)pdpte);
        if (pdpte & 1) {
            uint64_t *pd = (uint64_t*)(uintptr_t)(pdpte & ~0xFFFULL);
            uint32_t pd_idx = (cr2 >> 21) & 0x1FF;
            uint64_t pde = pd[pd_idx];
            kprintf("  PDE[%d]=0x%x%x\n", pd_idx, (uint32_t)(pde >> 32), (uint32_t)pde);
            if ((pde & 1) && !(pde & 0x80)) {
                uint64_t *pt = (uint64_t*)(uintptr_t)(pde & ~0xFFFULL);
                uint32_t pt_idx = (cr2 >> 12) & 0x1FF;
                uint64_t pte = pt[pt_idx];
                kprintf("  PTE[%d]=0x%x%x\n", pt_idx, (uint32_t)(pte >> 32), (uint32_t)pte);
            }
        }
    }
#else
    int present = !(r->err_code & 0x1); // 0 = Page not found, 1 = security error
    int rw = r->err_code & 0x2;  // 0 = read error, 2 = write error
    int us = r->err_code & 0x4; // 0 = kernel fault, 4 = usermode fault

    uint32_t pd_index = (uint32_t)((cr2 >> 22) & 0x3FF);
    uint32_t pt_index = (uint32_t)((cr2 >> 12) & 0x3FF);
    uint32_t pde_val = page_directory[pd_index].value;
    uint32_t pte_val = 0;
    if (page_directory[pd_index].present) {
        page_table_t *pt = (page_table_t*)(uintptr_t)(page_directory[pd_index].table << 12);
        pte_val = pt->pages[pt_index].value;
    }

    uintptr_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    uint32_t hw_pde = ((uint32_t*)cr3)[pd_index];

    kprintf("DEBUG: cr3=0x%x, page_dir=0x%x, hw_pde=0x%x\n", (uint32_t)cr3, (uint32_t)(uintptr_t)page_directory, hw_pde);
    kprintf("DEBUG: pd_index=%d, pde=0x%x, pt_index=%d, pte=0x%x\n", pd_index, pde_val, pt_index, pte_val);
#endif
    PANIC("Memory Access Violation (Page Fault)");
}