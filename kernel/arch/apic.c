/**
 * apic.c  –  Local APIC Driver & Inter-Processor Interrupts
 */
#include "include/apic.h"
#include "../mem/include/paging.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/port.h"

static volatile uint32_t *lapic = (volatile uint32_t *)LOCAL_APIC_BASE;

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
}

void apic_init(void) {
    /* Map Local APIC MMIO window */
    paging_map_page(LOCAL_APIC_BASE, LOCAL_APIC_BASE, 1, 1);

    /* Enable Local APIC via Spurious Interrupt Vector Register (bit 8 = enable) */
    lapic_write(APIC_SVR, lapic_read(APIC_SVR) | 0x1FF);

    kprintf("apic: local APIC initialized (Core ID %d)\n", apic_get_id());
}

uint32_t apic_get_id(void) {
    return (lapic_read(APIC_ID) >> 24) & 0xFF;
}

void apic_send_eoi(void) {
    lapic_write(APIC_EOI, 0);
}

void apic_send_init(uint8_t apic_id) {
    lapic_write(APIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    /* INIT delivery mode: assert, level triggered (0x4500) */
    lapic_write(APIC_ICR_LOW, 0x4500);
    for (volatile int i = 0; i < 10000; i++);
}

void apic_send_sipi(uint8_t apic_id, uint8_t vector) {
    lapic_write(APIC_ICR_HIGH, ((uint32_t)apic_id) << 24);
    /* Startup delivery mode: assert (0x4600) | vector */
    lapic_write(APIC_ICR_LOW, 0x4600 | vector);
    for (volatile int i = 0; i < 2000; i++);
}
