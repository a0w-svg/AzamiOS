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
    udp_pseudo_hdr_t pseudo;
    memcpy(pseudo.src_ip, ip->src_ip, 4);
    memcpy(pseudo.dst_ip, ip->dst_ip, 4);
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_UDP;
    pseudo.udp_length = udp->length;

    u32 sum = 0;
    const u16 *ptr;
    size_t len;

    /* 1. Sum pseudo header */
    ptr = (const u16 *)&pseudo;
    len = sizeof(udp_pseudo_hdr_t);
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    /* 2. Sum UDP header */
    ptr = (const u16 *)udp;
    len = sizeof(udp_hdr_t);
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    /* 3. Sum payload */
    ptr = (const u16 *)payload;
    len = payload_len;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const u8 *)ptr;
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    u16 res = (u16)(~sum);
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
    memcpy(pseudo_ip.src_ip, host_ip, 4);
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
            sched_block(THREAD_BLOCKED);
        }
        spinlock_unlock(&sock->lock);
        sched_yield();
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

    /* Verify UDP Checksum if non-zero */
    if (udp->checksum != 0) {
        u16 calc = udp_checksum(udp, ip_hdr, payload, payload_len);
        if (calc != 0 && calc != udp->checksum) {
            pr_debug("[UDP] Bad checksum (0x%04x != 0x%04x), dropping.\n", udp->checksum, calc);
            net_buf_free(buf);
            return;
        }
    }

    /* Check for kernel DHCP client dispatch */
    if (dst_port == DHCP_CLIENT_PORT) {
        net_buf_pull(buf, sizeof(udp_hdr_t));
        dhcp_input(buf, ip_hdr);
        return;
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
    } else {
        /* Port Unreachable */
        icmp_send_dest_unreach(ip_hdr, buf->data, ICMP_CODE_PORT_UNREACH);
    }

    net_buf_free(buf);
}
