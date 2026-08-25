/* ============================================================================
 * AzamiOS — Inter-Process Communication (IPC) Implementation
 * File: kernel/ipc/ipc.c
 * ============================================================================ */

#define DEBUG 0
#include <azami/debug.h>
#include "ipc.h"
#include "../mm/kmalloc.h"
#include "../mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../drivers/char/console.h"
#include "../../include/azami/defs.h"
#include "../syscall/syscall.h" /* Error codes like ENOMEM, EAGAIN */


static spinlock_t g_ipc_lock = SPINLOCK_INIT;
static u32 g_next_channel_id = 1;
static u32 g_next_shmem_id = 1;

/* ── Channel registry ─────────────────────────────────────────────────────── */
static ipc_channel_t *g_channel_registry[IPC_MAX_CHANNELS];
static u32             g_channel_count = 0;

/* ── Shared memory registry ───────────────────────────────────────────────── */
static ipc_shmem_t    *g_shmem_registry[IPC_MAX_SHMEM];
static u32             g_shmem_count = 0;

void ipc_init(void)
{
    for (u32 i = 0; i < IPC_MAX_CHANNELS; i++) g_channel_registry[i] = NULL;
    for (u32 i = 0; i < IPC_MAX_SHMEM; i++)    g_shmem_registry[i] = NULL;
    pr_debug("[IPC] Message channels & Shared Memory subsystem ready.\n");
}

/* ── Channel management ───────────────────────────────────────────────────── */

ipc_channel_t *ipc_channel_create(void)
{
    ipc_channel_t *chan = (ipc_channel_t *)kzalloc(sizeof(ipc_channel_t));
    if (!chan) return NULL;

    spinlock_lock(&g_ipc_lock);
    chan->channel_id = g_next_channel_id++;
    chan->lock = (spinlock_t)SPINLOCK_INIT;
    chan->refcount = 1; /* Registry holds one reference */
    chan->closed = false;
    process_t *curr_proc = sched_current_process();
    chan->owner_pid = curr_proc ? curr_proc->pid : 0;

    /* Register in the channel registry */
    if (g_channel_count < IPC_MAX_CHANNELS) {
        g_channel_registry[g_channel_count++] = chan;
    } else {
        spinlock_unlock(&g_ipc_lock);
        kfree(chan);
        return NULL;
    }
    spinlock_unlock(&g_ipc_lock);

    return chan;
}

ipc_channel_t *ipc_channel_find(u32 channel_id)
{
    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < g_channel_count; i++) {
        if (g_channel_registry[i] && g_channel_registry[i]->channel_id == channel_id) {
            ipc_channel_t *chan = g_channel_registry[i];
            __atomic_add_fetch(&chan->refcount, 1, __ATOMIC_SEQ_CST);
            spinlock_unlock(&g_ipc_lock);
            return chan;
        }
    }
    spinlock_unlock(&g_ipc_lock);
    return NULL;
}



/* ── Wait queue helpers ───────────────────────────────────────────────────── */

static void wait_queue_push(thread_t **queue, thread_t *t)
{
    t->next = *queue;
    *queue = t;
}

static thread_t *wait_queue_pop(thread_t **queue)
{
    if (!*queue) return NULL;
    thread_t *t = *queue;
    *queue = t->next;
    t->next = NULL;
    return t;
}

void ipc_channel_put(ipc_channel_t *chan)
{
    if (!chan) return;
    if (__atomic_sub_fetch(&chan->refcount, 1, __ATOMIC_SEQ_CST) == 0) {
        kfree(chan);
    }
}

void ipc_channel_destroy(ipc_channel_t *chan)
{
    if (!chan) return;
    
    spinlock_lock(&g_ipc_lock);
    
    /* Remove from registry */
    for (u32 i = 0; i < g_channel_count; i++) {
        if (g_channel_registry[i] == chan) {
            /* BUG-29: guard against underflow if registry is somehow empty */
            if (g_channel_count == 0) break;
            g_channel_registry[i] = g_channel_registry[--g_channel_count];
            g_channel_registry[g_channel_count] = NULL;
            break;
        }
    }
    spinlock_unlock(&g_ipc_lock);
    
    spinlock_lock(&chan->lock);
    /* Wake up any waiting threads */
    chan->closed = true;
    while (chan->send_wait) {
        thread_t *sender = wait_queue_pop(&chan->send_wait);
        if (sender) sched_unblock(sender);
    }
    while (chan->recv_wait) {
        thread_t *receiver = wait_queue_pop(&chan->recv_wait);
        if (receiver) sched_unblock(receiver);
    }
    spinlock_unlock(&chan->lock);
    
    ipc_channel_put(chan);
}

