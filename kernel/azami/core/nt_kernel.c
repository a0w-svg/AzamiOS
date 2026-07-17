/*
 * azamiOS Core System Implementation - Combined Object Manager, Scheduler & I/O Manager
 * Path: /kernel/azami/core/nt_kernel.c
 * Description: Hardened Object Manager, handle table allocation, priority-based
 *              thread scheduler with priority inheritance, and IRP I/O manager.
 */

#include "../include/azami_core.h"

/* =========================================================================
 * STATIC MEMORY POOLS (Zero Post-Boot Dynamic Allocation)
 * ========================================================================= */

static az_object_t g_object_pool[AZ_MAX_OBJECTS];
static az_spinlock_t g_object_pool_lock;

static az_handle_table_t g_handle_table_pool[AZ_MAX_PROCESSES];
static az_spinlock_t g_handle_table_pool_lock;

static az_irp_t g_irp_pool[AZ_MAX_IRPS];
static az_spinlock_t g_irp_pool_lock;

#define AZ_NUM_PRIORITIES 16U
static az_object_t* g_ready_queues[AZ_NUM_PRIORITIES];
static az_spinlock_t g_scheduler_lock;
static az_object_t* g_current_thread = NULL;

/* =========================================================================
 * INTERNAL FREESTANDING STRING HELPERS
 * ========================================================================= */

