#ifndef GDT_H
#define GDT_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) gdt_entry_t;

typedef struct {
    uint16_t limit;
    gdt_entry_t *base;
} __attribute__((packed)) gdt_ptr_t;

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

#define MAX_CPUS 16
extern uint64_t g_cpu_rsp0[MAX_CPUS];

void gdt_init(void);
void gdt_init_cpu(uint32_t core_id, uintptr_t kernel_stack_top);
void set_kernel_stack(uintptr_t stack_top);

#endif