void ipc_channel_close_all(process_t *proc)
{
    if (!proc) return;
    
    spinlock_lock(&g_ipc_lock);
    /* Collect channels to destroy to avoid deadlock when calling ipc_channel_destroy */
    ipc_channel_t *to_destroy[IPC_MAX_CHANNELS];
    u32 destroy_count = 0;
    
    for (u32 i = 0; i < g_channel_count; i++) {
        if (g_channel_registry[i] && g_channel_registry[i]->owner_pid == proc->pid) {
            ipc_channel_t *chan = g_channel_registry[i];
            __atomic_add_fetch(&chan->refcount, 1, __ATOMIC_SEQ_CST);
            to_destroy[destroy_count++] = chan;
        }
    }
    spinlock_unlock(&g_ipc_lock);
    
    for (u32 i = 0; i < destroy_count; i++) {
        ipc_channel_destroy(to_destroy[i]);
        ipc_channel_put(to_destroy[i]); /* Balance the refcount bump */
    }
}

/* ── Message passing ─────────────────────────────────────────────────────── */

s64 ipc_channel_send(ipc_channel_t *chan, const ipc_msg_t *msg, bool block)
{
    if (!chan || !msg) return -(s64)EINVAL;
    if (msg->length > IPC_MSG_MAX_SIZE) return -(s64)EINVAL;

    for (;;) {
        spinlock_lock(&chan->lock);
        if (chan->closed) {
            spinlock_unlock(&chan->lock);
            return -(s64)EPIPE;
        }

        if (chan->count < IPC_CHANNEL_DEPTH) {
            __builtin_memcpy(&chan->messages[chan->tail], msg, sizeof(ipc_msg_t));
            chan->tail = (chan->tail + 1) % IPC_CHANNEL_DEPTH;
            chan->count++;

            /* Wake up one waiting receiver if any */
            thread_t *receiver = wait_queue_pop(&chan->recv_wait);
            spinlock_unlock(&chan->lock);

            if (receiver) {
                sched_unblock(receiver);
            }
            return 0;
        }

        if (!block) {
            spinlock_unlock(&chan->lock);
            return -(s64)EAGAIN;
        }

        /* Buffer is full: block sender */
        thread_t *curr = sched_current_thread();
        wait_queue_push(&chan->send_wait, curr);
        spinlock_unlock(&chan->lock);

        sched_block(THREAD_BLOCKED);
    }
}

s64 ipc_channel_recv(ipc_channel_t *chan, ipc_msg_t *out_msg, bool block)
{
    if (!chan || !out_msg) return -(s64)EINVAL;

    for (;;) {
        spinlock_lock(&chan->lock);
        
        if (chan->count > 0) {
            __builtin_memcpy(out_msg, &chan->messages[chan->head], sizeof(ipc_msg_t));
            chan->head = (chan->head + 1) % IPC_CHANNEL_DEPTH;
            chan->count--;

            /* Wake up one waiting sender if any */
            thread_t *sender = wait_queue_pop(&chan->send_wait);
            spinlock_unlock(&chan->lock);

            if (sender) {
                sched_unblock(sender);
            }
            return 0;
        }

        if (chan->closed) {
            spinlock_unlock(&chan->lock);
            return -(s64)EPIPE;
        }

        if (!block) {
            spinlock_unlock(&chan->lock);
            return -(s64)EAGAIN;
        }

        /* Buffer is empty: block receiver */
        thread_t *curr = sched_current_thread();
        wait_queue_push(&chan->recv_wait, curr);
        spinlock_unlock(&chan->lock);

        sched_block(THREAD_BLOCKED);
    }
}

