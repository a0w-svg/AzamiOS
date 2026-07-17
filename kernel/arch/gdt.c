/*
    AzamiOS GDT module - Per-Core GDT & TSS separation
*/

#include <stdint.h>
#include <stdbool.h>
#include "./include/gdt.h"
#include "./include/smp.h"
#include "../klibc/include/string.h"

#define LOW_GDT(value) (uint16_t)(value & 0xFFFF)
#define HIGH_GDT(value) (uint16_t)((value >> 24) & 0xFF)
#define GRAN(value) (uint16_t)((value >> 16) & 0x0F)

gdt_entry_t gdt_table[8]; // Early boot / Core 0 initial GDT table
gdt_ptr_t gdt_pointer;
uint64_t g_cpu_rsp0[MAX_CPUS];

extern void gdt_flush(uintptr_t pointer);
extern bool g_is_uefi;

void set_gdt_gate(int32_t num, uintptr_t base, uint32_t limit, uint8_t access, uint8_t granularity_segment)
{
    gdt_entry_t* gdt_entry = &gdt_table[num];
    gdt_entry->base_low = LOW_GDT(base);
    gdt_entry->base_middle = (base >> 16) & 0xFF;
    gdt_entry->base_high = HIGH_GDT(base);
    gdt_entry->limit_low = LOW_GDT(limit);
    gdt_entry->granularity = (limit >> 16) & 0x0F;
    gdt_entry->granularity |= granularity_segment & 0xF0;
    gdt_entry->access = access;
}

static void set_gdt_gate_raw(gdt_entry_t *table, int32_t num, uintptr_t base, uint32_t limit, uint8_t access, uint8_t granularity_segment)
{
    gdt_entry_t* gdt_entry = &table[num];
    gdt_entry->base_low = LOW_GDT(base);
    gdt_entry->base_middle = (base >> 16) & 0xFF;
    gdt_entry->base_high = HIGH_GDT(base);
    gdt_entry->limit_low = LOW_GDT(limit);
    gdt_entry->granularity = (limit >> 16) & 0x0F;
    gdt_entry->granularity |= granularity_segment & 0xF0;
    gdt_entry->access = access;
}

static void write_tss_cpu(gdt_entry_t *table, tss_t *tss, int32_t num, uintptr_t rsp0) {
    uintptr_t base = (uintptr_t)tss;
    uint32_t limit = sizeof(tss_t) - 1;
    memset(tss, 0, sizeof(tss_t));
    tss->rsp0 = rsp0;
    tss->iopb_offset = sizeof(tss_t);
    set_gdt_gate_raw(table, num, base & 0xFFFFFFFF, limit, 0x89, 0x00);
    uint64_t base_high = base >> 32;
    uint64_t *next_entry = (uint64_t*)&table[num + 1];
    *next_entry = base_high;
}

void set_kernel_stack(uintptr_t esp0) {
    cpu_data_t *cpu = smp_get_current_cpu();
    if (!cpu) return;
    uint32_t id = cpu->cpu_id;
    if (id < MAX_CPUS) {
        g_cpu_rsp0[id] = esp0;
    }
    cpu->tss.rsp0 = esp0;
}

uint8_t kernel_tss_stack[4096] __attribute__((aligned(4096)));

void gdt_init_cpu(uint32_t core_id, uintptr_t kernel_stack_top)
{
    if (core_id >= MAX_CPUS) return;
    cpu_data_t *cpu = &g_cpu_data[core_id];
    cpu->cpu_id = core_id;
    g_cpu_rsp0[core_id] = kernel_stack_top;
    gdt_entry_t *table = cpu->gdt;

    set_gdt_gate_raw(table, 0, 0, 0, 0, 0);                // Index 0 (0x00): Null segment
    set_gdt_gate_raw(table, 1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // Index 1 (0x08): 64-bit Kernel Code (L=1, D=0)
    set_gdt_gate_raw(table, 2, 0, 0xFFFFFFFF, 0x92, 0xAF); // Index 2 (0x10): 64-bit Kernel Data
    set_gdt_gate_raw(table, 3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // Index 3 (0x18): 32-bit Compat User Code (SYSRET base)
    set_gdt_gate_raw(table, 4, 0, 0xFFFFFFFF, 0xF2, 0xAF); // Index 4 (0x20): User Data (SYSRETQ SS 0x23)
    set_gdt_gate_raw(table, 5, 0, 0xFFFFFFFF, 0xFA, 0xAF); // Index 5 (0x28): 64-bit User Code (SYSRETQ CS 0x2B)
    write_tss_cpu(table, &cpu->tss, 6, kernel_stack_top);  // Index 6 & 7 (0x30): 16-byte 64-bit TSS

    gdt_ptr_t gdt_ptr;
    gdt_ptr.limit = (sizeof(gdt_entry_t) * 8) - 1;
    gdt_ptr.base = table;
    gdt_flush((uintptr_t)&gdt_ptr);
    asm volatile("ltr %%ax" : : "a" (0x30));
}

void gdt_init(void)
{
    set_gdt_gate(0, 0, 0, 0, 0);                // Null segment
    set_gdt_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xAF); // 64-bit Kernel Code
    set_gdt_gate(2, 0, 0xFFFFFFFF, 0x92, 0xAF); // 64-bit Kernel Data
    set_gdt_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // 32-bit Compat User Code
    set_gdt_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xAF); // User Data
    set_gdt_gate(5, 0, 0xFFFFFFFF, 0xFA, 0xAF); // 64-bit User Code
    gdt_init_cpu(0, (uintptr_t)kernel_tss_stack + 4096);
}

