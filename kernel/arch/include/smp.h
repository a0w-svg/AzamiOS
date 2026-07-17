#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include <stdbool.h>
#include "gdt.h"

#define MAX_CPUS 16

struct process;

typedef struct {
    uint32_t cpu_id;
    bool active;
    uintptr_t active_context;
    struct process *current_proc;
    struct process *idle_proc;
    tss_t tss;
    gdt_entry_t gdt[8];
    uint8_t kernel_tss_stack[4096] __attribute__((aligned(16)));
} cpu_data_t;

extern cpu_data_t g_cpu_data[MAX_CPUS];

void smp_init(void);
cpu_data_t *smp_get_current_cpu(void);
uint32_t smp_get_cpu_count(void);
bool smp_is_active(void);
void smp_tlb_shootdown(uintptr_t virt_addr);

#endif

