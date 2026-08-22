/* ============================================================================
 * AzamiOS — IPv4 Protocol & Routing Table Header (ipv4.h)
 * File: include/azami/ipv4.h
 *
 * Implements RFC 791 Internet Protocol Version 4, IP checksum computation,
 * subnet routing table, longest prefix matching, and packet encapsulation.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define MAX_ROUTES 32

/* Routing table flags */
#define RT_FLAG_UP      0x01
#define RT_FLAG_GATEWAY 0x02
#define RT_FLAG_HOST    0x04

typedef struct {
    u8                 dst[4];     /* Destination network IP             */
    u8                 mask[4];    /* Subnet mask                        */
    u8                 gateway[4]; /* Gateway IP (0.0.0.0 if direct)     */
    struct net_device *dev;        /* Network device interface           */
    u32                flags;      /* RT_FLAG_*                          */
    u32                metric;     /* Route metric priority              */
} route_entry_t;

/* Public IPv4 & Routing API */
void route_init(void);
int  route_add(const u8 dst[4], const u8 mask[4], const u8 gw[4], struct net_device *dev, u32 flags, u32 metric);
int  route_del(const u8 dst[4], const u8 mask[4]);
int  route_lookup(const u8 dst_ip[4], u8 next_hop_out[4], struct net_device **dev_out);
void route_print_table(void);

void ipv4_init(void);
int  ipv4_send(net_buf_t *buf, const u8 dst_ip[4], u8 protocol);
void ipv4_input(net_buf_t *buf);
