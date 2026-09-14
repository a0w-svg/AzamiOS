/* ============================================================================
 * AzamiOS — AF_NETLINK / NETLINK_ROUTE Header (netlink.h)
 * File: include/azami/netlink.h
 *
 * Wire-format structs and constants for rtnetlink (RFC 3549), matching
 * Linux's ABI field-for-field: this is what lets a real, unmodified
 * netlink-based tool (a genuine iproute2 `ip`/`ss`, not just this OS's own)
 * query link/address/route state instead of getting confused by a made-up
 * layout. See kernel/net/netlink.c for what's actually implemented — a
 * read-only GETLINK/GETADDR/GETROUTE dump responder, not a general
 * rtnetlink server.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define NETLINK_ROUTE 0

struct sockaddr_nl {
    u16 nl_family;
    u16 nl_pad;
    u32 nl_pid;
    u32 nl_groups;
};

typedef struct __attribute__((packed)) {
    u32 nlmsg_len;
    u16 nlmsg_type;
    u16 nlmsg_flags;
    u32 nlmsg_seq;
    u32 nlmsg_pid;
} nlmsghdr_t;

#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(len) (((u32)(len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN     ((int)NLMSG_ALIGN(sizeof(nlmsghdr_t)))

#define NLMSG_NOOP    0x1
#define NLMSG_ERROR   0x2
#define NLMSG_DONE    0x3

#define NLM_F_REQUEST 0x0001
#define NLM_F_MULTI   0x0002
#define NLM_F_ACK     0x0004
#define NLM_F_ROOT    0x0100
#define NLM_F_MATCH   0x0200
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)

typedef struct __attribute__((packed)) {
    s32       error;
    nlmsghdr_t msg;
} nlmsgerr_t;

typedef struct __attribute__((packed)) {
    u16 rta_len;
    u16 rta_type;
} rtattr_t;

#define RTA_ALIGNTO 4U
#define RTA_ALIGN(len) (((u32)(len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))

/* ── RTM_*GETLINK / RTM_NEWLINK ──────────────────────────────────────────── */
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18

typedef struct __attribute__((packed)) {
    u8  ifi_family;
    u8  __ifi_pad;
    u16 ifi_type;
    s32 ifi_index;
    u32 ifi_flags;
    u32 ifi_change;
} ifinfomsg_t;

#define IFLA_UNSPEC   0
#define IFLA_ADDRESS  1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME   3
#define IFLA_MTU      4

/* ── RTM_*GETADDR / RTM_NEWADDR ──────────────────────────────────────────── */
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_GETADDR 22

typedef struct __attribute__((packed)) {
    u8  ifa_family;
    u8  ifa_prefixlen;
    u8  ifa_flags;
    u8  ifa_scope;
    s32 ifa_index;
} ifaddrmsg_t;

#define IFA_UNSPEC    0
#define IFA_ADDRESS   1
#define IFA_LOCAL     2
#define IFA_LABEL     3
#define IFA_BROADCAST 4

/* ── RTM_*GETROUTE / RTM_NEWROUTE ────────────────────────────────────────── */
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26

typedef struct __attribute__((packed)) {
    u8  rtm_family;
    u8  rtm_dst_len;
    u8  rtm_src_len;
    u8  rtm_tos;
    u8  rtm_table;
    u8  rtm_protocol;
    u8  rtm_scope;
    u8  rtm_type;
    u32 rtm_flags;
} rtmsg_t;

#define RTA_UNSPEC   0
#define RTA_DST      1
#define RTA_OIF      4
#define RTA_GATEWAY  5
#define RTA_PRIORITY 6

#define RT_TABLE_MAIN     254
#define RTPROT_UNSPEC     0
#define RTPROT_KERNEL     2
#define RTPROT_DHCP       16
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_SCOPE_HOST     254
#define RTN_UNICAST       1

/* Public AF_NETLINK API */
typedef struct netlink_sock {
    net_buf_queue_t     rx_queue;
    struct thread       *wait_thread;
    spinlock_t           lock;
    struct netlink_sock *next;
} netlink_sock_t;

netlink_sock_t *netlink_socket_create(void);
void             netlink_socket_close(netlink_sock_t *nl);
/* Parses just enough of the request (nlmsg_type/seq/pid — no attributes) to
 * build and queue its reply, exactly as if the kernel had processed a real
 * rtnetlink request. Returns `len` (the request is always fully consumed)
 * or -1 on allocation failure. */
s64              netlink_socket_send(netlink_sock_t *nl, const void *data, size_t len);
