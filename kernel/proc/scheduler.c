/**
 * scheduler.c  –  Preemptive Round-Robin Process Scheduler
 */
#include "include/scheduler.h"
#include "include/process.h"
#include "../arch/include/gdt.h"
#include "../mem/include/paging.h"
#include "../klibc/include/stdio.h"

uint32_t g_active_context = 0;
extern void switch_page_dir(void *page);

static process_t *g_head = NULL;
static process_t *g_current = NULL;
static bool g_sched_enabled = false;

void scheduler_init(void) {
    g_head = NULL;
    g_current = NULL;
    g_sched_enabled = false;
    kprintf("sched: preemptive scheduler initialized\n");
}

void scheduler_add(process_t *proc) {
    if (!g_head) {
        g_head = proc;
        proc->next = proc;
        g_current = proc;
    } else {
        process_t *tail = g_head;
        while (tail->next != g_head) tail = tail->next;
        tail->next = proc;
        proc->next = g_head;
    }
    g_sched_enabled = true;
}

process_t *scheduler_get_current(void) {
    return g_current;
}

void scheduler_schedule(void) {
    if (!g_sched_enabled || !g_current || !g_active_context) return;

    /* Save interrupted kernel esp */
    g_current->kernel_esp = g_active_context;

    /* Advance to next ready process */
    process_t *next = g_current->next;
    int guard = 0;
    while (next->state != PROC_READY && guard++ < 32) {
        next = next->next;
    }

    g_current = next;
    g_active_context = g_current->kernel_esp;

    /* Update hardware TSS ring-0 stack for next interrupt */
    set_kernel_stack(g_current->kernel_stack + 4096);

    /* Switch memory page directory if dirty */
    uint32_t cur_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cur_cr3));
    if (cur_cr3 != g_current->cr3) {
        switch_page_dir((void*)g_current->cr3);
    }
}
