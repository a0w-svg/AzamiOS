/* ============================================================================
 * AzamiOS — Kernel Network Stack Header (net.h)
 * File: include/azami/net.h
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "arp.h"

#define ETH_ALEN 6
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806

typedef struct __attribute__((packed)) {
    u8  dst[ETH_ALEN];
    u8  src[ETH_ALEN];
    u16 ethertype;
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    u16 htype;          /* Hardware type (1 for Ethernet) */
    u16 ptype;          /* Protocol type (0x0800 for IPv4) */
    u8  hlen;           /* Hardware address length (6) */
    u8  plen;           /* Protocol address length (4) */
    u16 oper;           /* Operation (1: request, 2: reply) */
    u8  sha[ETH_ALEN];  /* Sender hardware address */
    u8  spa[4];         /* Sender protocol address (IP) */
    u8  tha[ETH_ALEN];  /* Target hardware address */
    u8  tpa[4];         /* Target protocol address (IP) */
} arp_pkt_t;

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

typedef struct __attribute__((packed)) {
    u8  ihl_version;    /* Version (4 bits) + IHL (4 bits) */
    u8  tos;            /* Type of service */
    u16 total_len;      /* Total length (header + data) */
    u16 id;             /* Identification */
    u16 frag_offset;    /* Flags (3 bits) + Fragment offset (13 bits) */
    u8  ttl;            /* Time to live */
    u8  protocol;       /* Protocol (1: ICMP, 6: TCP, 17: UDP) */
    u16 checksum;       /* Header checksum */
    u8  src_ip[4];      /* Source IP */
    u8  dst_ip[4];      /* Destination IP */
} ipv4_hdr_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct __attribute__((packed)) {
    u8  type;           /* Message type (0: Echo Reply, 8: Echo Request) */
    u8  code;           /* Type sub-code */
    u16 checksum;       /* ICMP Checksum */
    u16 id;             /* Identifier */
    u16 seq;            /* Sequence number */
} icmp_hdr_t;

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

/* Network Interface Device Descriptor */
typedef struct net_device {
    char name[16];
    u8   mac[6];
    s64  (*send)(const void *data, size_t len);
    s64  (*recv)(void *buf, size_t max_len);
    bool (*link_up)(void);
} net_device_t;

/* Network IOCTL Codes */
#define SIOCGIFHWADDR   0x8910
#define SIOCGIFFLAGS    0x8913
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFGW       0x891d
#define SIOCSIFGW       0x891e
#define SIOCGIFDNS      0x891f
#define SIOCSIFDNS      0x8921
#define SIOCGIFNAME     0x8920
#define SIOCGIFPING     0x8930

/* Public Network API */
void net_init(void);
int  net_register_device(const net_device_t *dev);
net_device_t *net_get_default_device(void);
void net_process_incoming(const u8 *pkt, size_t len);
s64  net_send_icmp_ping(const u8 target_ip[4], u16 seq);
void net_get_ip(u8 ip_out[4]);
void net_set_ip(const u8 ip_in[4]);
void net_get_netmask(u8 mask_out[4]);
void net_set_netmask(const u8 mask_in[4]);
void net_get_gateway(u8 gw_out[4]);
void net_set_gateway(const u8 gw_in[4]);
void net_get_dns(u8 dns_out[4]);
void net_set_dns(const u8 dns_in[4]);
int  net_ioctl(u32 cmd, u64 arg);
u16  net_checksum(const void *data, size_t len);
s64  net_send_raw(const void *data, size_t len);
