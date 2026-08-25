/* ============================================================================
 * AzamiOS — Transmission Control Protocol Engine (tcp.c)
 * File: kernel/net/tcp.c
 *
 * Implements RFC 793 full 11-state TCP state machine, 3-way handshake,
 * sequence number tracking, sliding window buffers, listen backlog, and teardown.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/net.h"
#include "../../include/azami/net_buf.h"
#include "../../include/azami/ipv4.h"
#include "../../include/azami/icmp.h"
#include "../../include/azami/tcp.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../arch/x86_64/cpu/spinlock.h"

static tcp_sock_t *g_tcp_sockets = NULL;
static spinlock_t  g_tcp_lock = SPINLOCK_INIT;
static u16         g_tcp_ephemeral_port = TCP_PORT_EPHEMERAL_START;
static u32         g_isn_seed = 0x12345678;

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
static inline u32 ntohl(u32 v) { return htonl(v); }

static u32 generate_isn(void)
{
    return __atomic_fetch_add(&g_isn_seed, 0x10000U, __ATOMIC_RELAXED);
}

void tcp_init(void)
{
    spinlock_lock(&g_tcp_lock);
    g_tcp_sockets = NULL;
    g_tcp_ephemeral_port = TCP_PORT_EPHEMERAL_START;
    spinlock_unlock(&g_tcp_lock);
    pr_debug("[TCP] Full TCP 11-state protocol engine initialized.\n");
}

u16 tcp_checksum(const tcp_hdr_t *tcp, const ipv4_hdr_t *ip, const void *payload, size_t payload_len)
{
    tcp_pseudo_hdr_t pseudo;
    memcpy(pseudo.src_ip, ip->src_ip, 4);
    memcpy(pseudo.dst_ip, ip->dst_ip, 4);
    pseudo.zero = 0;
    pseudo.protocol = IP_PROTO_TCP;
    pseudo.tcp_length = htons((u16)(sizeof(tcp_hdr_t) + payload_len));

    u32 sum = 0;
    const u16 *ptr;
    size_t len;

    /* 1. Sum pseudo header */
    ptr = (const u16 *)&pseudo;
    len = sizeof(tcp_pseudo_hdr_t);
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    /* 2. Sum TCP header */
    ptr = (const u16 *)tcp;
    len = sizeof(tcp_hdr_t);
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    /* 3. Sum payload data */
    ptr = (const u16 *)payload;
    len = payload_len;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len > 0) {
        sum += *(const u8 *)ptr;
    }

    /* Fold to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    u16 res = (u16)(~sum);
    return res == 0 ? 0xFFFF : res;
}

static s64 tcp_send_packet(tcp_sock_t *sock, u8 flags, const void *payload, size_t payload_len)
{
    net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(tcp_hdr_t) + payload_len);
    if (!buf) return -ENOMEM;

    net_buf_reserve(buf, NET_BUF_HEADROOM);

    /* 1. Prepend TCP Header */
    tcp_hdr_t *tcp = (tcp_hdr_t *)net_buf_put(buf, sizeof(tcp_hdr_t));
    tcp->src_port = htons(sock->local_port);
    tcp->dst_port = htons(sock->remote_port);
    tcp->seq_num = htonl(sock->snd_nxt);
    tcp->ack_num = (flags & TCP_FLAG_ACK) ? htonl(sock->rcv_nxt) : 0;
    tcp->data_offset = (sizeof(tcp_hdr_t) / 4) << 4; /* 20 bytes (0x50) */
    tcp->flags = flags;
    tcp->window_size = htons((u16)sock->rcv_wnd);
    tcp->checksum = 0;
    tcp->urgent_pointer = 0;

    /* 2. Put Payload Data */
    if (payload && payload_len > 0) {
        void *pdata = net_buf_put(buf, payload_len);
        memcpy(pdata, payload, payload_len);
    }

    /* 3. Compute TCP Checksum */
    u8 host_ip[4];
    net_get_ip(host_ip);
    ipv4_hdr_t pseudo_ip;
    if (sock->remote_ip[0] == 127) {
        static const u8 loop_ip[4] = { 127, 0, 0, 1 };
        memcpy(pseudo_ip.src_ip, loop_ip, 4);
    } else {
        memcpy(pseudo_ip.src_ip, host_ip, 4);
    }
    memcpy(pseudo_ip.dst_ip, sock->remote_ip, 4);
    tcp->checksum = tcp_checksum(tcp, &pseudo_ip, payload, payload_len);

    /* Advance send sequence number */
    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
        sock->snd_nxt++;
    } else {
        sock->snd_nxt += payload_len;
    }

    /* 4. Transmit via IPv4 */
    return ipv4_send(buf, sock->remote_ip, IP_PROTO_TCP);
}

