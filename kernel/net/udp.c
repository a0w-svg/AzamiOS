/* ============================================================================
 * AzamiOS — User Datagram Protocol Engine (udp.c)
 * File: kernel/net/udp.c
 *
 * Implements RFC 768 UDP protocol, checksum verification, ephemeral port
 * allocation, datagram queuing, and non-blocking/blocking reception.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/net_buf.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/icmp.h"
#include "../../include/azami/udp.h"
#include "../../include/azami/dhcp.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

static udp_sock_t *g_udp_sockets = NULL;
static spinlock_t  g_udp_lock = SPINLOCK_INIT;
static u16         g_next_ephemeral_port = UDP_PORT_EPHEMERAL_START;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }

void udp_init(void)
{
    spinlock_lock(&g_udp_lock);
    g_udp_sockets = NULL;
    g_next_ephemeral_port = UDP_PORT_EPHEMERAL_START;
    spinlock_unlock(&g_udp_lock);
    pr_debug("[UDP] User Datagram Protocol engine initialized.\n");
}

u16 udp_checksum(const udp_hdr_t *udp, const ipv4_hdr_t *ip, const void *payload, size_t payload_len)
{
    /* The pseudo-header is assembled as plain bytes rather than a struct so
     * that building it and summing it cannot be reordered against each other
     * (see net_checksum_partial). */
    u8 pseudo[12];
    memcpy(pseudo + 0, ip->src_ip, 4);
    memcpy(pseudo + 4, ip->dst_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    memcpy(pseudo + 10, &udp->length, 2);   /* already in network order */

    /* The caller must have zeroed udp->checksum; it is covered by the sum. */
    u32 sum = net_checksum_partial(pseudo, sizeof(pseudo), 0);
    sum = net_checksum_partial(udp, sizeof(udp_hdr_t), sum);
    sum = net_checksum_partial(payload, payload_len, sum);

    u16 res = net_checksum_fold(sum);
    /* A computed zero is transmitted as all-ones, which is how UDP
     * distinguishes "checksum present" from "checksum omitted". */
    return res == 0 ? 0xFFFF : res;
}

udp_sock_t *udp_socket_create(void)
{
    udp_sock_t *s = (udp_sock_t *)kzalloc(sizeof(udp_sock_t));
    if (!s) return NULL;

    spinlock_init(&s->lock);
    net_buf_queue_init(&s->rx_queue);
    s->bound = false;
    s->connected = false;
    s->wait_thread = NULL;

    spinlock_lock(&g_udp_lock);
    s->next = g_udp_sockets;
    g_udp_sockets = s;
    spinlock_unlock(&g_udp_lock);

    return s;
}

