/* ============================================================================
 * AzamiOS — NT-Style Object Manager Implementation
 * File: kernel/object/object.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "object.h"
#include "../mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../syscall/syscall.h" /* EINVAL, EBADF, EMFILE */


/* EMFILE is already defined in defs.h (included via syscall.h) */

static spinlock_t g_object_lock = SPINLOCK_INIT;
static az_object_t *g_object_namespace = NULL;

void az_object_manager_init(void)
{
    pr_debug("[OBJECT] NT-Style Object Manager namespace active.\n");
}

az_object_t *az_object_create(const char *name, az_obj_type_t type, void *payload, void (*dtor)(void *))
{
    az_object_t *obj = (az_object_t *)kzalloc(sizeof(az_object_t));
    if (!obj) return NULL;

    for (int i = 0; name && name[i] && i < 63; i++) {
        obj->name[i] = name[i];
    }
    obj->type = type;
    obj->ref_count = 1;
    obj->payload = payload;
    obj->destructor = dtor;

    spinlock_lock(&g_object_lock);
    obj->next = g_object_namespace;
    g_object_namespace = obj;
    spinlock_unlock(&g_object_lock);

    return obj;
}

az_object_t *az_object_lookup(const char *name)
{
    if (!name) return NULL;

    spinlock_lock(&g_object_lock);
    az_object_t *curr = g_object_namespace;
    while (curr) {
        bool match = true;
        for (int i = 0; i < 64 && (name[i] || curr->name[i]); i++) {
            if (name[i] != curr->name[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            curr->ref_count++;
            spinlock_unlock(&g_object_lock);
            return curr;
        }
        curr = curr->next;
    }
    spinlock_unlock(&g_object_lock);
    return NULL;
}

void az_object_reference(az_object_t *obj)
{
    if (!obj) return;
    spinlock_lock(&g_object_lock);
    obj->ref_count++;
    spinlock_unlock(&g_object_lock);
}

void az_object_dereference(az_object_t *obj)
{
    if (!obj) return;
    spinlock_lock(&g_object_lock);
    if (obj->ref_count > 0) {
        obj->ref_count--;
    }
    if (obj->ref_count == 0) {
        /* Remove from namespace list */
        if (g_object_namespace == obj) {
            g_object_namespace = obj->next;
        } else {
            az_object_t *prev = g_object_namespace;
            while (prev && prev->next != obj) {
                prev = prev->next;
            }
            if (prev) prev->next = obj->next;
        }
        spinlock_unlock(&g_object_lock);

        if (obj->destructor && obj->payload) {
            obj->destructor(obj->payload);
        }
        kfree(obj);
        return;
    }
    spinlock_unlock(&g_object_lock);
}

s64 az_handle_open(process_t *proc, az_object_t *obj)
{
    if (!proc || !obj) return -(s64)EINVAL;

    for (s64 i = 3; i < 64; i++) { /* Reserve 0, 1, 2 for stdio */
        if (proc->obj_handle_table[i] == NULL) {
            proc->obj_handle_table[i] = obj;
            az_object_reference(obj);
            return i;
        }
    }
    return -(s64)EMFILE;
}

az_object_t *az_handle_get(process_t *proc, s64 handle_id)
{
    if (!proc || handle_id < 0 || handle_id >= 64) return NULL;
    return proc->obj_handle_table[handle_id];
}

s64 az_handle_close(process_t *proc, s64 handle_id)
{
    if (!proc || handle_id < 0 || handle_id >= 64) return -(s64)EBADF;
    az_object_t *obj = proc->obj_handle_table[handle_id];
    if (!obj) return -(s64)EBADF;

    proc->obj_handle_table[handle_id] = NULL;
    az_object_dereference(obj);
    return 0;
}
