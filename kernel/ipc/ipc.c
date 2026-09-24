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
#include "../perf/ktrace.h"


static spinlock_t g_ipc_lock = SPINLOCK_INIT;
static u32 g_next_channel_id = 1;
static u32 g_next_shmem_id = 1;

/* ── Channel registry ─────────────────────────────────────────────────────── */
static ipc_channel_t *g_channel_registry[IPC_MAX_CHANNELS];
static u32             g_channel_count = 0;

/* ── Channel-id hash ──────────────────────────────────────────────────────
 *
 * ipc_channel_find() runs on every az_channel_send() and every
 * az_channel_recv() — on a desktop that is every window message, every input
 * event and every damage report. It used to walk g_channel_registry[]
 * comparing ids, up to IPC_MAX_CHANNELS of them, while holding g_ipc_lock:
 * an O(n) scan inside the one lock every IPC operation on every CPU has to
 * take, so the scan length was also the length of the serialised section.
 *
 * The registry array stays — ipc_cleanup_process() iterates it by owner pid —
 * but lookups go through this chained hash instead. Channel ids are handed
 * out sequentially, so id & (BUCKETS-1) spreads at most IPC_MAX_CHANNELS live
 * channels over 256 buckets and a bucket is almost always one entry long.
 *
 * Lookups take only the bucket's stripe lock, never g_ipc_lock, so sends and
 * receives on unrelated channels no longer serialise against each other or
 * against channel creation. Lock order where both are held (create, destroy)
 * is g_ipc_lock -> stripe lock, never the reverse.
 */
#define IPC_CHAN_HASH_BUCKETS 256u
#define IPC_CHAN_HASH_STRIPES 16u

static ipc_channel_t *g_channel_hash[IPC_CHAN_HASH_BUCKETS];

static spinlock_t g_chan_hash_locks[IPC_CHAN_HASH_STRIPES] = {
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
};

static inline u32 chan_hash_bucket(u32 channel_id)
{
    return channel_id & (IPC_CHAN_HASH_BUCKETS - 1u);
}

static inline spinlock_t *chan_hash_lock(u32 bucket)
{
    return &g_chan_hash_locks[bucket % IPC_CHAN_HASH_STRIPES];
}

/* ── Shared memory registry ───────────────────────────────────────────────── */
static ipc_shmem_t    *g_shmem_registry[IPC_MAX_SHMEM];
static u32             g_shmem_count = 0;

/* Same arrangement as the channel hash above, for the same reason: lookup by
 * id was a scan of the registry under g_ipc_lock. Shared-memory ids are also
 * handed out sequentially. */
#define IPC_SHMEM_HASH_BUCKETS 256u

static ipc_shmem_t *g_shmem_hash[IPC_SHMEM_HASH_BUCKETS];

static spinlock_t g_shmem_hash_locks[IPC_CHAN_HASH_STRIPES] = {
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
    SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT, SPINLOCK_INIT,
};

static inline u32 shmem_hash_bucket(u32 shmem_id)
{
    return shmem_id & (IPC_SHMEM_HASH_BUCKETS - 1u);
}

static inline spinlock_t *shmem_hash_lock(u32 bucket)
{
    return &g_shmem_hash_locks[bucket % IPC_CHAN_HASH_STRIPES];
}

/* Unlink @shmem from the id hash. Caller must not hold the stripe lock. */
static void shmem_hash_remove(ipc_shmem_t *shmem)
{
    u32 bucket = shmem_hash_bucket(shmem->shmem_id);
    spinlock_t *hl = shmem_hash_lock(bucket);
    spinlock_lock(hl);
    ipc_shmem_t **pp = &g_shmem_hash[bucket];
    while (*pp) {
        if (*pp == shmem) { *pp = shmem->hash_next; break; }
        pp = &(*pp)->hash_next;
    }
    shmem->hash_next = NULL;
    spinlock_unlock(hl);
}

void ipc_init(void)
{
    for (u32 i = 0; i < IPC_MAX_CHANNELS; i++) g_channel_registry[i] = NULL;
    for (u32 i = 0; i < IPC_CHAN_HASH_BUCKETS; i++) g_channel_hash[i] = NULL;
    for (u32 i = 0; i < IPC_MAX_SHMEM; i++)    g_shmem_registry[i] = NULL;
    for (u32 i = 0; i < IPC_SHMEM_HASH_BUCKETS; i++) g_shmem_hash[i] = NULL;
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

    /* Publish in the lookup hash. Taken while g_ipc_lock is held, which is
     * the one direction the two locks are ever nested in. */
    {
        u32 bucket = chan_hash_bucket(chan->channel_id);
        spinlock_t *hl = chan_hash_lock(bucket);
        spinlock_lock(hl);
        chan->hash_next = g_channel_hash[bucket];
        g_channel_hash[bucket] = chan;
        spinlock_unlock(hl);
    }
    spinlock_unlock(&g_ipc_lock);

    return chan;
}

