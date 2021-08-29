#include "./include/pit.h"
#include "../arch/include/isr.h"
#include "../klibc/include/port.h"
#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL1 0x41
#define PIT_CMD_REG  0x43
uint32_t pit_ticks = 0;

void pit_handler(registers_t *r)
{
    pit_ticks++; // increment the pit_ticks value after receiving an PIT interrupt;
    UNUSED(r);
}

/*
    Set pit count;
*/
void set_pit_count(uint32_t count)
{
    __asm__ volatile("cli"); // disable interrupts
    outb(PIT_CHANNEL0, count & 0xFF); // set low byte.
    outb(PIT_CHANNEL0, (count & 0xFF00) >> 8); // set high byte.
    __asm__ volatile("sti"); // enable interrupts;
}

/*
    Reads current pit count.
*/
uint32_t read_pit_count()
{
    uint32_t count = 0;
    __asm__ volatile("cli"); // disable interrupts;
    outb(PIT_CMD_REG, 0b0000000);
    count = inb(PIT_CHANNEL0); // low byte;
    count |= inb(PIT_CHANNEL0) << 8; // high byte;
    return count;
}
/*
    Set PIT frequency;
*/
void set_pit_phase(uint32_t hz)
{
    uint32_t divisor = 1193180 / hz; // Calculate divisor;
    outb(PIT_CMD_REG, 0x36); // set command byte 0x36;
    outb(PIT_CHANNEL0, divisor & 0xFF); // set low byte of divisor;
    outb(PIT_CHANNEL0, divisor >> 8);  // set high byte of divisor;
}

void init_pit()
{
    register_interrupt_handler(IRQ0, pit_handler); // install PIT handler;
}

void pit_wait(int ticks)
{
    unsigned long eticks;
    eticks = pit_ticks + ticks; // total number ticks;
    while(pit_ticks < eticks);
}