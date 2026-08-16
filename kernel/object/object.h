/* ============================================================================
 * AzamiOS — NT-Style Object Manager Header
 * File: kernel/object/object.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../sched/sched.h"

typedef enum {
    AZ_OBJ_UNKNOWN = 0,
    AZ_OBJ_CHANNEL,
    AZ_OBJ_SHMEM,
    AZ_OBJ_PROCESS,
    AZ_OBJ_DEVICE,
    AZ_OBJ_FILE
} az_obj_type_t;

typedef struct az_object {
    char           name[64];
    az_obj_type_t  type;
    u32            ref_count;
    void          *payload;
    void         (*destructor)(void *payload);
    struct az_object *next;
} az_object_t;

/** az_object_manager_init() — Initialize Object Manager namespace root. */
void az_object_manager_init(void);

/** az_object_create(name, type, payload, dtor) — Create and register a new object. */
az_object_t *az_object_create(const char *name, az_obj_type_t type, void *payload, void (*dtor)(void *));

/** az_object_lookup(name) — Find an object by namespace path and reference it. */
az_object_t *az_object_lookup(const char *name);

/** az_object_reference(obj) — Increment reference count on object. */
void az_object_reference(az_object_t *obj);

/** az_object_dereference(obj) — Decrement reference count; destroy if reaches zero. */
void az_object_dereference(az_object_t *obj);

/** az_handle_open(proc, obj) — Insert object into process handle table and return handle ID. */
s64 az_handle_open(process_t *proc, az_object_t *obj);

/** az_handle_get(proc, handle_id) — Get object associated with process handle. */
az_object_t *az_handle_get(process_t *proc, s64 handle_id);

/** az_handle_close(proc, handle_id) — Close process handle and dereference object. */
s64 az_handle_close(process_t *proc, s64 handle_id);
