#include "./include/pit.h"
#include "../arch/include/isr.h"
#include "../arch/include/apic.h"
#include "../arch/include/pic.h"
#include "../arch/include/spinlock.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../proc/include/scheduler.h"

#define PIT_CHANNEL0 0x40
#define PIT_CHANNEL1 0x41
#define PIT_CHANNEL2 0x42
#define PIT_CMD_REG  0x43
#define PIT_BASE_FREQ 1193182UL

volatile uint64_t g_system_ticks = 0;
uint32_t pit_ticks = 0;
static volatile int g_tick_lock = 0;
static volatile int g_pit_hw_lock = 0;

static void serial_hex(uint64_t val) {
    for (int i = 60; i >= 0; i -= 4) {
        int d = (val >> i) & 0xF;
        outb(0x3F8, d < 10 ? '0' + d : 'a' + (d - 10));
    }
}

void pit_handler(registers_t *r)
{
    static int printed = 0;
    if (printed < 5 && r && r->eip) {
        outb(0x3F8, '\r');
        outb(0x3F8, '\n');
        outb(0x3F8, '[');
        serial_hex(r->eip);
        outb(0x3F8, ' ');
        serial_hex(r->cs);
        outb(0x3F8, ']');
        printed++;
    }

    /*
     * Flawless EOI signaling across all execution paths:
     * Acknowledge Local APIC EOI and legacy PIC EOI before invoking scheduler_schedule().
     * If scheduler_schedule() switches context, this ISR will not reach code after the
     * schedule call until the interrupted thread resumes. Sending EOI immediately ensures
     * the timer hardware (APIC or PIC) clears its In-Service Register and continues firing
     * subsequent ticks without hanging.
     */
    if (g_apic_enabled) {
        apic_send_eoi();
    }
    PIC_send_EOI(0);

    /*
     * Atomic tick counting:
     * Only the Bootstrap Processor (BSP, Core 0) or when APIC is disabled increments
     * the global system uptime ticks. This prevents AP cores from multiplying the tick rate
     * when all cores receive local APIC periodic timer interrupts.
     */
    uint32_t core_id = apic_get_id();
    if (core_id == 0 || !g_apic_enabled) {
        unsigned long flags;
        spinlock_acquire_irqsave(&g_tick_lock, &flags);
        g_system_ticks++;
        pit_ticks = (uint32_t)g_system_ticks;
        spinlock_release_irqrestore(&g_tick_lock, flags);
    }

    /* Preemptive task scheduling on the current core */
    scheduler_schedule();
    UNUSED(r);
}

/*
    Set pit count;
*/
void set_pit_count(uint32_t count)
{
    unsigned long flags;
    spinlock_acquire_irqsave(&g_pit_hw_lock, &flags);
    outb(PIT_CHANNEL0, (uint8_t)(count & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((count >> 8) & 0xFF));
    spinlock_release_irqrestore(&g_pit_hw_lock, flags);
}

/*
    Reads current pit count atomically without partial read corruption.
*/
uint32_t read_pit_count(void)
{
    uint32_t count = 0;
    unsigned long flags;
    spinlock_acquire_irqsave(&g_pit_hw_lock, &flags);
    outb(PIT_CMD_REG, 0b00000000); /* Latch Channel 0 count */
    count = (uint32_t)inb(PIT_CHANNEL0);
    count |= (uint32_t)inb(PIT_CHANNEL0) << 8;
    spinlock_release_irqrestore(&g_pit_hw_lock, flags);
    return count;
}

/*
    Set PIT frequency using precise register calibration (no integer truncation).
*/
void set_pit_phase(uint32_t hz)
{
    if (hz == 0) return;
    /* Eliminate rounding errors and integer truncations via rounded integer division */
    uint32_t divisor = (PIT_BASE_FREQ + (hz / 2)) / hz;
    if (divisor < 18) divisor = 18; /* Maximum hardware frequency ~66.2 kHz */
    if (divisor > 65535) divisor = 0; /* 0 in 16-bit hardware counter equals 65536 */

    unsigned long flags;
    spinlock_acquire_irqsave(&g_pit_hw_lock, &flags);
    outb(PIT_CMD_REG, 0x34); /* Channel 0, lobyte/hibyte, Mode 2 (rate generator), 16-bit binary */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
    spinlock_release_irqrestore(&g_pit_hw_lock, flags);
}

void init_pit(void)
{
    /* Register timer interrupt handler on vector 32 (IRQ0) */
    register_interrupt_handler(32, pit_handler);
    set_pit_phase(PIT_TIMER_FREQ_HZ);
    kprintf("pit: legacy Programmable Interval Timer initialized at %u Hz\n", PIT_TIMER_FREQ_HZ);
}

uint64_t timer_get_ticks(void)
{
    unsigned long flags;
    spinlock_acquire_irqsave(&g_tick_lock, &flags);
    uint64_t t = g_system_ticks;
    spinlock_release_irqrestore(&g_tick_lock, flags);
    return t;
}

uint32_t timer_get_ticks_32(void)
{
    return (uint32_t)timer_get_ticks();
}

void pit_wait(int ticks)
{
    if (ticks <= 0) return;
    uint64_t eticks = timer_get_ticks() + (uint64_t)ticks;
    while (timer_get_ticks() < eticks) {
        asm volatile("pause");
    }
}