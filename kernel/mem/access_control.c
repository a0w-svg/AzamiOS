/**
 * access_control.c  -  Ring-based memory and I/O access control
 *
 * Implements:
 *   - syscall_validate_ptr()  : validate user ptrs before kernel dereference
 *   - ac_kill_current_process(): kill bad user process, schedule next
 *   - ac_demand_page()        : demand-paging — allocate frame on user fault
 */
#include "include/access_control.h"
#include "include/paging.h"
#include "include/pmm.h"
#include "../klibc/include/stdio.h"
#include "../proc/include/process.h"
#include "../proc/include/scheduler.h"
#include <stdbool.h>

/* ── Syscall pointer validation ──────────────────────────────────────────── */

bool syscall_validate_ptr(const registers_t *r, uintptr_t ptr, uintptr_t size) {
    /* Kernel-mode callers are always trusted */
    if (get_current_ring(r) == RING_KERNEL) return true;

    if (ptr == 0) {
        kprintf("[AC] DENIED: NULL ptr from ring-3 syscall\n");
        return false;
    }

    if (ptr >= USER_SPACE_TOP) {
        kprintf("[AC] DENIED: ring-3 ptr 0x%llx is outside user space!\n",
                (unsigned long long)ptr);
        return false;
    }

    if (size > 0) {
        /* Check for wrap-around and that the whole range stays in user space */
        uintptr_t end = ptr + size;
        if (end < ptr || end > USER_SPACE_TOP) {
            kprintf("[AC] DENIED: ring-3 ptr range 0x%llx+%llu overflows user space\n",
                    (unsigned long long)ptr, (unsigned long long)size);
            return false;
        }
    }

    return true;
}

/* ── Process kill (segfault) ─────────────────────────────────────────────── */

void ac_kill_current_process(const char *reason) {
    process_t *cur = scheduler_get_current();

    if (cur) {
        kprintf("[AC] SEGFAULT: killing process '%s' (PID %u): %s\n",
                cur->name, (unsigned)cur->pid, reason ? reason : "memory violation");
        cur->state = PROC_DEAD;

        /* Try to schedule next ready process */
        if (cur->pid > 1) {
            scheduler_schedule();
            return;
        }

        /*
         * If we are PID 1 or no other process exists, we cannot continue.
         */
        kprintf("[AC] No runnable process after kill. Halting.\n");
    } else {
        /*
         * No scheduler running (exec model — direct enter_usermode path).
         * The user program faulted with no process tracking in place.
         * We cannot return to the kernel exec caller safely, so halt.
         */
        kprintf("[AC] SEGFAULT in exec context (no scheduler): %s\n",
                reason ? reason : "memory violation");
    }

    for (;;) asm volatile("hlt");
}

/* ── Demand paging ───────────────────────────────────────────────────────── */

bool ac_demand_page(uintptr_t fault_addr) {
    /* Only handle user-space addresses */
    if (!is_user_address(fault_addr, 0)) {
        return false;
    }

    /* Align down to 4 KB page boundary */
    uintptr_t page_virt = fault_addr & ~((uintptr_t)(PAGE_SIZE - 1));

    /* Allocate a new physical frame from the PMM */
    void *phys = pmm_alloc_block();
    if (!phys) {
        kprintf("[AC] demand_page: OOM — no physical frame for 0x%x\n",
                (uint32_t)fault_addr);
        return false;
    }

    /* Map virtual → physical as user-accessible, writable */
    paging_map_page((uintptr_t)phys, page_virt,
                    /*is_kernel=*/0,
                    /*is_writable=*/1);

    kprintf("[AC] demand_page: mapped virt=0x%x → phys=0x%x\n",
            (uint32_t)page_virt, (uint32_t)(uintptr_t)phys);

    return true;
}
