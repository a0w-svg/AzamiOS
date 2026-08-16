/* ============================================================================
 * AzamiOS — Inter-Process Communication (IPC) Header
 * File: kernel/ipc/ipc.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../sched/sched.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define IPC_MSG_MAX_SIZE    256
#define IPC_CHANNEL_DEPTH   64
#define IPC_MAX_CHANNELS    128
#define IPC_MAX_SHMEM       128

/* Well-known channel ID for the display server (azwm) */
#define IPC_CHANNEL_DISPLAY_SERVER  1

typedef struct {
    u32    sender_pid;
    u32    msg_type;
    size_t length;
    u8     data[IPC_MSG_MAX_SIZE];
} ipc_msg_t;

typedef struct ipc_channel {
    u32         channel_id;
    u32         owner_pid;
    spinlock_t  lock;
    ipc_msg_t   messages[IPC_CHANNEL_DEPTH];
    u32         head;
    u32         tail;
    u32         count;
    u32         refcount;
    bool        closed;
    thread_t   *send_wait;
    thread_t   *recv_wait;
} ipc_channel_t;

#define IPC_SHMEM_MAX_PAGES 1024

typedef struct {
    u32         shmem_id;
    phys_addr_t *phys_pages;    /* C-05: dynamically allocated, page_count entries */
    size_t      page_count;
    u32         refcount;
} ipc_shmem_t;

/** ipc_init() — Initialize the IPC subsystem. */
void ipc_init(void);

/** ipc_channel_create() — Allocate a new message channel and register it. */
ipc_channel_t *ipc_channel_create(void);

/** ipc_channel_find(id) — Look up a channel by its numeric ID. */
ipc_channel_t *ipc_channel_find(u32 channel_id);

/** ipc_channel_destroy(chan) — Destroy a message channel. */
void ipc_channel_destroy(ipc_channel_t *chan);

/** ipc_channel_put(chan) — Decrement refcount and free if zero. */
void ipc_channel_put(ipc_channel_t *chan);

/** ipc_channel_send(chan, msg, block) — Send a message over a channel. */
s64 ipc_channel_send(ipc_channel_t *chan, const ipc_msg_t *msg, bool block);

/** ipc_channel_recv(chan, out_msg, block) — Receive a message from a channel. */
s64 ipc_channel_recv(ipc_channel_t *chan, ipc_msg_t *out_msg, bool block);

/** ipc_channel_close_all(proc) — Close all channels owned by a process. */
void ipc_channel_close_all(process_t *proc);

/** ipc_shmem_create(page_count) — Create a shared memory object and register it. */
ipc_shmem_t *ipc_shmem_create(size_t page_count);

/** ipc_shmem_find(id) — Look up a shared memory object by its numeric ID. */
ipc_shmem_t *ipc_shmem_find(u32 shmem_id);

/** ipc_shmem_map(shmem, target_proc, virt_addr, flags) — Map shmem into target address space. */
s64 ipc_shmem_map(ipc_shmem_t *shmem, process_t *target_proc, virt_addr_t virt_addr, u64 flags);

/** ipc_shmem_destroy(shmem) — Destroy a shared memory object. */
void ipc_shmem_destroy(ipc_shmem_t *shmem);

/** ipc_shmem_put(shmem) — Decrement refcount and free if zero. */
void ipc_shmem_put(ipc_shmem_t *shmem);

/** ipc_shmem_unmap(shmem, target_proc, virt_addr) — Unmap shmem from target address space. */
s64 ipc_shmem_unmap(ipc_shmem_t *shmem, process_t *target_proc, virt_addr_t virt_addr);

/** ipc_shmem_unmap_all(proc) — Unmap all shared memory mapped by a process. */
void ipc_shmem_unmap_all(process_t *proc);
