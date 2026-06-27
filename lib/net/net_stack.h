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

/* ── Public API ────────────────────────────────────────────────────── */
void net_stack_init(void);
void net_receive_packet(const uint8_t *packet, uint32_t len);
void net_send_ping(uint32_t target_ip);
void net_print_arp_cache(void);
void net_print_status(void);

#endif /* NET_STACK_H */