/* ── Shared memory management ─────────────────────────────────────────────── */

ipc_shmem_t *ipc_shmem_create(size_t page_count)
{
    if (page_count == 0 || page_count > IPC_SHMEM_MAX_PAGES) return NULL;

    ipc_shmem_t *shmem = (ipc_shmem_t *)kzalloc(sizeof(ipc_shmem_t));
    if (!shmem) return NULL;

    /* C-05: dynamically allocate the phys_pages array */
    shmem->phys_pages = (phys_addr_t *)kzalloc(page_count * sizeof(phys_addr_t));
    if (!shmem->phys_pages) {
        kfree(shmem);
        return NULL;
    }

    /* Allocate pages one-by-one so we don't need a contiguous block */
    size_t allocated = 0;
    for (size_t i = 0; i < page_count; i++) {
        phys_addr_t p = pmm_alloc_page();
        if (!p) {
            /* Roll back already-allocated pages */
            for (size_t j = 0; j < allocated; j++)
                pmm_free_page(shmem->phys_pages[j]);
            kfree(shmem->phys_pages);
            kfree(shmem);
            return NULL;
        }
        /* Zero the page through HHDM */
        u8 *ptr = (u8 *)PHYS_TO_VIRT(p);
        for (size_t b = 0; b < PAGE_SIZE; b++) ptr[b] = 0;
        shmem->phys_pages[i] = p;
        allocated++;
    }

    spinlock_lock(&g_ipc_lock);
    shmem->shmem_id = g_next_shmem_id++;
    shmem->page_count = page_count;
    shmem->refcount = 1;

    if (g_shmem_count < IPC_MAX_SHMEM) {
        g_shmem_registry[g_shmem_count++] = shmem;
    } else {
        spinlock_unlock(&g_ipc_lock);
        for (size_t i = 0; i < page_count; i++) pmm_free_page(shmem->phys_pages[i]);
        kfree(shmem->phys_pages);
        kfree(shmem);
        return NULL;
    }
    spinlock_unlock(&g_ipc_lock);

    return shmem;
}

ipc_shmem_t *ipc_shmem_find(u32 shmem_id)
{
    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < g_shmem_count; i++) {
        if (g_shmem_registry[i] && g_shmem_registry[i]->shmem_id == shmem_id) {
            ipc_shmem_t *shmem = g_shmem_registry[i];
            /* Bump refcount under the lock before releasing it, so the caller
             * holds a reference that prevents concurrent ipc_shmem_put() from
             * freeing the object.  Caller must call ipc_shmem_put() when done. */
            __atomic_add_fetch(&shmem->refcount, 1, __ATOMIC_SEQ_CST);
            spinlock_unlock(&g_ipc_lock);
            return shmem;
        }
    }
    spinlock_unlock(&g_ipc_lock);
    return NULL;
}

s64 ipc_shmem_map(ipc_shmem_t *shmem, process_t *target_proc, virt_addr_t virt_addr, u64 flags)
{
    if (!shmem || !target_proc || (virt_addr & (PAGE_SIZE - 1))) {
        return -(s64)EINVAL;
    }

    phys_addr_t pml4 = target_proc->pml4_phys;
    if (!pml4) return -(s64)EINVAL;

    spinlock_lock(&g_ipc_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SHMEM_PER_PROC; i++) {
        if (target_proc->shmem_maps[i].shmem_id == 0) {
            slot = i;
            break;
        }
    }
    if (slot == -1) {
        spinlock_unlock(&g_ipc_lock);
        return -(s64)ENOMEM;
    }

    target_proc->shmem_maps[slot].shmem_id = shmem->shmem_id;
    target_proc->shmem_maps[slot].virt_addr = virt_addr;
    target_proc->shmem_maps[slot].shmem_ptr = (void *)shmem;
    __atomic_add_fetch(&shmem->refcount, 1, __ATOMIC_SEQ_CST);
    spinlock_unlock(&g_ipc_lock);

    for (size_t i = 0; i < shmem->page_count; i++) {
        vmm_map(pml4, virt_addr + i * PAGE_SIZE, shmem->phys_pages[i], flags | VMM_F_SHARED);
    }

    return 0;
}

