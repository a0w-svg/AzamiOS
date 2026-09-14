/* ============================================================================
 * AzamiOS — AF_PACKET Link-Layer Capture Sockets (packet.c)
 * File: kernel/net/packet.c
 *
 * Complete-Ethernet-frame capture/injection — the primitive a tcpdump-style
 * tool needs that AF_INET/SOCK_RAW (kernel/net/socket.c's raw_sock_t, IP
 * payloads only) can't provide.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/socket.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"

static pkt_sock_t *g_pkt_sockets = NULL;
static spinlock_t  g_pkt_lock = SPINLOCK_INIT;

/* g_pkt_lock and each pkt_sock_t's own ->lock use spinlock_lock_irqsave()/
 * _irqrestore() throughout this file, for the same reason as kernel/net/
 * socket.c's identical note on g_raw_lock: packet_input() runs from a timer
 * interrupt on any CPU (net_poll() -> ... -> net_process_incoming()), while
 * packet_socket_create()/packet_socket_close() and a blocked reader take
 * these locks from ordinary process context. See kernel/net/tcp.c for the
 * full writeup of why a plain spinlock_lock() would deadlock here. */

pkt_sock_t *packet_socket_create(void)
{
    pkt_sock_t *p = (pkt_sock_t *)kzalloc(sizeof(pkt_sock_t));
    if (!p) return NULL;
    net_buf_queue_init(&p->rx_queue);
    spinlock_init(&p->lock);
    p->wait_thread = NULL;

    irqflags_t flags = spinlock_lock_irqsave(&g_pkt_lock);
    p->next = g_pkt_sockets;
    g_pkt_sockets = p;
    spinlock_unlock_irqrestore(&g_pkt_lock, flags);
    return p;
}

void packet_socket_close(pkt_sock_t *pkt)
{
    if (!pkt) return;

    irqflags_t list_flags = spinlock_lock_irqsave(&g_pkt_lock);
    pkt_sock_t **curr = &g_pkt_sockets;
    while (*curr) {
        if (*curr == pkt) {
            *curr = pkt->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock_irqrestore(&g_pkt_lock, list_flags);

    irqflags_t sock_flags = spinlock_lock_irqsave(&pkt->lock);
    net_buf_queue_purge(&pkt->rx_queue);
    if (pkt->wait_thread) {
        sched_unblock(pkt->wait_thread);
        pkt->wait_thread = NULL;
    }
    spinlock_unlock_irqrestore(&pkt->lock, sock_flags);
    kfree(pkt);
}

void packet_input(const u8 *frame, size_t len)
{
    if (!frame || len == 0) return;

    irqflags_t list_flags = spinlock_lock_irqsave(&g_pkt_lock);
    for (pkt_sock_t *cur = g_pkt_sockets; cur; cur = cur->next) {
        net_buf_t *clone = net_buf_alloc(len);
        if (!clone) continue;
        void *p = net_buf_put(clone, len);
        memcpy(p, frame, len);

        net_buf_queue_push(&cur->rx_queue, clone);

        irqflags_t sock_flags = spinlock_lock_irqsave(&cur->lock);
        if (cur->wait_thread) {
            sched_unblock(cur->wait_thread);
            cur->wait_thread = NULL;
        }
        spinlock_unlock_irqrestore(&cur->lock, sock_flags);
    }
    spinlock_unlock_irqrestore(&g_pkt_lock, list_flags);
}

s64 packet_send(const void *frame, size_t len)
{
    if (!frame || len == 0) return -1;
    net_device_t *dev = net_get_default_device();
    if (!dev) return -1;
    dev->send(frame, len);
    return (s64)len;
}
