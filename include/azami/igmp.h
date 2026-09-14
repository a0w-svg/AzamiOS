/* ============================================================================
 * AzamiOS — Internet Group Management Protocol v2 Header (igmp.h)
 * File: include/azami/igmp.h
 *
 * Implements RFC 2236 IGMPv2 host-side group membership: join/leave
 * tracking, unsolicited Membership Reports, and Query responses.  No
 * router-side state (Querier election, group timers) — this stack is
 * always a host, never a multicast router.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "net_buf.h"
#include "ipv4.h"

#define IP_PROTO_IGMP 2

/* IGMP Message Types (RFC 2236 §2) */
#define IGMP_TYPE_QUERY     0x11
#define IGMP_TYPE_V1_REPORT 0x12
#define IGMP_TYPE_V2_REPORT 0x16
#define IGMP_TYPE_V2_LEAVE  0x17

/* How many multicast groups this host can have joined at once — one socket
 * per app that cares (mDNS, a streaming client) times a handful of groups
 * each is generous for anything running here. */
#define IGMP_MAX_GROUPS 16

typedef struct __attribute__((packed)) {
    u8  type;
    u8  max_resp_time;  /* tenths of a second; Query only, ignored elsewhere */
    u16 checksum;
    u8  group[4];        /* 0.0.0.0 on a General Query                       */
} igmp_hdr_t;

void igmp_init(void);
void igmp_input(net_buf_t *buf, const ipv4_hdr_t *ip_hdr);

/* IP_ADD_MEMBERSHIP / IP_DROP_MEMBERSHIP backing (kernel/syscall/syscall.c's
 * sys_setsockopt_impl). Reference-counted: joining a group already joined
 * bumps a refcount instead of re-reporting; leaving drops it and only sends
 * a Leave Group once nothing else still wants the group. Returns 0 on
 * success, -1 if the group table is full (join) or the group wasn't joined
 * (leave). */
int  ip_multicast_join(const u8 group[4]);
int  ip_multicast_leave(const u8 group[4]);
/* Whether *this host* has joined `group` — used by ipv4_input() to decide
 * whether a multicast-destined datagram is actually for us. */
bool ip_multicast_is_member(const u8 group[4]);
