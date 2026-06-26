#ifndef APIC_H
#define APIC_H

#include <stdint.h>
#include <stdbool.h>

#define LOCAL_APIC_BASE 0xFEE00000

#define APIC_ID          0x0020
#define APIC_VER         0x0030
#define APIC_TPR         0x0080
#define APIC_EOI         0x00B0
#define APIC_SVR         0x00F0
#define APIC_ICR_LOW     0x0300
#define APIC_ICR_HIGH    0x0310

void apic_init(void);
void apic_send_eoi(void);
uint32_t apic_get_id(void);
void apic_send_init(uint8_t apic_id);
void apic_send_sipi(uint8_t apic_id, uint8_t vector);

#endif
