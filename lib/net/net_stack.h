/**
 * lib/net/net_stack.h  –  AzamiOS TCP/IP Protocol Stack Header
 *
 * Kernel-independent: only depends on <stdint.h>, <stdbool.h>, <stddef.h>.
 * Canonical copy — kernel/drivers/include/net_stack.h delegates here.
 */
#ifndef NET_STACK_H
#define NET_STACK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ── Endianness utilities ──────────────────────────────────────────── */
#define htons(x) ((uint16_t)((((x) & 0xFF) << 8) | (((x) >> 8) & 0xFF)))
#define ntohs(x) htons(x)
#define htonl(x) ((uint32_t)((((x) & 0xFF) << 24) | (((x) & 0xFF00) << 8) | \
                              (((x) >> 8) & 0xFF00) | (((x) >> 24) & 0xFF)))
#define ntohl(x) htonl(x)

/* ── EtherTypes ────────────────────────────────────────────────────── */
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV4 0x0800

/* ── IP Protocols ──────────────────────────────────────────────────── */
#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* ── TCP Flags ─────────────────────────────────────────────────────── */
#define TCP_FLAG_FIN   0x01
#define TCP_FLAG_SYN   0x02
#define TCP_FLAG_RST   0x04
#define TCP_FLAG_PSH   0x08
#define TCP_FLAG_ACK   0x10

#pragma pack(push, 1)

/* Ethernet II Header */
typedef struct {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
} eth_hdr_t;

/* ARP Header */
typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t opcode;
    uint8_t  src_mac[6];
    uint32_t src_ip;
    uint8_t  dst_mac[6];
    uint32_t dst_ip;
} arp_hdr_t;

/* IPv4 Header */
typedef struct {
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} ipv4_hdr_t;

/* ICMP Header */
typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

/* UDP Header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

/* TCP Header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset_res;
    uint8_t  flags;
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_hdr_t;

#pragma pack(pop)

/* ARP Cache entry */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    bool     valid;
} arp_entry_t;

/* Network configuration */
typedef struct {
    uint32_t ip_addr;
    uint32_t subnet_mask;
    uint32_t gateway_ip;
    uint8_t  mac_addr[6];
} net_config_t;

/* ── TCP States & Socket Abstraction ───────────────────────────────── */
typedef enum {
    TCP_STATE_CLOSED = 0,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RCVD,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT1,
    TCP_STATE_FIN_WAIT2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

typedef void (*tcp_rx_callback_t)(int sock_id, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint32_t len);
typedef void (*udp_rx_callback_t)(int sock_id, uint32_t src_ip, uint16_t src_port, const uint8_t *data, uint32_t len);

typedef struct {
    bool              valid;
    tcp_state_t       state;
    uint16_t          local_port;
    uint32_t          remote_ip;
    uint16_t          remote_port;
    uint32_t          seq_num;
    uint32_t          ack_num;
    tcp_rx_callback_t rx_cb;
} tcp_socket_t;

typedef struct {
    bool              valid;
    uint16_t          local_port;
    udp_rx_callback_t rx_cb;
} udp_socket_t;

/* ── Public API ────────────────────────────────────────────────────── */
void net_stack_init(void);
void net_receive_packet(const uint8_t *packet, uint32_t len);
void net_send_ping(uint32_t target_ip);
void net_print_arp_cache(void);
void net_print_status(void);

/* Protocols & Hardening */
bool net_dns_resolve(const char *domain, uint32_t *out_ip);
bool net_dhcp_request(void);
bool net_ntp_sync(uint32_t *out_epoch);
uint32_t net_tcp_get_isn(void);

/* Socket API */
int  net_tcp_socket_open(tcp_rx_callback_t cb);
bool net_tcp_bind(int sock_id, uint16_t local_port);
bool net_tcp_listen(int sock_id);
bool net_tcp_connect(int sock_id, uint32_t remote_ip, uint16_t remote_port);
int  net_tcp_send(int sock_id, const uint8_t *data, uint32_t len);
void net_tcp_close(int sock_id);

int  net_udp_socket_open(udp_rx_callback_t cb);
bool net_udp_bind(int sock_id, uint16_t local_port);
int  net_udp_sendto(int sock_id, uint32_t dst_ip, uint16_t dst_port, const uint8_t *data, uint32_t len);
void net_udp_close(int sock_id);

#endif /* NET_STACK_H */