ipc_channel_t *ipc_channel_find(u32 channel_id)
{
    u32 bucket = chan_hash_bucket(channel_id);
    spinlock_t *hl = chan_hash_lock(bucket);

    /* The reference is taken under the same stripe lock that
     * ipc_channel_destroy() unlinks under, so a channel cannot be unlinked
     * and dropped between being found here and being referenced. */
    spinlock_lock(hl);
    for (ipc_channel_t *chan = g_channel_hash[bucket]; chan; chan = chan->hash_next) {
        if (chan->channel_id == channel_id) {
            __atomic_add_fetch(&chan->refcount, 1, __ATOMIC_SEQ_CST);
            spinlock_unlock(hl);
            return chan;
        }
    }
    spinlock_unlock(hl);
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

    /* Unlink from the lookup hash. After this no new ipc_channel_find() can
     * reach the channel, so no new reference can be taken on it. */
    {
        u32 bucket = chan_hash_bucket(chan->channel_id);
        spinlock_t *hl = chan_hash_lock(bucket);
        spinlock_lock(hl);
        ipc_channel_t **pp = &g_channel_hash[bucket];
        while (*pp) {
            if (*pp == chan) { *pp = chan->hash_next; break; }
            pp = &(*pp)->hash_next;
        }
        chan->hash_next = NULL;
        spinlock_unlock(hl);
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
    KTRACE_CALL("ipc_channel_send", chan ? chan->channel_id : 0);
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

        /* BUG fix: must use _PENDING variant so a concurrent sched_unblock()
         * that races between push and block sets unblock_pending and
         * sched_post_switch() requeues the thread rather than losing the wakeup. */
        sched_block(THREAD_BLOCKED_PENDING);
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

        /* BUG fix: must use _PENDING variant (see send path above). */
        sched_block(THREAD_BLOCKED_PENDING);
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
        {
            u32 bucket = shmem_hash_bucket(shmem->shmem_id);
            spinlock_t *hl = shmem_hash_lock(bucket);
            spinlock_lock(hl);
            shmem->hash_next = g_shmem_hash[bucket];
            g_shmem_hash[bucket] = shmem;
            spinlock_unlock(hl);
        }
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
    u32 bucket = shmem_hash_bucket(shmem_id);
    spinlock_t *hl = shmem_hash_lock(bucket);

    /* The reference is taken under the same stripe lock the unlink below
     * uses, so the object cannot be dropped between being found and being
     * referenced. Caller must ipc_shmem_put() when done. */
    spinlock_lock(hl);
    for (ipc_shmem_t *shmem = g_shmem_hash[bucket]; shmem; shmem = shmem->hash_next) {
        if (shmem->shmem_id == shmem_id) {
            __atomic_add_fetch(&shmem->refcount, 1, __ATOMIC_SEQ_CST);
            spinlock_unlock(hl);
            return shmem;
        }
    }
    spinlock_unlock(hl);
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
        if (vmm_map(pml4, virt_addr + i * PAGE_SIZE, shmem->phys_pages[i], flags | VMM_F_SHARED) != 0) {
            /* A partial mapping is worse than none: the caller (and every
             * client that trusts az_shmem_map succeeding) would go on
             * writing/reading up to page_count pages, walking straight off
             * the end of what's actually mapped and into a not-present
             * page fault — this is what silently discarding vmm_map()'s
             * return value used to let happen. Tear down what did map and
             * fail the call cleanly instead. */
            vmm_unmap_range(pml4, virt_addr, i, false);

            spinlock_lock(&g_ipc_lock);
            target_proc->shmem_maps[slot].shmem_id  = 0;
            target_proc->shmem_maps[slot].virt_addr = 0;
            target_proc->shmem_maps[slot].shmem_ptr = NULL;
            spinlock_unlock(&g_ipc_lock);
            ipc_shmem_put(shmem); /* release the refcount taken above */

            return -(s64)ENOMEM;
        }
    }

    return 0;
}

void ipc_shmem_put(ipc_shmem_t *shmem)
{
    if (!shmem) return;

    /*
     * Drop the reference and decide the object's fate under the same stripe
     * lock ipc_shmem_find() takes.
     *
     * Doing the decrement outside that lock leaves a window: the count can
     * reach zero while the object is still reachable through the id hash (and
     * previously, the registry), so a concurrent find() can raise it back to
     * one and return the object to a caller — which this path then frees
     * underneath them. Deciding under the lookup lock makes "the count hit
     * zero" and "nobody can find it any more" one indivisible step.
     */
    u32 bucket = shmem_hash_bucket(shmem->shmem_id);
    spinlock_t *hl = shmem_hash_lock(bucket);

    spinlock_lock(hl);
    if (__atomic_sub_fetch(&shmem->refcount, 1, __ATOMIC_SEQ_CST) != 0) {
        spinlock_unlock(hl);
        return;
    }
    /* Zero, and no find() can have slipped in: unlink before letting go. */
    {
        ipc_shmem_t **pp = &g_shmem_hash[bucket];
        while (*pp) {
            if (*pp == shmem) { *pp = shmem->hash_next; break; }
            pp = &(*pp)->hash_next;
        }
        shmem->hash_next = NULL;
    }
    spinlock_unlock(hl);

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
    shmem_hash_remove(shmem);
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

    /* Batched: one TLB shootdown for the whole segment instead of one per
     * page. The frames belong to the shmem object, so nothing is freed here. */
    vmm_unmap_range(pml4, virt_addr, shmem->page_count, false);

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
            vmm_unmap_range(proc->pml4_phys, vaddr, shmem->page_count, false);
            ipc_shmem_put(shmem);
        }
    }
}
