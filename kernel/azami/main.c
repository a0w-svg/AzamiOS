/*
 * azamiOS Main Entry Point
 * Path: /kernel/azami/main.c
 * Description: Sequential initialization of Object Manager, Priority Scheduler,
 *              Asynchronous I/O Manager, LXSS, Win32k compositor, and spawning
 *              the initial system root process and init thread.
 */

#include "include/azami_core.h"

/* =========================================================================
 * INITIAL SYSTEM THREAD ENTRY STUB
 * ========================================================================= */

static void az_init_thread_entry(void) {
    while (1) {
        az_scheduler_yield();
    }
}

/* =========================================================================
 * KERNEL MAIN ENTRY POINT
 * ========================================================================= */

az_status_t az_kernel_main(void) {
    az_status_t status = AZ_STATUS_SUCCESS;

    status = az_object_manager_init();
    if (AZ_ERROR(status)) {
        return status;
    }

    status = az_scheduler_init();
    if (AZ_ERROR(status)) {
        return status;
    }

    status = az_io_manager_init();
    if (AZ_ERROR(status)) {
        return status;
    }

    status = az_lxss_init();
    if (AZ_ERROR(status)) {
        return status;
    }

    status = az_win32k_init();
    if (AZ_ERROR(status)) {
        return status;
    }

    az_object_t* init_proc_obj = NULL;
    status = az_object_create(AZ_OBJ_TYPE_PROCESS, "azami_init_process", &init_proc_obj);
    if (AZ_ERROR(status) || init_proc_obj == NULL) {
        return status;
    }

    az_spinlock_acquire(&init_proc_obj->lock);
    az_process_t* proc = &init_proc_obj->data.process;
    proc->pid = 1U;
    proc->parent_pid = 0U;
    proc->page_directory_cr3 = 0x100000ULL;
    atomic_store_explicit(&proc->active_thread_count, 1, memory_order_release);
    proc->is_terminated = false;
    az_spinlock_release(&init_proc_obj->lock);

    status = az_handle_table_create(&init_proc_obj->data.process.handle_table);
    if (AZ_ERROR(status)) {
        az_object_dereference(init_proc_obj);
        return status;
    }

    az_object_t* init_thread_obj = NULL;
    status = az_thread_create_kernel(init_proc_obj, 15U, (uint64_t)(uintptr_t)az_init_thread_entry, &init_thread_obj);
    if (AZ_ERROR(status) || init_thread_obj == NULL) {
        az_handle_table_destroy(init_proc_obj->data.process.handle_table);
        az_object_dereference(init_proc_obj);
        return status;
    }

    status = az_scheduler_schedule_thread(init_thread_obj);
    if (AZ_ERROR(status)) {
        az_object_dereference(init_thread_obj);
        az_handle_table_destroy(init_proc_obj->data.process.handle_table);
        az_object_dereference(init_proc_obj);
        return status;
    }

    az_object_dereference(init_thread_obj);
    az_object_dereference(init_proc_obj);

    return az_scheduler_yield();
}
