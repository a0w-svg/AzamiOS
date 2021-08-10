#include "./include/pic.h"
#include "../klibc/include/port.h"
/*
                                AzamiOS PIC module
    The 8259 Programmable Interrupt Controller (PIC) is one of the most important chips
    making up the x86 architecture. Without it, the x86 architecture would not be an interrupt
    driven architecture. The function of the 8259A is to manage hardware interrupts and send them
    to the appropriate system interrupt. This allows the system to respond to devices needs without 
    loss of time (from polling the device, for instance;
*/
// 8259A Software Port Mappings
#define PIC1 0x20           // IO base address for primary PIC;
#define PIC2 0xA0           // IO base address for secondary PIC;
#define PIC1_COMMAND PIC1   // Primary PIC Command  and Status Register;
#define PIC1_DATA (PIC1+1)  // Primary PIC Interrupt Mask Register and Data Register;
#define PIC2_COMMAND PIC2   // Secondary PIC Command and Status Register;
#define PIC2_DATA (PIC2+1)  // Secondary PIC Interrut Mask Register and Data Register;

#define PIC_EOI     0x20    // End-of-interrupt command code;
/* Initialization Control Words (ICW)
    ICW1 - 0x11;
    ICW2 - vector offset;
    ICW3 - Tell it how it is wired to primary/secondary;
    ICW4 - additional information about the environment;
*/
#define ICW1_ICW4       0x01    // ICW4 (not) needed;
#define ICW1_SINGLE     0x02    // Single (cascade) mode;
#define ICW1_INTERVAL4  0x04    // Call address interval 4 (8);
#define ICW1_LEVEL      0x08    // Level triggered (edge) mode;
#define ICW1_INIT       0x10    // Initialization - required!;

#define ICW4_8086           0x01    // 8086/88 (MCS-80/85) mode;
#define ICW4_AUTO           0x02    // Auto (normal) End-of-Interrupt;
#define ICW4_BUF_SECONDARY  0x08    // Buffered mode secondary;
#define ICW4_BUF_PRIMARY    0x0C    // Buffered mode primary;
#define ICW4_SFNM           0x10    // Specialfully nested (not);

#define PIC_READ_IRR   0x0A // OCW3 irq ready next CMD read;
#define PIC_READ_ISR   0x0B // OCW3 irq service next CMD read;

/*
    reinitialize the PIC controllers, giving them specified  vector offsets 
    rather than 8h and 70h, as configured by default;
    arguments:
        offset1 - vector offset for primary PIC;
        offset2 - vector offset for secondary PIC;
*/
void PIC_remap(uint8_t offset1, uint8_t offset2)
{
    uint8_t p_mask, s_mask;

    p_mask = inb(PIC1_DATA);
    s_mask = inb(PIC2_DATA);
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4); // starts the initialization sequence
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC1_DATA, offset1); // ICW2: Primary PIC vector offset;
    io_wait();
    outb(PIC2_DATA, offset2); // ICW2: Secondary PIC vector offset;
    io_wait();
    outb(PIC1_DATA, 4); // ICW3: tell primary PIC that there is a secondary PIC at IRQ2;
    io_wait();
    outb(PIC2_DATA, 2); // ICW3: tell  secondary PIC its cascade identity;
    io_wait();

    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, p_mask); // Restore saved masks;
    outb(PIC2_DATA, s_mask);
}

/*
    Send the End-Of-Interrupt signal to irq device;
*/
void PIC_send_EOI(uint8_t irq)
{
    if(irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI); // If irq is greater than eight or equal, send the EOI signal to secondary PIC.
    else outb(PIC1_COMMAND, PIC_EOI); // Send the EOI signal to primary PIC;
}
/*
    Sets the ignore bit on the specifed IRQline;
*/
void IRQ_set_mask(uint8_t IRQline)
{
    uint16_t port;
    uint8_t value;
    if(IRQline < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        IRQline -= 8;
    }
    value = inb(port) | (1 << IRQline);
    outb(port, value);
}

/*
    Clears the ignore bit on the specifed IRQline;
*/
void IRQ_clear_mask(uint8_t IRQline)
{
    uint16_t port;
    uint8_t value;
    if(IRQline < 8)
        port = PIC1_DATA;
    else
    {
        port = PIC2_DATA;
        IRQline -= 8;
    }
    value = inb(port) & ~(1 << IRQline);
    outb(port, value);
}

static uint16_t __pic_get_irq_reg(uint8_t ocw3)
{
    /*
        OCW3 to PIC1 COMMAND to get the register values; PIC2 is chained, and 
        represents IRQs 8-15; PIC1 is IRQs 0-7, with 2 being the chain;
    */
    outb(PIC1_COMMAND, ocw3);
    outb(PIC2_COMMAND, ocw3);
    return (inb(PIC2_COMMAND) << 8) | inb(PIC1_COMMAND);
}

/*
    Returns the combined value of the cascaded PICs irq request register;
*/
uint16_t pic_get_irr()
{
    return __pic_get_irq_reg(PIC_READ_IRR);
}

/*
    Returns the combined value of the cascaded PICs in-service register;
*/
uint16_t pic_get_isr()
{
    return __pic_get_irq_reg(PIC_READ_ISR);
}
/*
    Initialize PIC;
*/
void init_PIC()
{
    PIC_remap(0x20, 0x28);
}