static void az_strncpy_internal(char* dst, const char* src, size_t max_len) {
    if (dst == NULL || max_len == 0U) {
        return;
    }
    size_t i = 0U;
    if (src != NULL) {
        while (i < (max_len - 1U) && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

static void az_memset_internal(void* ptr, int value, size_t num) {
    if (ptr != NULL) {
        uint8_t* p = (uint8_t*)ptr;
        for (size_t i = 0U; i < num; i++) {
            p[i] = (uint8_t)value;
        }
    }
}

/* =========================================================================
 * SECTION 1: OBJECT MANAGER INITIALIZATION & IMPLEMENTATION
 * ========================================================================= */

az_status_t az_object_manager_init(void) {
    az_spinlock_init(&g_object_pool_lock);
    az_spinlock_init(&g_handle_table_pool_lock);

    az_spinlock_acquire(&g_object_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_OBJECTS; i++) {
        az_memset_internal(&g_object_pool[i], 0, sizeof(az_object_t));
        g_object_pool[i].is_allocated = false;
        az_spinlock_init(&g_object_pool[i].lock);
    }
    az_spinlock_release(&g_object_pool_lock);

    az_spinlock_acquire(&g_handle_table_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_PROCESSES; i++) {
        az_memset_internal(&g_handle_table_pool[i], 0, sizeof(az_handle_table_t));
        g_handle_table_pool[i].is_allocated = false;
        az_spinlock_init(&g_handle_table_pool[i].lock);
    }
    az_spinlock_release(&g_handle_table_pool_lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_object_create(az_object_type_t type, const char* name, az_object_t** out_object) {
    if (out_object == NULL || type == AZ_OBJ_TYPE_UNKNOWN) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_object = NULL;

    az_object_t* slot = NULL;
    az_spinlock_acquire(&g_object_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_OBJECTS; i++) {
        if (!g_object_pool[i].is_allocated) {
            slot = &g_object_pool[i];
            slot->is_allocated = true;
            break;
        }
    }
    az_spinlock_release(&g_object_pool_lock);

    if (slot == NULL) {
        return AZ_STATUS_INSUFFICIENT_RESOURCES;
    }

    az_spinlock_acquire(&slot->lock);
    slot->type = type;
    atomic_store_explicit(&slot->ref_count, 1, memory_order_release);
    if (name != NULL) {
        az_strncpy_internal(slot->name, name, AZ_MAX_PATH_LEN);
    } else {
        slot->name[0] = '\0';
    }
    az_memset_internal(&slot->data, 0, sizeof(slot->data));
    az_spinlock_release(&slot->lock);

    *out_object = slot;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_object_reference(az_object_t* object) {
    if (object == NULL || !object->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    atomic_fetch_add_explicit(&object->ref_count, 1, memory_order_acq_rel);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_object_dereference(az_object_t* object) {
    if (object == NULL || !object->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    int32_t prev_count = atomic_fetch_sub_explicit(&object->ref_count, 1, memory_order_acq_rel);
    if (prev_count <= 1) {
        az_spinlock_acquire(&g_object_pool_lock);
        az_spinlock_acquire(&object->lock);
        object->is_allocated = false;
        object->type = AZ_OBJ_TYPE_UNKNOWN;
        atomic_store_explicit(&object->ref_count, 0, memory_order_release);
        object->name[0] = '\0';
        az_spinlock_release(&object->lock);
        az_spinlock_release(&g_object_pool_lock);
    }
    return AZ_STATUS_SUCCESS;
}

az_status_t az_handle_table_create(az_handle_table_t** out_table) {
    if (out_table == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_table = NULL;

    az_handle_table_t* slot = NULL;
    az_spinlock_acquire(&g_handle_table_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_PROCESSES; i++) {
        if (!g_handle_table_pool[i].is_allocated) {
            slot = &g_handle_table_pool[i];
            slot->is_allocated = true;
            break;
        }
    }
    az_spinlock_release(&g_handle_table_pool_lock);

    if (slot == NULL) {
        return AZ_STATUS_INSUFFICIENT_RESOURCES;
    }

    az_spinlock_acquire(&slot->lock);
    for (size_t j = 0U; j < AZ_MAX_HANDLES_PER_TABLE; j++) {
        slot->entries[j].object = NULL;
        slot->entries[j].granted_access = 0U;
        slot->entries[j].in_use = false;
    }
    slot->active_count = 0U;
    az_spinlock_release(&slot->lock);

    *out_table = slot;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_handle_table_destroy(az_handle_table_t* table) {
    if (table == NULL || !table->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&table->lock);
    for (size_t i = 0U; i < AZ_MAX_HANDLES_PER_TABLE; i++) {
        if (table->entries[i].in_use) {
            if (table->entries[i].object != NULL) {
                az_object_dereference(table->entries[i].object);
            }
            table->entries[i].object = NULL;
            table->entries[i].in_use = false;
        }
    }
    table->active_count = 0U;
    az_spinlock_release(&table->lock);

    az_spinlock_acquire(&g_handle_table_pool_lock);
    table->is_allocated = false;
    az_spinlock_release(&g_handle_table_pool_lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_handle_create(az_handle_table_t* table, az_object_t* object, uint32_t access, az_handle_t* out_handle) {
    if (table == NULL || !table->is_allocated || object == NULL || out_handle == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_handle = AZ_INVALID_HANDLE;

    az_status_t ref_status = az_object_reference(object);
    if (AZ_ERROR(ref_status)) {
        return ref_status;
    }

    az_spinlock_acquire(&table->lock);
    for (size_t i = 1U; i < AZ_MAX_HANDLES_PER_TABLE; i++) {
        if (!table->entries[i].in_use) {
            table->entries[i].object = object;
            table->entries[i].granted_access = access;
            table->entries[i].in_use = true;
            table->active_count++;
            *out_handle = (az_handle_t)i;
            az_spinlock_release(&table->lock);
            return AZ_STATUS_SUCCESS;
        }
    }
    az_spinlock_release(&table->lock);

    az_object_dereference(object);
    return AZ_STATUS_INSUFFICIENT_RESOURCES;
}

az_status_t az_handle_lookup(az_handle_table_t* table, az_handle_t handle, az_object_type_t expected_type, az_object_t** out_object) {
    if (table == NULL || !table->is_allocated || out_object == NULL || handle == AZ_INVALID_HANDLE || handle >= AZ_MAX_HANDLES_PER_TABLE) {
        return AZ_STATUS_INVALID_HANDLE;
    }
    *out_object = NULL;

    az_spinlock_acquire(&table->lock);
    az_handle_entry_t* entry = &table->entries[handle];
    if (!entry->in_use || entry->object == NULL) {
        az_spinlock_release(&table->lock);
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_object_t* target = entry->object;
    if (expected_type != AZ_OBJ_TYPE_UNKNOWN && target->type != expected_type) {
        az_spinlock_release(&table->lock);
        return AZ_STATUS_OBJECT_TYPE_MISMATCH;
    }

    az_status_t ref_status = az_object_reference(target);
    az_spinlock_release(&table->lock);

    if (AZ_ERROR(ref_status)) {
        return ref_status;
    }

    *out_object = target;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_handle_close(az_handle_table_t* table, az_handle_t handle) {
    if (table == NULL || !table->is_allocated || handle == AZ_INVALID_HANDLE || handle >= AZ_MAX_HANDLES_PER_TABLE) {
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_spinlock_acquire(&table->lock);
    az_handle_entry_t* entry = &table->entries[handle];
    if (!entry->in_use || entry->object == NULL) {
        az_spinlock_release(&table->lock);
        return AZ_STATUS_INVALID_HANDLE;
    }

    az_object_t* target = entry->object;
    entry->object = NULL;
    entry->granted_access = 0U;
    entry->in_use = false;
    if (table->active_count > 0U) {
        table->active_count--;
    }
    az_spinlock_release(&table->lock);

    az_object_dereference(target);
    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * SECTION 2: PRIORITY-BASED THREAD SCHEDULER & PRIORITY INHERITANCE
 * ========================================================================= */

az_status_t az_scheduler_init(void) {
    az_spinlock_init(&g_scheduler_lock);
    az_spinlock_acquire(&g_scheduler_lock);
    for (size_t i = 0U; i < AZ_NUM_PRIORITIES; i++) {
        g_ready_queues[i] = NULL;
    }
    g_current_thread = NULL;
    az_spinlock_release(&g_scheduler_lock);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_thread_create_kernel(az_object_t* process_obj, uint8_t priority, uint64_t entry_point, az_object_t** out_thread) {
    if (out_thread == NULL || priority >= AZ_NUM_PRIORITIES || entry_point == 0U) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_object_t* thread_obj = NULL;
    az_status_t status = az_object_create(AZ_OBJ_TYPE_THREAD, "az_kernel_thread", &thread_obj);
    if (AZ_ERROR(status)) {
        return status;
    }

    az_spinlock_acquire(&thread_obj->lock);
    az_thread_t* th = &thread_obj->data.thread;
    th->tid = (uint32_t)((uintptr_t)thread_obj & 0xFFFFFFFFU);
    th->owner_pid = (process_obj != NULL && process_obj->type == AZ_OBJ_TYPE_PROCESS) ? process_obj->data.process.pid : 0U;
    th->base_priority = priority;
    atomic_store_explicit(&th->current_priority, priority, memory_order_release);
    th->inherited_priority = 0U;
    th->state = AZ_THREAD_STATE_READY;
    th->saved_rip = entry_point;
    th->saved_rsp = 0U;
    atomic_store_explicit(&th->is_preempted, false, memory_order_release);
    th->process_obj = process_obj;
    th->blocked_on_mutex = NULL;
    th->next_ready = NULL;
    if (process_obj != NULL) {
        az_object_reference(process_obj);
    }
    az_spinlock_release(&thread_obj->lock);

    *out_thread = thread_obj;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_scheduler_schedule_thread(az_object_t* thread_obj) {
    if (thread_obj == NULL || thread_obj->type != AZ_OBJ_TYPE_THREAD || !thread_obj->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&g_scheduler_lock);
    az_spinlock_acquire(&thread_obj->lock);
    az_thread_t* th = &thread_obj->data.thread;
    th->state = AZ_THREAD_STATE_READY;
    uint8_t prio = atomic_load_explicit(&th->current_priority, memory_order_acquire);
    if (prio >= AZ_NUM_PRIORITIES) {
        prio = AZ_NUM_PRIORITIES - 1U;
    }

    th->next_ready = NULL;
    if (g_ready_queues[prio] == NULL) {
        g_ready_queues[prio] = thread_obj;
    } else {
        az_object_t* curr = g_ready_queues[prio];
        while (curr->data.thread.next_ready != NULL) {
            curr = curr->data.thread.next_ready;
        }
        curr->data.thread.next_ready = thread_obj;
    }
    az_spinlock_release(&thread_obj->lock);
    az_spinlock_release(&g_scheduler_lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_scheduler_yield(void) {
    az_spinlock_acquire(&g_scheduler_lock);
    az_object_t* current = g_current_thread;
    if (current != NULL) {
        az_spinlock_acquire(&current->lock);
        current->data.thread.state = AZ_THREAD_STATE_READY;
        uint8_t prio = atomic_load_explicit(&current->data.thread.current_priority, memory_order_acquire);
        if (prio >= AZ_NUM_PRIORITIES) {
            prio = AZ_NUM_PRIORITIES - 1U;
        }
        current->data.thread.next_ready = NULL;
        if (g_ready_queues[prio] == NULL) {
            g_ready_queues[prio] = current;
        } else {
            az_object_t* tail = g_ready_queues[prio];
            while (tail->data.thread.next_ready != NULL) {
                tail = tail->data.thread.next_ready;
            }
            tail->data.thread.next_ready = current;
        }
        az_spinlock_release(&current->lock);
    }

    az_object_t* next_thread = NULL;
    for (int32_t p = (int32_t)AZ_NUM_PRIORITIES - 1; p >= 0; p--) {
        if (g_ready_queues[p] != NULL) {
            next_thread = g_ready_queues[p];
            g_ready_queues[p] = next_thread->data.thread.next_ready;
            next_thread->data.thread.next_ready = NULL;
            break;
        }
    }

    if (next_thread != NULL) {
        az_spinlock_acquire(&next_thread->lock);
        next_thread->data.thread.state = AZ_THREAD_STATE_RUNNING;
        az_spinlock_release(&next_thread->lock);
        g_current_thread = next_thread;
    } else {
        g_current_thread = NULL;
    }
    az_spinlock_release(&g_scheduler_lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_scheduler_get_current_thread(az_object_t** out_thread) {
    if (out_thread == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    az_spinlock_acquire(&g_scheduler_lock);
    *out_thread = g_current_thread;
    if (g_current_thread != NULL) {
        az_object_reference(g_current_thread);
    }
    az_spinlock_release(&g_scheduler_lock);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_mutex_acquire_with_priority_inheritance(az_object_t* mutex_obj, az_object_t* calling_thread) {
    if (mutex_obj == NULL || calling_thread == NULL || calling_thread->type != AZ_OBJ_TYPE_THREAD) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&mutex_obj->lock);
    az_spinlock_acquire(&calling_thread->lock);
    az_thread_t* th = &calling_thread->data.thread;
    uint8_t caller_prio = atomic_load_explicit(&th->current_priority, memory_order_acquire);
    az_spinlock_release(&calling_thread->lock);

    if (mutex_obj->data.file.access_flags == 0U) {
        mutex_obj->data.file.access_flags = 1U;
        mutex_obj->data.file.file_offset = (uint64_t)(uintptr_t)calling_thread;
        az_spinlock_release(&mutex_obj->lock);
        return AZ_STATUS_SUCCESS;
    }

    az_object_t* owner_thread = (az_object_t*)(uintptr_t)mutex_obj->data.file.file_offset;
    if (owner_thread != NULL && owner_thread->type == AZ_OBJ_TYPE_THREAD) {
        az_spinlock_acquire(&owner_thread->lock);
        az_thread_t* owner_th = &owner_thread->data.thread;
        uint8_t owner_prio = atomic_load_explicit(&owner_th->current_priority, memory_order_acquire);
        if (caller_prio > owner_prio) {
            atomic_store_explicit(&owner_th->current_priority, caller_prio, memory_order_release);
            owner_th->inherited_priority = caller_prio;
        }
        az_spinlock_release(&owner_thread->lock);
    }
    az_spinlock_release(&mutex_obj->lock);

    return AZ_STATUS_PENDING;
}

az_status_t az_mutex_release_with_priority_inheritance(az_object_t* mutex_obj, az_object_t* calling_thread) {
    if (mutex_obj == NULL || calling_thread == NULL || calling_thread->type != AZ_OBJ_TYPE_THREAD) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&mutex_obj->lock);
    if (mutex_obj->data.file.file_offset != (uint64_t)(uintptr_t)calling_thread) {
        az_spinlock_release(&mutex_obj->lock);
        return AZ_STATUS_ACCESS_DENIED;
    }

    az_spinlock_acquire(&calling_thread->lock);
    az_thread_t* th = &calling_thread->data.thread;
    if (th->inherited_priority > 0U) {
        atomic_store_explicit(&th->current_priority, th->base_priority, memory_order_release);
        th->inherited_priority = 0U;
    }
    az_spinlock_release(&calling_thread->lock);

    mutex_obj->data.file.access_flags = 0U;
    mutex_obj->data.file.file_offset = 0U;
    az_spinlock_release(&mutex_obj->lock);

    return AZ_STATUS_SUCCESS;
}

/* =========================================================================
 * SECTION 3: ASYNCHRONOUS I/O MANAGER IMPLEMENTATION
 * ========================================================================= */

az_status_t az_io_manager_init(void) {
    az_spinlock_init(&g_irp_pool_lock);
    az_spinlock_acquire(&g_irp_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_IRPS; i++) {
        az_memset_internal(&g_irp_pool[i], 0, sizeof(az_irp_t));
        g_irp_pool[i].is_allocated = false;
        az_spinlock_init(&g_irp_pool[i].lock);
    }
    az_spinlock_release(&g_irp_pool_lock);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_allocate(az_irp_major_t major_func, az_object_t* target, az_irp_t** out_irp) {
    if (out_irp == NULL || target == NULL || !target->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_irp = NULL;

    az_irp_t* slot = NULL;
    az_spinlock_acquire(&g_irp_pool_lock);
    for (size_t i = 0U; i < AZ_MAX_IRPS; i++) {
        if (!g_irp_pool[i].is_allocated) {
            slot = &g_irp_pool[i];
            slot->is_allocated = true;
            break;
        }
    }
    az_spinlock_release(&g_irp_pool_lock);

    if (slot == NULL) {
        return AZ_STATUS_INSUFFICIENT_RESOURCES;
    }

    az_spinlock_acquire(&slot->lock);
    slot->major_function = major_func;
    slot->io_status = AZ_STATUS_PENDING;
    slot->information = 0U;
    slot->target_object = target;
    az_object_reference(target);
    slot->calling_thread = g_current_thread;
    if (g_current_thread != NULL) {
        az_object_reference(g_current_thread);
    }
    slot->offset = 0U;
    slot->buffer_length = 0U;
    az_memset_internal(slot->buffer, 0, AZ_IRP_DATA_BUFFER_SIZE);
    atomic_store_explicit(&slot->is_completed, false, memory_order_release);
    atomic_store_explicit(&slot->is_pending, true, memory_order_release);
    slot->completion_routine = NULL;
    slot->completion_context = NULL;
    slot->next = NULL;
    az_spinlock_release(&slot->lock);

    *out_irp = slot;
    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_free(az_irp_t* irp) {
    if (irp == NULL || !irp->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&irp->lock);
    if (irp->target_object != NULL) {
        az_object_dereference(irp->target_object);
        irp->target_object = NULL;
    }
    if (irp->calling_thread != NULL) {
        az_object_dereference(irp->calling_thread);
        irp->calling_thread = NULL;
    }
    irp->is_allocated = false;
    az_spinlock_release(&irp->lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_submit_async(az_irp_t* irp) {
    if (irp == NULL || !irp->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&irp->lock);
    atomic_store_explicit(&irp->is_pending, true, memory_order_release);
    atomic_store_explicit(&irp->is_completed, false, memory_order_release);
    az_spinlock_release(&irp->lock);

    return AZ_STATUS_PENDING;
}

az_status_t az_irp_complete(az_irp_t* irp, az_status_t io_status, uint64_t info) {
    if (irp == NULL || !irp->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&irp->lock);
    irp->io_status = io_status;
    irp->information = info;
    atomic_store_explicit(&irp->is_pending, false, memory_order_release);
    atomic_store_explicit(&irp->is_completed, true, memory_order_release);

    az_irp_completion_routine_t callback = irp->completion_routine;
    void* context = irp->completion_context;
    az_spinlock_release(&irp->lock);

    if (callback != NULL) {
        callback(irp, context);
    }
    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_queue_init(az_irp_queue_t* queue) {
    if (queue == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    az_spinlock_init(&queue->lock);
    az_spinlock_acquire(&queue->lock);
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0U;
    az_spinlock_release(&queue->lock);
    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_queue_push(az_irp_queue_t* queue, az_irp_t* irp) {
    if (queue == NULL || irp == NULL || !irp->is_allocated) {
        return AZ_STATUS_INVALID_PARAMETER;
    }

    az_spinlock_acquire(&queue->lock);
    irp->next = NULL;
    if (queue->tail == NULL) {
        queue->head = irp;
        queue->tail = irp;
    } else {
        queue->tail->next = irp;
        queue->tail = irp;
    }
    queue->count++;
    az_spinlock_release(&queue->lock);

    return AZ_STATUS_SUCCESS;
}

az_status_t az_irp_queue_pop(az_irp_queue_t* queue, az_irp_t** out_irp) {
    if (queue == NULL || out_irp == NULL) {
        return AZ_STATUS_INVALID_PARAMETER;
    }
    *out_irp = NULL;

    az_spinlock_acquire(&queue->lock);
    if (queue->head == NULL) {
        az_spinlock_release(&queue->lock);
        return AZ_STATUS_PENDING;
    }

    az_irp_t* irp = queue->head;
    queue->head = irp->next;
    if (queue->head == NULL) {
        queue->tail = NULL;
    }
    irp->next = NULL;
    queue->count--;
    az_spinlock_release(&queue->lock);

    *out_irp = irp;
    return AZ_STATUS_SUCCESS;
}
