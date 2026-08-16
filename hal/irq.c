/* ============================================================================
 * AzamiOS — HAL IRQ Subsystem
 * File: hal/irq.c
 * ============================================================================ */

#include "irq.h"
#include "../arch/x86_64/cpu/pic.h"
#include "../arch/x86_64/cpu/lapic.h"
#include "../drivers/acpi/ioapic.h"

void hal_irq_enable(u8 irq, u8 vector)
{
    if (ioapic_is_active()) {
        /* Route through IO APIC to the calling CPU's LAPIC */
        ioapic_set_irq(irq, vector, lapic_id());
    } else {
        /* Fallback: Unmask the legacy 8259 PIC */
        pic_clear_mask(irq);
    }
}

void hal_irq_disable(u8 irq)
{
    if (ioapic_is_active()) {
        ioapic_mask_irq(irq);
    } else {
        pic_set_mask(irq);
    }
}

void hal_irq_eoi(u8 vector)
{
    if (ioapic_is_active()) {
        /* APIC mode: LAPIC handles EOI for all hardware interrupts except spurious (255) */
        if (vector != 255) {
            lapic_eoi();
        }
    } else {
        /* PIC mode: Hardware IRQs start at vector 32 */
        if (vector >= 32 && vector <= 47) {
            pic_eoi(vector - 32);
        }
    }
}
