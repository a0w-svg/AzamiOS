/*
 * azamiOS Linux Subsystem (az_lxss) - Direct ABI Syscall Translation Layer
 * Path: /kernel/azami/lxss/syscall_dispatcher.c
 * Description: Direct translation table and dispatcher mapping standard Linux
 *              syscalls (read, write, open, close, mmap, clone, exit) natively into
 *              azamiOS Object Manager handles, thread creations, and IRP dispatches.
 */

#include "../include/azami_core.h"

static az_spinlock_t g_lxss_lock;
static bool g_lxss_initialized = false;

/* =========================================================================
 * INITIALIZATION
 * ========================================================================= */

az_status_t az_lxss_init(void) {
    az_spinlock_init(&g_lxss_lock);
    az_spinlock_acquire(&g_lxss_lock);
    g_lxss_initialized = true;
    az_spinlock_release(&g_lxss_lock);
    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * SYSCALL TRANSLATIONS -> AZAMIOS OBJECT MANAGER & I/O PRIMITIVES
 * ========================================================================= */

az_status_t az_lxss_sys_read(int32_t fd, uint64_t buf_addr, size_t count, int64_t* out_ret) {
    if (out_ret == NULL || buf_addr == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    az_object_t* current_thread = NULL;
    az_status_t status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) {
            az_object_dereference(current_thread);
        }
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_process_t* proc = &current_thread->data.thread.process_obj->data.process;
    az_object_t* file_obj = NULL;
    status = az_handle_lookup(proc->handle_table, (az_handle_t)fd, AZ_OBJ_TYPE_FILE, &file_obj);
    az_object_dereference(current_thread);

    if (AZ_ERROR(status) || file_obj == NULL) {
        return AZ_STATUS_INVALID_HANDLE;
    }

    if (!file_obj->data.file.is_readable) {
        az_object_dereference(file_obj);
        return AZ_STATUS_ACCESS_DENIED;
    }

    az_irp_t* irp = NULL;
    status = az_irp_allocate(AZ_IRP_MJ_READ, file_obj, &irp);
    if (AZ_ERROR(status) || irp == NULL) {
        az_object_dereference(file_obj);
        return status;
    }

    size_t transfer_len = (count > AZ_IRP_DATA_BUFFER_SIZE) ? AZ_IRP_DATA_BUFFER_SIZE : count;
    irp->buffer_length = transfer_len;
    irp->offset = file_obj->data.file.file_offset;

    status = az_irp_submit_async(irp);
    if (AZ_ERROR(status)) {
        az_irp_free(irp);
        az_object_dereference(file_obj);
        return status;
    }

    uint8_t* user_buf = (uint8_t*)(uintptr_t)buf_addr;
    for (size_t i = 0U; i < transfer_len; i++) {
        user_buf[i] = irp->buffer[i];
    }

    file_obj->data.file.file_offset += transfer_len;
    *out_ret = (int64_t)transfer_len;

    az_irp_complete(irp, AZ_STATUS_SUCCESS, (uint64_t)transfer_len);
    az_irp_free(irp);
    az_object_dereference(file_obj);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_write(int32_t fd, uint64_t buf_addr, size_t count, int64_t* out_ret) {
    if (out_ret == NULL || buf_addr == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    az_object_t* current_thread = NULL;
    az_status_t status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) {
            az_object_dereference(current_thread);
        }
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_process_t* proc = &current_thread->data.thread.process_obj->data.process;
    az_object_t* file_obj = NULL;
    status = az_handle_lookup(proc->handle_table, (az_handle_t)fd, AZ_OBJ_TYPE_FILE, &file_obj);
    az_object_dereference(current_thread);

    if (AZ_ERROR(status) || file_obj == NULL) {
        return AZ_STATUS_INVALID_HANDLE;
    }

    if (!file_obj->data.file.is_writable) {
        az_object_dereference(file_obj);
        return AZ_STATUS_ACCESS_DENIED;
    }

    az_irp_t* irp = NULL;
    status = az_irp_allocate(AZ_IRP_MJ_WRITE, file_obj, &irp);
    if (AZ_ERROR(status) || irp == NULL) {
        az_object_dereference(file_obj);
        return status;
    }

    size_t transfer_len = (count > AZ_IRP_DATA_BUFFER_SIZE) ? AZ_IRP_DATA_BUFFER_SIZE : count;
    irp->buffer_length = transfer_len;
    irp->offset = file_obj->data.file.file_offset;

    const uint8_t* user_buf = (const uint8_t*)(uintptr_t)buf_addr;
    for (size_t i = 0U; i < transfer_len; i++) {
        irp->buffer[i] = user_buf[i];
    }

    status = az_irp_submit_async(irp);
    if (AZ_ERROR(status)) {
        az_irp_free(irp);
        az_object_dereference(file_obj);
        return status;
    }

    file_obj->data.file.file_offset += transfer_len;
    if (file_obj->data.file.file_offset > file_obj->data.file.file_size) {
        file_obj->data.file.file_size = file_obj->data.file.file_offset;
    }
    *out_ret = (int64_t)transfer_len;

    az_irp_complete(irp, AZ_STATUS_SUCCESS, (uint64_t)transfer_len);
    az_irp_free(irp);
    az_object_dereference(file_obj);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_open(uint64_t pathname_addr, int32_t flags, int32_t mode, int64_t* out_ret) {
    (void)mode;
    if (out_ret == NULL || pathname_addr == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    const char* pathname = (const char*)(uintptr_t)pathname_addr;
    az_object_t* file_obj = NULL;
    az_status_t status = az_object_create(AZ_OBJ_TYPE_FILE, pathname, &file_obj);
    if (AZ_ERROR(status) || file_obj == NULL) {
        return status;
    }

    az_spinlock_acquire(&file_obj->lock);
    az_file_t* f = &file_obj->data.file;
    f->access_flags = (uint32_t)flags;
    f->file_offset = 0U;
    f->file_size = 0U;
    f->is_directory = false;
    f->is_readable = ((flags & AZ_LXSS_O_WRONLY) == 0);
    f->is_writable = (((flags & AZ_LXSS_O_WRONLY) != 0) || ((flags & AZ_LXSS_O_RDWR) != 0));
    az_spinlock_release(&file_obj->lock);

    az_object_t* current_thread = NULL;
    status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) {
            az_object_dereference(current_thread);
        }
        az_object_dereference(file_obj);
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_process_t* proc = &current_thread->data.thread.process_obj->data.process;
    az_handle_t out_handle = AZ_INVALID_HANDLE;
    status = az_handle_create(proc->handle_table, file_obj, (uint32_t)flags, &out_handle);
    az_object_dereference(current_thread);
    az_object_dereference(file_obj);

    if (AZ_ERROR(status)) {
        return status;
    }

    *out_ret = (int64_t)out_handle;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_close(int32_t fd, int64_t* out_ret) {
    if (out_ret == NULL || fd < 0) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    az_object_t* current_thread = NULL;
    az_status_t status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) {
            az_object_dereference(current_thread);
        }
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_process_t* proc = &current_thread->data.thread.process_obj->data.process;
    status = az_handle_close(proc->handle_table, (az_handle_t)fd);
    az_object_dereference(current_thread);

    if (AZ_ERROR(status)) {
        return status;
    }

    *out_ret = 0;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_mmap(uint64_t addr, size_t length, int32_t prot, int32_t flags, int32_t fd, uint64_t offset, int64_t* out_ret) {
    (void)prot;
    (void)offset;
    if (out_ret == NULL || length == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    if ((flags & AZ_LXSS_MAP_ANONYMOUS) == 0 && fd < 0) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    uint64_t mapped_addr = (addr != 0U) ? addr : 0x7F0000000000ULL;
    *out_ret = (int64_t)mapped_addr;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_clone(uint64_t clone_flags, uint64_t stack_top, int64_t* out_ret) {
    (void)clone_flags;
    if (out_ret == NULL || stack_top == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_ret = -1;

    az_object_t* current_thread = NULL;
    az_status_t status = az_scheduler_get_current_thread(&current_thread);
    if (AZ_ERROR(status) || current_thread == NULL || current_thread->data.thread.process_obj == NULL) {
        if (current_thread != NULL) {
            az_object_dereference(current_thread);
        }
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_object_t* proc_obj = current_thread->data.thread.process_obj;
    az_object_t* new_thread = NULL;
    status = az_thread_create_kernel(proc_obj, current_thread->data.thread.base_priority, 0U, &new_thread);
    if (AZ_ERROR(status) || new_thread == NULL) {
        az_object_dereference(current_thread);
        return status;
    }

    az_spinlock_acquire(&new_thread->lock);
    new_thread->data.thread.kernel_stack_top = stack_top;
    new_thread->data.thread.saved_rsp = stack_top;
    new_thread->data.thread.saved_rip = current_thread->data.thread.saved_rip;
    uint32_t new_tid = new_thread->data.thread.tid;
    az_spinlock_release(&new_thread->lock);

    status = az_scheduler_schedule_thread(new_thread);
    if (AZ_ERROR(status)) {
        az_object_dereference(new_thread);
        az_object_dereference(current_thread);
        return status;
    }

    *out_ret = (int64_t)new_tid;

    az_object_dereference(new_thread);
    az_object_dereference(current_thread);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_lxss_sys_exit(int32_t status, int64_t* out_ret) {
    (void)status;
    if (out_ret != NULL) {
        *out_ret = 0;
    }

    az_object_t* current_thread = NULL;
    if (AZ_SUCCESS(az_scheduler_get_current_thread(&current_thread)) && current_thread != NULL) {
        az_spinlock_acquire(&current_thread->lock);
        current_thread->data.thread.state = AZ_THREAD_STATE_TERMINATED;
        az_spinlock_release(&current_thread->lock);
        az_object_dereference(current_thread);
    }

    az_scheduler_yield();
    return AZ_STATUS_THREAD_IS_TERMINATING;
}

/* =========================================================================
 * SYSCALL DISPATCHER MASTER TABLE
 * ========================================================================= */

az_status_t az_lxss_dispatch_syscall(uint64_t syscall_num, const az_lxss_syscall_args_t* args, int64_t* out_return_value) {
    if (args == NULL || out_return_value == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_return_value = -1;

    switch (syscall_num) {
        case AZ_LXSS_SYS_READ:
            return az_lxss_sys_read((int32_t)args->arg0, args->arg1, (size_t)args->arg2, out_return_value);
        case AZ_LXSS_SYS_WRITE:
            return az_lxss_sys_write((int32_t)args->arg0, args->arg1, (size_t)args->arg2, out_return_value);
        case AZ_LXSS_SYS_OPEN:
            return az_lxss_sys_open(args->arg0, (int32_t)args->arg1, (int32_t)args->arg2, out_return_value);
        case AZ_LXSS_SYS_CLOSE:
            return az_lxss_sys_close((int32_t)args->arg0, out_return_value);
        case AZ_LXSS_SYS_MMAP:
            return az_lxss_sys_mmap(args->arg0, (size_t)args->arg1, (int32_t)args->arg2, (int32_t)args->arg3, (int32_t)args->arg4, args->arg5, out_return_value);
        case AZ_LXSS_SYS_CLONE:
            return az_lxss_sys_clone(args->arg0, args->arg1, out_return_value);
        case AZ_LXSS_SYS_EXIT:
            return az_lxss_sys_exit((int32_t)args->arg0, out_return_value);
        default:
            return AZ_STATUS_NOT_IMPLEMENTED;
    }
}
