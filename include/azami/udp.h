/* ============================================================================
 * AzamiOS — User Datagram Protocol Header (udp.h)
 * File: include/azami/udp.h
 *
 * Implements RFC 768 User Datagram Protocol, pseudo-header checksum,
 * ephemeral port allocation, and UDP socket endpoint queues.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "net.h"
#include "ipv4.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define UDP_PORT_EPHEMERAL_START 49152
#define UDP_PORT_EPHEMERAL_END   65535
#define MAX_UDP_SOCKETS          128

typedef struct __attribute__((packed)) {
    u16 src_port;
    u16 dst_port;
    u16 length;
    u16 checksum;
} udp_hdr_t;

typedef struct __attribute__((packed)) {
    u8  src_ip[4];
    u8  dst_ip[4];
    u8  zero;
    u8  protocol;
    u16 udp_length;
} udp_pseudo_hdr_t;

typedef struct udp_sock {
    u16 local_port;
    u8  local_ip[4];
    u16 remote_port;
    u8  remote_ip[4];
    bool bound;
    bool connected;
    net_buf_queue_t rx_queue;
    struct thread  *wait_thread;
    spinlock_t      lock;
    struct udp_sock *next;
} udp_sock_t;

/* Public UDP API */
void        udp_init(void);
udp_sock_t *udp_socket_create(void);
void        udp_socket_close(udp_sock_t *sock);
int         udp_bind(udp_sock_t *sock, const u8 ip[4], u16 port);
int         udp_connect(udp_sock_t *sock, const u8 ip[4], u16 port);
s64         udp_sendto(udp_sock_t *sock, const void *data, size_t len, const u8 dst_ip[4], u16 dst_port);
s64         udp_recvfrom(udp_sock_t *sock, void *buf, size_t max_len, u8 src_ip_out[4], u16 *src_port_out, bool nonblock);
void        udp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr);
bool        udp_poll(udp_sock_t *sock);
u16         udp_checksum(const udp_hdr_t *udp, const ipv4_hdr_t *ip, const void *payload, size_t payload_len);
