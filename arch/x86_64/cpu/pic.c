/* ============================================================================
 * AzamiOS — Legacy 8259 PIC Implementation
 * File: arch/x86_64/cpu/pic.c
 * ============================================================================ */

#include "pic.h"
#include "../../../include/azami/defs.h"

void pic_init(u8 offset1, u8 offset2)
{
    /* ICW1: start initialisation sequence (edge triggered, cascade mode) */
    outb(PIC1_CMD,  PIC_ICW1_INIT);  io_wait();
    outb(PIC2_CMD,  PIC_ICW1_INIT);  io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, offset1);         io_wait();
    outb(PIC2_DATA, offset2);         io_wait();

    /* ICW3: tell master that a slave is on IRQ2, tell slave its cascade id */
    outb(PIC1_DATA, 0x04);            io_wait(); /* master: slave on IRQ2 */
    outb(PIC2_DATA, 0x02);            io_wait(); /* slave:  cascade identity 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, PIC_ICW4_8086);   io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);   io_wait();

    /* Mask all IRQs initially — unmask selectively later */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_mask_all(void)
{
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

void pic_eoi(u8 irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_set_mask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) | (u8)(1U << irq));
}

void pic_clear_mask(u8 irq)
{
    u16 port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    if (irq >= 8) irq -= 8;
    outb(port, inb(port) & (u8)~(1U << irq));
}
