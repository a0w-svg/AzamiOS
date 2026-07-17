/**
 * apic.c  –  Local APIC Driver & Inter-Processor Interrupts
 */
#include "include/apic.h"
#include "include/pic.h"
#include "../drivers/include/pit.h"
#include "../mem/include/paging.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/port.h"

static volatile uint32_t *lapic = (volatile uint32_t *)LOCAL_APIC_BASE;
bool g_apic_enabled = false;
uint32_t g_apic_timer_ticks_per_hz = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

void apic_init(void) {
    /* Map Local APIC MMIO window */
    paging_map_page(LOCAL_APIC_BASE, LOCAL_APIC_BASE, 1, 1);
    g_apic_enabled = true;

    /* Enable Local APIC via Spurious Interrupt Vector Register (bit 8 = enable) */
    lapic_write(APIC_SVR, lapic_read(APIC_SVR) | 0x1FF);

    apic_timer_calibrate(PIT_TIMER_FREQ_HZ);
    kprintf("apic: local APIC initialized (Core ID %d)\n", apic_get_id());
}

uint32_t apic_get_id(void) {
    if (!g_apic_enabled) return 0;
    return (lapic_read(APIC_ID) >> 24) & 0xFF;
}

void apic_send_eoi(void) {
    if (!g_apic_enabled) return;
    lapic_write(APIC_EOI, 0);
}

void apic_send_init(uint8_t apic_id) {
    if (!g_apic_enabled) return;
    lapic_write(APIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    /* INIT delivery mode: assert, level triggered (0x4500) */
    lapic_write(APIC_ICR_LOW, 0x4500);
    for (volatile int i = 0; i < 10000; i++);
}

void apic_send_sipi(uint8_t apic_id, uint8_t vector) {
    if (!g_apic_enabled) return;
    lapic_write(APIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    /* Startup delivery mode: assert (0x4600) | vector */
    lapic_write(APIC_ICR_LOW, 0x4600 | vector);
    for (volatile int i = 0; i < 2000; i++);
}

void apic_timer_init(uint32_t hz) {
    if (!g_apic_enabled || g_apic_timer_ticks_per_hz == 0) return;
    (void)hz;
    lapic_write(APIC_TIMER_DIV, 0x03); /* Divide by 16 */
    /* Periodic Mode (bit 17 = 1 -> 0x00020000) | Vector 32 (0x20, timer interrupt) */
    lapic_write(APIC_TIMER_LVT, 0x00020000 | 0x20);
    lapic_write(APIC_TIMER_INIT_CNT, g_apic_timer_ticks_per_hz);
}

void apic_timer_calibrate(uint32_t hz) {
    if (!g_apic_enabled || hz == 0) return;

    /* If already calibrated by BSP, simply start the timer on this core */
    if (g_apic_timer_ticks_per_hz != 0) {
        apic_timer_init(hz);
        return;
    }

    kprintf("apic: calibrating Local APIC timer against reference PIT...\n");

    /* Mask APIC timer and set divider to 16 during calibration */
    lapic_write(APIC_TIMER_LVT, 0x10000);
    lapic_write(APIC_TIMER_DIV, 0x03);

    /*
     * Configure legacy PIT Channel 0 for 10 ms one-shot reference countdown:
     * 1193182 Hz * 0.01s = 11932 ticks. Mode 0 = interrupt on terminal count.
     */
    uint16_t ref_ticks = 11932;
    unsigned long flags;
    /* Save flags and disable interrupts while running reference calibration loop */
    asm volatile("pushf; pop %0; cli" : "=r" (flags) : : "memory");

    outb(0x43, 0x30); /* Channel 0, lobyte/hibyte, Mode 0, 16-bit binary */
    outb(0x40, (uint8_t)(ref_ticks & 0xFF));
    outb(0x40, (uint8_t)((ref_ticks >> 8) & 0xFF));

    /* Set APIC Timer initial count to maximum 32-bit value to start counting down */
    lapic_write(APIC_TIMER_INIT_CNT, 0xFFFFFFFF);

    /* Poll PIT Channel 0 until 10 ms elapses (counter reaches 0 or rolls over) */
    uint32_t prev_pit = ref_ticks;
    while (1) {
        outb(0x43, 0x00); /* Latch Channel 0 */
        uint32_t cur_pit = (uint32_t)inb(0x40);
        cur_pit |= (uint32_t)inb(0x40) << 8;
        if (cur_pit > prev_pit || cur_pit < 50) {
            break;
        }
        prev_pit = cur_pit;
        asm volatile("pause");
    }

    uint32_t elapsed_ticks = 0xFFFFFFFF - lapic_read(APIC_TIMER_CUR_CNT);

    /* Restore interrupt state */
    if (flags & (1 << 9)) {
        asm volatile("sti" ::: "memory");
    }

    if (elapsed_ticks < 1000) {
        kprintf("apic: Local APIC timer calibration failed (elapsed %u ticks), falling back to PIT\n", elapsed_ticks);
        set_pit_phase(hz);
        return;
    }

    /*
     * Calculate exact ticks per periodic interval (hz):
     * elapsed_ticks is for 10 ms (1/100 of a second).
     * Ticks per interval = (elapsed_ticks * 100 + hz / 2) / hz.
     */
    g_apic_timer_ticks_per_hz = (elapsed_ticks * 100 + (hz / 2)) / hz;
    kprintf("apic: calibrated Local APIC timer (%u ticks/ms, periodic interval=%u ticks at %u Hz)\n",
            elapsed_ticks / 10, g_apic_timer_ticks_per_hz, hz);

    /* Start the Local APIC periodic timer on this core and mask legacy PIC IRQ0 */
    apic_timer_init(hz);
    if (apic_get_id() == 0) {
        IRQ_set_mask(0);
    }
}

void apic_send_ipi(uint8_t apic_id, uint8_t vector) {
    if (!g_apic_enabled) return;
    lapic_write(APIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    lapic_write(APIC_ICR_LOW, 0x4000 | vector);
}

void apic_broadcast_ipi_exclude_self(uint8_t vector) {
    if (!g_apic_enabled) return;
    lapic_write(APIC_ICR_LOW, 0xC0000 | 0x4000 | vector);
}

