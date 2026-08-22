/* ============================================================================
 * AzamiOS — Packet Buffer Implementation (net_buf.c)
 * File: kernel/net/net_buf.c
 *
 * Implements sk_buff-style buffer allocation, headroom reservation, zero-copy
 * push/pull header modification, reference counting, and FIFO queuing.
 * ============================================================================ */

#include "../../include/azami/net_buf.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/lib/string.h"

net_buf_t *net_buf_alloc(size_t size)
{
    if (size == 0) size = NET_BUF_DEFAULT_SIZE;

    net_buf_t *buf = (net_buf_t *)kzalloc(sizeof(net_buf_t));
    if (!buf) return NULL;

    u8 *data = (u8 *)kmalloc(size);
    if (!data) {
        kfree(buf);
        return NULL;
    }

    buf->head = data;
    buf->data = data;
    buf->tail = data;
    buf->end = data + size;
    buf->len = 0;
    buf->capacity = size;
    buf->refcount = 1;
    buf->dev = NULL;
    buf->protocol = 0;
    buf->next = NULL;

    return buf;
}

net_buf_t *net_buf_clone(net_buf_t *buf)
{
    if (!buf) return NULL;

    net_buf_t *clone = net_buf_alloc(buf->capacity);
    if (!clone) return NULL;

    size_t head_offset = (size_t)(buf->data - buf->head);
    clone->data = clone->head + head_offset;
    clone->tail = clone->data + buf->len;
    clone->len = buf->len;
    clone->protocol = buf->protocol;
    clone->dev = buf->dev;

    if (buf->len > 0) {
        memcpy(clone->data, buf->data, buf->len);
    }

    return clone;
}

net_buf_t *net_buf_ref(net_buf_t *buf)
{
    if (buf) {
        buf->refcount++;
    }
    return buf;
}

void net_buf_free(net_buf_t *buf)
{
    if (!buf) return;

    if (buf->refcount > 1) {
        buf->refcount--;
        return;
    }

    if (buf->head) {
        kfree(buf->head);
        buf->head = NULL;
    }
    kfree(buf);
}

void *net_buf_reserve(net_buf_t *buf, size_t len)
{
    if (!buf) return NULL;
    if (buf->data + len > buf->end) return NULL;

    buf->data += len;
    buf->tail += len;
    return buf->data;
}

void *net_buf_put(net_buf_t *buf, size_t len)
{
    if (!buf) return NULL;
    if (buf->tail + len > buf->end) return NULL;

    void *orig = buf->tail;
    buf->tail += len;
    buf->len += len;
    return orig;
}

void *net_buf_push(net_buf_t *buf, size_t len)
{
    if (!buf) return NULL;
    if (buf->data - len < buf->head) return NULL;

    buf->data -= len;
    buf->len += len;
    return buf->data;
}

void *net_buf_pull(net_buf_t *buf, size_t len)
{
    if (!buf || len > buf->len) return NULL;

    void *orig = buf->data;
    buf->data += len;
    buf->len -= len;
    return orig;
}

/* ── Queue Operations ─────────────────────────────────────────────────────── */

void net_buf_queue_init(net_buf_queue_t *q)
{
    if (!q) return;
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    spinlock_init(&q->lock);
}

void net_buf_queue_push(net_buf_queue_t *q, net_buf_t *buf)
{
    if (!q || !buf) return;

    spinlock_lock(&q->lock);
    buf->next = NULL;

    if (!q->tail) {
        q->head = buf;
        q->tail = buf;
    } else {
        q->tail->next = buf;
        q->tail = buf;
    }
    q->count++;
    spinlock_unlock(&q->lock);
}

net_buf_t *net_buf_queue_pop(net_buf_queue_t *q)
{
    if (!q) return NULL;

    spinlock_lock(&q->lock);
    if (!q->head) {
        spinlock_unlock(&q->lock);
        return NULL;
    }

    net_buf_t *buf = q->head;
    q->head = buf->next;
    if (!q->head) {
        q->tail = NULL;
    }
    buf->next = NULL;
    q->count--;
    spinlock_unlock(&q->lock);

    return buf;
}

void net_buf_queue_purge(net_buf_queue_t *q)
{
    if (!q) return;

    spinlock_lock(&q->lock);
    net_buf_t *cur = q->head;
    while (cur) {
        net_buf_t *next = cur->next;
        net_buf_free(cur);
        cur = next;
    }
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
    spinlock_unlock(&q->lock);
}

size_t net_buf_queue_len(net_buf_queue_t *q)
{
    if (!q) return 0;
    spinlock_lock(&q->lock);
    size_t c = q->count;
    spinlock_unlock(&q->lock);
    return c;
}