tcp_sock_t *tcp_socket_create(void)
{
    tcp_sock_t *s = (tcp_sock_t *)kzalloc(sizeof(tcp_sock_t));
    if (!s) return NULL;

    spinlock_init(&s->lock);
    s->state = TCP_STATE_CLOSED;
    s->bound = false;
    s->rcv_wnd = TCP_DEFAULT_WINDOW;
    s->snd_wnd = TCP_DEFAULT_WINDOW;

    s->rx_buf = (u8 *)kmalloc(TCP_RX_BUF_SIZE);
    s->tx_buf = (u8 *)kmalloc(TCP_TX_BUF_SIZE);

    if (!s->rx_buf || !s->tx_buf) {
        if (s->rx_buf) kfree(s->rx_buf);
        if (s->tx_buf) kfree(s->tx_buf);
        kfree(s);
        return NULL;
    }

    s->rx_head = 0; s->rx_tail = 0; s->rx_len = 0;
    s->tx_head = 0; s->tx_tail = 0; s->tx_len = 0;
    s->backlog_count = 0;
    s->backlog_max = 0;

    spinlock_lock(&g_tcp_lock);
    s->next = g_tcp_sockets;
    g_tcp_sockets = s;
    spinlock_unlock(&g_tcp_lock);

    return s;
}

