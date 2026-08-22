/* ============================================================================
 * AzamiOS — Packet Buffer Abstraction (net_buf.h)
 * File: include/azami/net_buf.h
 *
 * Zero-copy network packet buffer (sk_buff equivalent) for header encapsulation,
 * decapsulation, queuing, and reference counting.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define NET_BUF_DEFAULT_SIZE 2048
#define NET_BUF_HEADROOM     128

typedef struct net_buf {
    u8             *head;       /* Start of allocated buffer                  */
    u8             *data;       /* Start of active payload                    */
    u8             *tail;       /* End of active payload                      */
    u8             *end;        /* End of allocated buffer                    */
    size_t          len;        /* Length of active data (tail - data)        */
    size_t          capacity;   /* Total buffer capacity                      */
    u32             refcount;   /* Reference counter                          */
    struct net_device *dev;     /* Network interface associated with buffer   */
    u16             protocol;   /* Packet protocol (ETH_P_IP, ETH_P_ARP, etc) */
    struct net_buf *next;       /* Linked list pointer for packet queues      */
} net_buf_t;

typedef struct {
    net_buf_t  *head;
    net_buf_t  *tail;
    size_t      count;
    spinlock_t  lock;
} net_buf_queue_t;

/* Buffer lifecycle */
net_buf_t *net_buf_alloc(size_t size);
net_buf_t *net_buf_clone(net_buf_t *buf);
net_buf_t *net_buf_ref(net_buf_t *buf);
void       net_buf_free(net_buf_t *buf);

/* Header manipulation */
void *net_buf_reserve(net_buf_t *buf, size_t len);
void *net_buf_put(net_buf_t *buf, size_t len);
void *net_buf_push(net_buf_t *buf, size_t len);
void *net_buf_pull(net_buf_t *buf, size_t len);

/* Thread-safe queue operations */
void       net_buf_queue_init(net_buf_queue_t *q);
void       net_buf_queue_push(net_buf_queue_t *q, net_buf_t *buf);
net_buf_t *net_buf_queue_pop(net_buf_queue_t *q);
void       net_buf_queue_purge(net_buf_queue_t *q);
size_t     net_buf_queue_len(net_buf_queue_t *q);
