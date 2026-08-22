/* ============================================================================
 * AzamiOS — Transmission Control Protocol Header (tcp.h)
 * File: include/azami/tcp.h
 *
 * Implements RFC 793 Transmission Control Protocol, 11-state state machine,
 * 3-way handshake, sequence/acknowledgment tracking, sliding window buffers,
 * connection backlog queue, and teardown.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "net.h"
#include "ipv4.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define TCP_PORT_EPHEMERAL_START 49152
#define TCP_PORT_EPHEMERAL_END   65535
#define MAX_TCP_SOCKETS          128
#define TCP_MAX_BACKLOG          16
#define TCP_RX_BUF_SIZE          65536
#define TCP_TX_BUF_SIZE          65536
#define TCP_DEFAULT_MSS          1460
#define TCP_DEFAULT_WINDOW       65535

/* TCP Header Flags */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10
#define TCP_FLAG_URG 0x20
#define TCP_FLAG_ECE 0x40
#define TCP_FLAG_CWR 0x80

/* TCP 11 States */
typedef enum {
    TCP_STATE_CLOSED       = 0,
    TCP_STATE_LISTEN       = 1,
    TCP_STATE_SYN_SENT     = 2,
    TCP_STATE_SYN_RECEIVED = 3,
    TCP_STATE_ESTABLISHED  = 4,
    TCP_STATE_FIN_WAIT_1   = 5,
    TCP_STATE_FIN_WAIT_2   = 6,
    TCP_STATE_CLOSE_WAIT   = 7,
    TCP_STATE_CLOSING      = 8,
    TCP_STATE_LAST_ACK     = 9,
    TCP_STATE_TIME_WAIT    = 10,
} tcp_state_t;

typedef struct __attribute__((packed)) {
    u16 src_port;
    u16 dst_port;
    u32 seq_num;
    u32 ack_num;
    u8  data_offset; /* (offset >> 4) * 4 bytes */
    u8  flags;
    u16 window_size;
    u16 checksum;
    u16 urgent_pointer;
} tcp_hdr_t;

typedef struct __attribute__((packed)) {
    u8  src_ip[4];
    u8  dst_ip[4];
    u8  zero;
    u8  protocol; /* 6 */
    u16 tcp_length;
} tcp_pseudo_hdr_t;

typedef struct tcp_sock {
    u16              local_port;
    u8               local_ip[4];
    u16              remote_port;
    u8               remote_ip[4];
    tcp_state_t      state;
    bool             bound;

    /* Sequence number tracking */
    u32              snd_una;    /* Send unacknowledged                        */
    u32              snd_nxt;    /* Next sequence number to send               */
    u32              snd_wnd;    /* Send window                                */
    u32              rcv_nxt;    /* Next sequence number expected from peer    */
    u32              rcv_wnd;    /* Receive window advertisement               */
    u32              iss;        /* Initial send sequence number               */
    u32              irs;        /* Initial receive sequence number            */

    /* Stream reception circular ring buffer */
    u8              *rx_buf;
    size_t           rx_head;
    size_t           rx_tail;
    size_t           rx_len;

    /* Stream transmission circular ring buffer */
    u8              *tx_buf;
    size_t           tx_head;
    size_t           tx_tail;
    size_t           tx_len;

    /* Listen connection backlog */
    struct tcp_sock *backlog[TCP_MAX_BACKLOG];
    int              backlog_count;
    int              backlog_max;

    /* Thread wait queues */
    struct thread   *rx_wait_thread;
    struct thread   *tx_wait_thread;
    struct thread   *conn_wait_thread;
    struct thread   *accept_wait_thread;

    spinlock_t       lock;
    struct tcp_sock *next;
} tcp_sock_t;

/* Public TCP API */
void        tcp_init(void);
tcp_sock_t *tcp_socket_create(void);
void        tcp_socket_close(tcp_sock_t *sock);
int         tcp_bind(tcp_sock_t *sock, const u8 ip[4], u16 port);
int         tcp_listen(tcp_sock_t *sock, int backlog);
tcp_sock_t *tcp_accept(tcp_sock_t *listener, u8 client_ip_out[4], u16 *client_port_out, bool nonblock);
int         tcp_connect(tcp_sock_t *sock, const u8 dst_ip[4], u16 dst_port, bool nonblock);
s64         tcp_send(tcp_sock_t *sock, const void *data, size_t len, int flags);
s64         tcp_recv(tcp_sock_t *sock, void *buf, size_t max_len, bool nonblock);
int         tcp_shutdown(tcp_sock_t *sock, int how);
void        tcp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr);
void        tcp_timer_tick(void);
bool        tcp_poll_in(tcp_sock_t *sock);
bool        tcp_poll_out(tcp_sock_t *sock);
u16         tcp_checksum(const tcp_hdr_t *tcp, const ipv4_hdr_t *ip, const void *payload, size_t payload_len);