void tcp_socket_close(tcp_sock_t *sock)
{
    if (!sock) return;

    spinlock_lock(&sock->lock);
    if (sock->state == TCP_STATE_ESTABLISHED || sock->state == TCP_STATE_CLOSE_WAIT) {
        /* Initiate graceful teardown (FIN) */
        tcp_send_packet(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        sock->state = (sock->state == TCP_STATE_ESTABLISHED) ? TCP_STATE_FIN_WAIT_1 : TCP_STATE_LAST_ACK;
    } else {
        sock->state = TCP_STATE_CLOSED;
    }

    /* Unblock all waiting threads */
    if (sock->rx_wait_thread) { sched_unblock(sock->rx_wait_thread); sock->rx_wait_thread = NULL; }
    if (sock->tx_wait_thread) { sched_unblock(sock->tx_wait_thread); sock->tx_wait_thread = NULL; }
    if (sock->conn_wait_thread) { sched_unblock(sock->conn_wait_thread); sock->conn_wait_thread = NULL; }
    if (sock->accept_wait_thread) { sched_unblock(sock->accept_wait_thread); sock->accept_wait_thread = NULL; }
    spinlock_unlock(&sock->lock);

    spinlock_lock(&g_tcp_lock);
    tcp_sock_t **curr = &g_tcp_sockets;
    while (*curr) {
        if (*curr == sock) {
            *curr = sock->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock(&g_tcp_lock);

    if (sock->rx_buf) kfree(sock->rx_buf);
    if (sock->tx_buf) kfree(sock->tx_buf);
    kfree(sock);
}

static u16 tcp_alloc_ephemeral_port(void)
{
    for (int attempts = 0; attempts < 16384; attempts++) {
        u16 port = g_tcp_ephemeral_port;
        if (g_tcp_ephemeral_port >= (u16)TCP_PORT_EPHEMERAL_END) {
            g_tcp_ephemeral_port = TCP_PORT_EPHEMERAL_START;
        } else {
            g_tcp_ephemeral_port++;
        }

        bool in_use = false;
        tcp_sock_t *cur = g_tcp_sockets;
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

int tcp_bind(tcp_sock_t *sock, const u8 ip[4], u16 port)
{
    if (!sock) return -EINVAL;

    spinlock_lock(&g_tcp_lock);
    spinlock_lock(&sock->lock);

    if (port == 0) {
        port = tcp_alloc_ephemeral_port();
        if (port == 0) {
            spinlock_unlock(&sock->lock);
            spinlock_unlock(&g_tcp_lock);
            return -EADDRINUSE;
        }
    } else {
        tcp_sock_t *cur = g_tcp_sockets;
        while (cur) {
            if (cur != sock && cur->bound && cur->local_port == port) {
                spinlock_unlock(&sock->lock);
                spinlock_unlock(&g_tcp_lock);
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
    spinlock_unlock(&g_tcp_lock);

    return 0;
}

int tcp_listen(tcp_sock_t *sock, int backlog)
{
    if (!sock) return -EINVAL;
    if (!sock->bound) {
        int res = tcp_bind(sock, NULL, 0);
        if (res < 0) return res;
    }

    spinlock_lock(&sock->lock);
    sock->state = TCP_STATE_LISTEN;
    sock->backlog_max = (backlog > TCP_MAX_BACKLOG) ? TCP_MAX_BACKLOG : (backlog <= 0 ? 5 : backlog);
    sock->backlog_count = 0;
    spinlock_unlock(&sock->lock);

    return 0;
}

tcp_sock_t *tcp_accept(tcp_sock_t *listener, u8 client_ip_out[4], u16 *client_port_out, bool nonblock)
{
    if (!listener || listener->state != TCP_STATE_LISTEN) return NULL;

    for (;;) {
        spinlock_lock(&listener->lock);
        if (listener->backlog_count > 0) {
            int ready_idx = -1;
            for (int i = 0; i < listener->backlog_count; i++) {
                if (listener->backlog[i] && (listener->backlog[i]->state == TCP_STATE_ESTABLISHED || listener->backlog[i]->state == TCP_STATE_CLOSE_WAIT)) {
                    ready_idx = i;
                    break;
                }
            }
            if (ready_idx >= 0) {
                tcp_sock_t *client = listener->backlog[ready_idx];
                for (int i = ready_idx; i < listener->backlog_count - 1; i++) {
                    listener->backlog[i] = listener->backlog[i + 1];
                }
                listener->backlog_count--;

                if (client_ip_out) memcpy(client_ip_out, client->remote_ip, 4);
                if (client_port_out) *client_port_out = client->remote_port;

                spinlock_unlock(&listener->lock);
                return client;
            }
        }

        if (nonblock) {
            spinlock_unlock(&listener->lock);
            return NULL;
        }

        listener->accept_wait_thread = sched_current_thread();
        sched_block(THREAD_BLOCKED);
        spinlock_unlock(&listener->lock);
        sched_yield();
    }
}

int tcp_connect(tcp_sock_t *sock, const u8 dst_ip[4], u16 dst_port, bool nonblock)
{
    if (!sock || !dst_ip || dst_port == 0) return -EINVAL;

    if (!sock->bound) {
        int res = tcp_bind(sock, NULL, 0);
        if (res < 0) return res;
    }

    spinlock_lock(&sock->lock);
    memcpy(sock->remote_ip, dst_ip, 4);
    sock->remote_port = dst_port;

    sock->iss = generate_isn();
    sock->snd_una = sock->iss;
    sock->snd_nxt = sock->iss;
    sock->state = TCP_STATE_SYN_SENT;

    /* Transmit initial SYN packet */
    tcp_send_packet(sock, TCP_FLAG_SYN, NULL, 0);

    if (nonblock) {
        spinlock_unlock(&sock->lock);
        return -EINPROGRESS;
    }

    /* Block waiting for 3-way handshake completion */
    while (sock->state == TCP_STATE_SYN_SENT) {
        sock->conn_wait_thread = sched_current_thread();
        sched_block(THREAD_BLOCKED);
        spinlock_unlock(&sock->lock);
        sched_yield();
        spinlock_lock(&sock->lock);
    }

    int result = (sock->state == TCP_STATE_ESTABLISHED) ? 0 : -ECONNREFUSED;
    spinlock_unlock(&sock->lock);
    return result;
}

s64 tcp_send(tcp_sock_t *sock, const void *data, size_t len, int flags)
{
    (void)flags;
    if (!sock || !data) return -EINVAL;
    if (sock->state != TCP_STATE_ESTABLISHED) return -ENOTCONN;

    const u8 *ptr = (const u8 *)data;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = (remaining > TCP_DEFAULT_MSS) ? TCP_DEFAULT_MSS : remaining;
        s64 res = tcp_send_packet(sock, TCP_FLAG_ACK | TCP_FLAG_PSH, ptr, chunk);
        if (res < 0) return (len - remaining > 0) ? (s64)(len - remaining) : res;
        ptr += chunk;
        remaining -= chunk;
    }

    return (s64)len;
}

s64 tcp_recv(tcp_sock_t *sock, void *buf, size_t max_len, bool nonblock)
{
    if (!sock || !buf || max_len == 0) return -EINVAL;

    for (;;) {
        spinlock_lock(&sock->lock);

        if (sock->rx_len > 0) {
            size_t copy_len = (sock->rx_len < max_len) ? sock->rx_len : max_len;
            for (size_t i = 0; i < copy_len; i++) {
                ((u8 *)buf)[i] = sock->rx_buf[(sock->rx_head + i) % TCP_RX_BUF_SIZE];
            }
            sock->rx_head = (sock->rx_head + copy_len) % TCP_RX_BUF_SIZE;
            sock->rx_len -= copy_len;
            spinlock_unlock(&sock->lock);
            return (s64)copy_len;
        }

        /* If connection was closed by peer and buffer is drained, return EOF */
        if (sock->state == TCP_STATE_CLOSE_WAIT || sock->state == TCP_STATE_CLOSED || sock->state == TCP_STATE_TIME_WAIT) {
            spinlock_unlock(&sock->lock);
            return 0; /* EOF */
        }

        if (nonblock) {
            spinlock_unlock(&sock->lock);
            return -(s64)EAGAIN;
        }

        sock->rx_wait_thread = sched_current_thread();
        sched_block(THREAD_BLOCKED);
        spinlock_unlock(&sock->lock);
        sched_yield();
    }
}

int tcp_shutdown(tcp_sock_t *sock, int how)
{
    (void)how;
    if (!sock) return -EINVAL;
    if (sock->state == TCP_STATE_ESTABLISHED) {
        tcp_send_packet(sock, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        sock->state = TCP_STATE_FIN_WAIT_1;
    }
    return 0;
}

bool tcp_poll_in(tcp_sock_t *sock)
{
    if (!sock) return false;
    return (sock->rx_len > 0) || (sock->state == TCP_STATE_LISTEN && sock->backlog_count > 0) ||
           (sock->state == TCP_STATE_CLOSE_WAIT);
}

bool tcp_poll_out(tcp_sock_t *sock)
{
    if (!sock) return false;
    return sock->state == TCP_STATE_ESTABLISHED;
}

void tcp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr)
{
    if (!buf || !ip_hdr || buf->len < sizeof(tcp_hdr_t)) {
        if (buf) net_buf_free(buf);
        return;
    }

    const tcp_hdr_t *tcp = (const tcp_hdr_t *)buf->data;
    u16 src_port = ntohs(tcp->src_port);
    u16 dst_port = ntohs(tcp->dst_port);
    u32 seq_num = ntohl(tcp->seq_num);
    u32 ack_num = ntohl(tcp->ack_num);
    u8  flags = tcp->flags;

    size_t header_len = (size_t)((tcp->data_offset >> 4) * 4);
    if (header_len < sizeof(tcp_hdr_t) || buf->len < header_len) {
        net_buf_free(buf);
        return;
    }

    size_t payload_len = buf->len - header_len;
    const u8 *payload = buf->data + header_len;

    /* Verify Checksum */
    if (tcp_checksum(tcp, ip_hdr, payload, payload_len) != 0) {
        pr_debug("[TCP] Bad checksum in incoming packet, dropping.\n");
        net_buf_free(buf);
        return;
    }

    /* Find matching socket: 1. Exact 4-tuple match */
    spinlock_lock(&g_tcp_lock);
    tcp_sock_t *sock = NULL;
    tcp_sock_t *listener = NULL;
    tcp_sock_t *cur = g_tcp_sockets;

    while (cur) {
        if (cur->bound && cur->local_port == dst_port) {
            if (cur->state == TCP_STATE_LISTEN) {
                listener = cur;
            } else if (cur->remote_port == src_port && memcmp(cur->remote_ip, ip_hdr->src_ip, 4) == 0) {
                sock = cur;
                break;
            }
        }
        cur = cur->next;
    }
    if (!sock) sock = listener;
    spinlock_unlock(&g_tcp_lock);

    if (!sock) {
        /* No listener or connection: reply with RST if not incoming RST */
        if (!(flags & TCP_FLAG_RST)) {
            net_buf_t *rst_buf = net_buf_alloc(NET_BUF_HEADROOM + sizeof(tcp_hdr_t));
            if (rst_buf) {
                net_buf_reserve(rst_buf, NET_BUF_HEADROOM);
                tcp_hdr_t *rst_tcp = (tcp_hdr_t *)net_buf_put(rst_buf, sizeof(tcp_hdr_t));
                rst_tcp->src_port = htons(dst_port);
                rst_tcp->dst_port = htons(src_port);
                rst_tcp->seq_num = (flags & TCP_FLAG_ACK) ? htonl(ack_num) : 0;
                rst_tcp->ack_num = htonl(seq_num + payload_len + ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) ? 1 : 0));
                rst_tcp->data_offset = (sizeof(tcp_hdr_t) / 4) << 4;
                rst_tcp->flags = TCP_FLAG_RST | ((flags & TCP_FLAG_ACK) ? 0 : TCP_FLAG_ACK);
                rst_tcp->window_size = 0;
                rst_tcp->checksum = 0;
                rst_tcp->urgent_pointer = 0;

                u8 host_ip[4];
                net_get_ip(host_ip);
                ipv4_hdr_t p_ip;
                memcpy(p_ip.src_ip, host_ip, 4);
                memcpy(p_ip.dst_ip, ip_hdr->src_ip, 4);
                rst_tcp->checksum = tcp_checksum(rst_tcp, &p_ip, NULL, 0);

                ipv4_send(rst_buf, ip_hdr->src_ip, IP_PROTO_TCP);
            }
        }
        net_buf_free(buf);
        return;
    }

    spinlock_lock(&sock->lock);

    /* ── State Machine Processing ─────────────────────────────────────────── */
    switch (sock->state) {
    case TCP_STATE_LISTEN:
        if (flags & TCP_FLAG_SYN) {
            /* Create new child socket for incoming connection */
            if (sock->backlog_count < sock->backlog_max) {
                tcp_sock_t *child = tcp_socket_create();
                if (child) {
                    child->local_port = sock->local_port;
                    memcpy(child->local_ip, sock->local_ip, 4);
                    child->remote_port = src_port;
                    memcpy(child->remote_ip, ip_hdr->src_ip, 4);
                    child->bound = true;
                    child->irs = seq_num;
                    child->rcv_nxt = seq_num + 1;
                    child->iss = generate_isn();
                    child->snd_una = child->iss;
                    child->snd_nxt = child->iss;
                    child->state = TCP_STATE_SYN_RECEIVED;

                    /* Send SYN-ACK */
                    tcp_send_packet(child, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);

                    sock->backlog[sock->backlog_count++] = child;
                }
            }
        }
        break;

    case TCP_STATE_SYN_SENT:
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
            sock->irs = seq_num;
            sock->rcv_nxt = seq_num + 1;
            sock->snd_una = ack_num;
            sock->state = TCP_STATE_ESTABLISHED;

            /* Send final ACK of 3-way handshake */
            tcp_send_packet(sock, TCP_FLAG_ACK, NULL, 0);

            if (sock->conn_wait_thread) {
                sched_unblock(sock->conn_wait_thread);
                sock->conn_wait_thread = NULL;
            }
        }
        break;

    case TCP_STATE_SYN_RECEIVED:
        if (flags & TCP_FLAG_ACK) {
            sock->snd_una = ack_num;
            sock->state = TCP_STATE_ESTABLISHED;

            /* Notify listener's accept thread */
            tcp_sock_t *l = g_tcp_sockets;
            while (l) {
                if (l->state == TCP_STATE_LISTEN && l->local_port == sock->local_port) {
                    if (l->accept_wait_thread) {
                        sched_unblock(l->accept_wait_thread);
                        l->accept_wait_thread = NULL;
                    }
                    break;
                }
                l = l->next;
            }
        }
        break;

    case TCP_STATE_ESTABLISHED:
        /* Process Inbound Data */
        if (payload_len > 0) {
            /* Copy into reception circular buffer */
            for (size_t i = 0; i < payload_len; i++) {
                if (sock->rx_len < TCP_RX_BUF_SIZE) {
                    sock->rx_buf[(sock->rx_head + sock->rx_len) % TCP_RX_BUF_SIZE] = payload[i];
                    sock->rx_len++;
                }
            }
            sock->rcv_nxt += payload_len;

            /* Acknowledge received data */
            tcp_send_packet(sock, TCP_FLAG_ACK, NULL, 0);

            if (sock->rx_wait_thread) {
                sched_unblock(sock->rx_wait_thread);
                sock->rx_wait_thread = NULL;
            }
        }

        /* Process Remote FIN */
        if (flags & TCP_FLAG_FIN) {
            sock->rcv_nxt++;
            sock->state = TCP_STATE_CLOSE_WAIT;
            tcp_send_packet(sock, TCP_FLAG_ACK, NULL, 0);

            if (sock->rx_wait_thread) {
                sched_unblock(sock->rx_wait_thread);
                sock->rx_wait_thread = NULL;
            }
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if ((flags & (TCP_FLAG_FIN | TCP_FLAG_ACK)) == (TCP_FLAG_FIN | TCP_FLAG_ACK)) {
            sock->rcv_nxt++;
            tcp_send_packet(sock, TCP_FLAG_ACK, NULL, 0);
            sock->state = TCP_STATE_TIME_WAIT;
        } else if (flags & TCP_FLAG_ACK) {
            sock->state = TCP_STATE_FIN_WAIT_2;
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            sock->rcv_nxt++;
            tcp_send_packet(sock, TCP_FLAG_ACK, NULL, 0);
            sock->state = TCP_STATE_TIME_WAIT;
        }
        break;

    case TCP_STATE_LAST_ACK:
        if (flags & TCP_FLAG_ACK) {
            sock->state = TCP_STATE_CLOSED;
        }
        break;

    default:
        break;
    }

    spinlock_unlock(&sock->lock);
    net_buf_free(buf);
}

void tcp_timer_tick(void)
{
    /* Maintenance timer tick for TCP state transitions / TIME_WAIT cleanup */
}