void udp_socket_close(udp_sock_t *sock)
{
    if (!sock) return;

    spinlock_lock(&g_udp_lock);
    udp_sock_t **curr = &g_udp_sockets;
    while (*curr) {
        if (*curr == sock) {
            *curr = sock->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock(&g_udp_lock);

    spinlock_lock(&sock->lock);
    net_buf_queue_purge(&sock->rx_queue);
    if (sock->wait_thread) {
        sched_unblock(sock->wait_thread);
        sock->wait_thread = NULL;
    }
    spinlock_unlock(&sock->lock);

    kfree(sock);
}

static u16 udp_alloc_ephemeral_port(void)
{
    for (int attempts = 0; attempts < 16384; attempts++) {
        u16 port = g_next_ephemeral_port;
        if (g_next_ephemeral_port >= (u16)UDP_PORT_EPHEMERAL_END) {
            g_next_ephemeral_port = UDP_PORT_EPHEMERAL_START;
        } else {
            g_next_ephemeral_port++;
        }

        /* Check collision */
        bool in_use = false;
        udp_sock_t *cur = g_udp_sockets;
        while (cur) {
            if (cur->bound && cur->local_port == port) {
                in_use = true;
                break;
            }
            cur = cur->next;
        }
        if (!in_use) return port;
    }
    return 0;
}

int udp_bind(udp_sock_t *sock, const u8 ip[4], u16 port)
{
    if (!sock) return -EINVAL;

    spinlock_lock(&g_udp_lock);
    spinlock_lock(&sock->lock);

    if (port == 0) {
        port = udp_alloc_ephemeral_port();
        if (port == 0) {
            spinlock_unlock(&sock->lock);
            spinlock_unlock(&g_udp_lock);
            return -EADDRINUSE;
        }
    } else {
        /* Check if port is already taken */
        udp_sock_t *cur = g_udp_sockets;
        while (cur) {
            if (cur != sock && cur->bound && cur->local_port == port) {
                spinlock_unlock(&sock->lock);
                spinlock_unlock(&g_udp_lock);
                return -EADDRINUSE;
            }
            cur = cur->next;
        }
    }

    sock->local_port = port;
    if (ip) memcpy(sock->local_ip, ip, 4);
    else memset(sock->local_ip, 0, 4);
    sock->bound = true;

    spinlock_unlock(&sock->lock);
    spinlock_unlock(&g_udp_lock);

    return 0;
}

int udp_connect(udp_sock_t *sock, const u8 ip[4], u16 port)
{
    if (!sock || !ip || port == 0) return -EINVAL;

    if (!sock->bound) {
        int res = udp_bind(sock, NULL, 0);
        if (res < 0) return res;
    }

    spinlock_lock(&sock->lock);
    memcpy(sock->remote_ip, ip, 4);
    sock->remote_port = port;
    sock->connected = true;
    spinlock_unlock(&sock->lock);

    return 0;
}

s64 udp_sendto(udp_sock_t *sock, const void *data, size_t len, const u8 dst_ip[4], u16 dst_port)
{
    if (!sock || !data) return -EINVAL;

    if (!sock->bound) {
        int res = udp_bind(sock, NULL, 0);
        if (res < 0) return res;
    }

    u8 target_ip[4];
    u16 target_port = dst_port;

    if (dst_ip && dst_port > 0) {
        memcpy(target_ip, dst_ip, 4);
    } else if (sock->connected) {
        memcpy(target_ip, sock->remote_ip, 4);
        target_port = sock->remote_port;
    } else {
        return -EDESTADDRREQ;
    }

    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(udp_hdr_t) + len);
    if (!buf) return -ENOMEM;

    net_buf_reserve(buf, NET_BUF_HEADROOM);

    /* 1. Put UDP Header */
    udp_hdr_t *udp = (udp_hdr_t *)net_buf_put(buf, sizeof(udp_hdr_t));
    udp->src_port = htons(sock->local_port);
    udp->dst_port = htons(target_port);
    udp->length = htons((u16)(sizeof(udp_hdr_t) + len));
    udp->checksum = 0;

    /* 2. Put Payload Data */
    void *payload = net_buf_put(buf, len);
    memcpy(payload, data, len);

    /* 3. Compute UDP Checksum */
    u8 host_ip[4];
    net_get_ip(host_ip);
    ipv4_hdr_t pseudo_ip;
    if (target_ip[0] == 127) {
        static const u8 loop_src[4] = { 127, 0, 0, 1 };
        memcpy(pseudo_ip.src_ip, loop_src, 4);
    } else {
        memcpy(pseudo_ip.src_ip, host_ip, 4);
    }
    memcpy(pseudo_ip.dst_ip, target_ip, 4);
    udp->checksum = udp_checksum(udp, &pseudo_ip, payload, len);

    /* 4. Transmit via IPv4 */
    int ret = ipv4_send(buf, target_ip, IP_PROTO_UDP);
    if (ret < 0) return ret;

    return (s64)len;
}

s64 udp_recvfrom(udp_sock_t *sock, void *buf, size_t max_len, u8 src_ip_out[4], u16 *src_port_out, bool nonblock)
{
    if (!sock || !buf || max_len == 0) return -EINVAL;

    if (!sock->bound) {
        int res = udp_bind(sock, NULL, 0);
        if (res < 0) return res;
    }

    for (;;) {
        net_buf_t *pkt = net_buf_queue_pop(&sock->rx_queue);
        if (pkt) {
            /* Packet buffer contains: [src_ip 4B][src_port 2B][payload ...] */
            if (pkt->len < 6) {
                net_buf_free(pkt);
                continue;
            }

            if (src_ip_out) memcpy(src_ip_out, pkt->data, 4);
            if (src_port_out) {
                u16 sp;
                memcpy(&sp, pkt->data + 4, 2);
                *src_port_out = ntohs(sp);
            }

            size_t payload_len = pkt->len - 6;
            size_t copy_len = (payload_len < max_len) ? payload_len : max_len;
            memcpy(buf, pkt->data + 6, copy_len);

            net_buf_free(pkt);
            return (s64)copy_len;
        }

        if (nonblock) {
            return -(s64)EAGAIN;
        }

        /* Sleep waiting for datagram */
        spinlock_lock(&sock->lock);
        if (net_buf_queue_len(&sock->rx_queue) == 0) {
            sock->wait_thread = sched_current_thread();
            spinlock_unlock(&sock->lock);
            sched_block(THREAD_BLOCKED_PENDING);
        } else {
            spinlock_unlock(&sock->lock);
        }
    }
}

bool udp_poll(udp_sock_t *sock)
{
    if (!sock) return false;
    return net_buf_queue_len(&sock->rx_queue) > 0;
}

void udp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr)
{
    if (!buf || !ip_hdr || buf->len < sizeof(udp_hdr_t)) {
        if (buf) net_buf_free(buf);
        return;
    }

    const udp_hdr_t *udp = (const udp_hdr_t *)buf->data;
    u16 src_port = ntohs(udp->src_port);
    u16 dst_port = ntohs(udp->dst_port);
    u16 udp_len = ntohs(udp->length);

    if (udp_len < sizeof(udp_hdr_t) || buf->len < udp_len) {
        net_buf_free(buf);
        return;
    }

    size_t payload_len = udp_len - sizeof(udp_hdr_t);
    const u8 *payload = buf->data + sizeof(udp_hdr_t);

    /*
     * Verify the checksum whenever the sender supplied one — including on
     * loopback.  Skipping it there costs nothing but hides transmit-side
     * checksum bugs from every local test, which is exactly how a wrong
     * checksum on every outgoing datagram went unnoticed.
     */
    if (udp->checksum != 0) {
        udp_hdr_t udp_zero = *udp;
        udp_zero.checksum = 0;
        u16 calc = udp_checksum(&udp_zero, ip_hdr, payload, payload_len);
        if (calc != udp->checksum) {
            pr_debug("[UDP] Bad checksum (0x%04x != 0x%04x), dropping.\n", udp->checksum, calc);
            net_buf_free(buf);
            return;
        }
    }



    /* Check for kernel DHCP client dispatch */
    if (dst_port == DHCP_CLIENT_PORT) {
        net_buf_t *dhcp_clone = net_buf_clone(buf);
        if (dhcp_clone) {
            net_buf_pull(dhcp_clone, sizeof(udp_hdr_t));
            dhcp_input(dhcp_clone, ip_hdr);
        }
    }

    /* Find matching socket */
    spinlock_lock(&g_udp_lock);
    udp_sock_t *target_sock = NULL;
    udp_sock_t *cur = g_udp_sockets;
    while (cur) {
        if (cur->bound && cur->local_port == dst_port) {
            target_sock = cur;
            break;
        }
        cur = cur->next;
    }
    spinlock_unlock(&g_udp_lock);

    if (target_sock) {
        /* Allocate a packet buffer containing [src_ip 4B][src_port 2B][payload] */
        net_buf_t *rx_buf = net_buf_alloc(6 + payload_len);
        if (rx_buf) {
            u8 *p = (u8 *)net_buf_put(rx_buf, 6 + payload_len);
            memcpy(p, ip_hdr->src_ip, 4);
            u16 sp_net = htons(src_port);
            memcpy(p + 4, &sp_net, 2);
            memcpy(p + 6, payload, payload_len);

            net_buf_queue_push(&target_sock->rx_queue, rx_buf);

            /* Wakeup sleeping thread if waiting */
            spinlock_lock(&target_sock->lock);
            if (target_sock->wait_thread) {
                sched_unblock(target_sock->wait_thread);
                target_sock->wait_thread = NULL;
            }
            spinlock_unlock(&target_sock->lock);
        }
    } else if (dst_port != DHCP_CLIENT_PORT) {
        /* Port Unreachable */
        icmp_send_dest_unreach(ip_hdr, buf->data, buf->len, ICMP_CODE_PORT_UNREACH);
    }

    net_buf_free(buf);
}
