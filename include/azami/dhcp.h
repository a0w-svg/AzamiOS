/* ============================================================================
 * AzamiOS — Dynamic Host Configuration Protocol Header (dhcp.h)
 * File: include/azami/dhcp.h
 *
 * Implements RFC 2131 / RFC 2132 DHCP protocol state machine, options parsing,
 * packet formatting, and lease management.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "ipv4.h"

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_BOOTREQUEST 1
#define DHCP_BOOTREPLY   2
#define DHCP_HTYPE_ETH   1
#define DHCP_MAGIC_COOKIE 0x63825363

/* DHCP Options */
#define DHCP_OPT_PAD            0
#define DHCP_OPT_SUBNET_MASK    1
#define DHCP_OPT_ROUTER         3
#define DHCP_OPT_DNS            6
#define DHCP_OPT_HOSTNAME       12
#define DHCP_OPT_DOMAIN_NAME    15
#define DHCP_OPT_REQUESTED_IP   50
#define DHCP_OPT_LEASE_TIME     51
#define DHCP_OPT_MSG_TYPE       53
#define DHCP_OPT_SERVER_ID      54
#define DHCP_OPT_PARAM_REQ_LIST 55
#define DHCP_OPT_RENEWAL_TIME   58
#define DHCP_OPT_REBINDING_TIME 59
#define DHCP_OPT_CLIENT_ID      61
#define DHCP_OPT_END            255

/* DHCP Message Types (Option 53) */
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

typedef struct __attribute__((packed)) {
    u8  op;
    u8  htype;
    u8  hlen;
    u8  hops;
    u32 xid;
    u16 secs;
    u16 flags;
    u8  ciaddr[4];
    u8  yiaddr[4];
    u8  siaddr[4];
    u8  giaddr[4];
    u8  chaddr[16];
    u8  sname[64];
    u8  file[128];
    u32 magic_cookie;
    u8  options[308];
} dhcp_packet_t;

typedef enum {
    DHCP_STATE_INIT       = 0,
    DHCP_STATE_SELECTING  = 1,
    DHCP_STATE_REQUESTING = 2,
    DHCP_STATE_BOUND      = 3,
    DHCP_STATE_RENEWING   = 4,
    DHCP_STATE_REBINDING  = 5,
} dhcp_state_t;

typedef struct {
    u8           offered_ip[4];
    u8           netmask[4];
    u8           gateway[4];
    u8           dns[4];
    u8           server_id[4];
    u32          lease_time;
    u32          renewal_time;
    u32          rebinding_time;
    u32          xid;
    dhcp_state_t state;
} dhcp_lease_t;

/* Public Kernel DHCP API */
void dhcp_init(void);
int  dhcp_start_discovery(void);
void dhcp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr);
void dhcp_get_lease(dhcp_lease_t *out_lease);