void ipc_shmem_put(ipc_shmem_t *shmem)
{
    if (!shmem) return;

    /* Atomically decrement and check: if we reach zero, destroy the object.
     * Using atomic ops (not a spinlock) to mirror ipc_channel_put() and avoid
     * a race where two concurrent put() calls both observe refcount > 0 and
     * both attempt to free the memory. */
    if (__atomic_sub_fetch(&shmem->refcount, 1, __ATOMIC_SEQ_CST) != 0) return;

    /* Refcount reached 0 — remove from registry and free pages */
    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < g_shmem_count; i++) {
        if (g_shmem_registry[i] == shmem) {
            g_shmem_registry[i] = g_shmem_registry[--g_shmem_count];
            g_shmem_registry[g_shmem_count] = NULL;
            break;
        }
    }
    spinlock_unlock(&g_ipc_lock);

    for (size_t i = 0; i < shmem->page_count; i++)
        pmm_free_page(shmem->phys_pages[i]);
    kfree(shmem->phys_pages); /* C-05: free the dynamically allocated array */
    kfree(shmem);
}

void ipc_shmem_destroy(ipc_shmem_t *shmem)
{
    if (!shmem) return;
    
    spinlock_lock(&g_ipc_lock);
    for (u32 i = 0; i < g_shmem_count; i++) {
        if (g_shmem_registry[i] == shmem) {
            g_shmem_registry[i] = g_shmem_registry[--g_shmem_count];
            g_shmem_registry[g_shmem_count] = NULL;
            /* We removed it from the registry, so we can now drop the registry's refcount */
            spinlock_unlock(&g_ipc_lock);
            ipc_shmem_put(shmem);
            return;
        }
    }
    spinlock_unlock(&g_ipc_lock);
}

s64 ipc_shmem_unmap(ipc_shmem_t *shmem, process_t *target_proc, virt_addr_t virt_addr)
{
    if (!target_proc || (virt_addr & (PAGE_SIZE - 1))) {
        return -(s64)EINVAL;
    }

    phys_addr_t pml4 = target_proc->pml4_phys;
    if (!pml4) return -(s64)EINVAL;

    spinlock_lock(&g_ipc_lock);
    int slot = -1;
    for (int i = 0; i < MAX_SHMEM_PER_PROC; i++) {
        if (target_proc->shmem_maps[i].virt_addr == virt_addr &&
            (shmem == NULL || target_proc->shmem_maps[i].shmem_ptr == (void *)shmem)) {
            slot = i;
            shmem = (ipc_shmem_t *)target_proc->shmem_maps[i].shmem_ptr;
            target_proc->shmem_maps[i].shmem_id = 0;
            target_proc->shmem_maps[i].virt_addr = 0;
            target_proc->shmem_maps[i].shmem_ptr = NULL;
            break;
        }
    }
    spinlock_unlock(&g_ipc_lock);

    if (slot == -1 || !shmem) return -(s64)EINVAL;

    for (size_t i = 0; i < shmem->page_count; i++) {
        vmm_unmap(pml4, virt_addr + i * PAGE_SIZE);
    }

    ipc_shmem_put(shmem);

    return 0;
}

void ipc_shmem_unmap_all(process_t *proc)
{
    if (!proc) return;
    for (int i = 0; i < MAX_SHMEM_PER_PROC; i++) {
        spinlock_lock(&g_ipc_lock);
        ipc_shmem_t *shmem = (ipc_shmem_t *)proc->shmem_maps[i].shmem_ptr;
        virt_addr_t vaddr = proc->shmem_maps[i].virt_addr;
        proc->shmem_maps[i].shmem_id = 0;
        proc->shmem_maps[i].virt_addr = 0;
        proc->shmem_maps[i].shmem_ptr = NULL;
        spinlock_unlock(&g_ipc_lock);

        if (shmem && proc->pml4_phys) {
            for (size_t p = 0; p < shmem->page_count; p++) {
                vmm_unmap(proc->pml4_phys, vaddr + p * PAGE_SIZE);
            }
            ipc_shmem_put(shmem);
        }
    }
}
