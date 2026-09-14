/* ============================================================================
 * AzamiOS Userspace — Routing Netlink (linux/rtnetlink.h)
 * File: userland/libc/include/linux/rtnetlink.h
 *
 * RTM_GETLINK/GETADDR/GETROUTE and their ifinfomsg/ifaddrmsg/rtmsg
 * payloads, matching Linux's uapi layout — kernel/net/netlink.c only
 * implements the read-only GETLINK/GETADDR/GETROUTE dump side of this.
 * ============================================================================ */
#pragma once

#include "netlink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Link (RTM_*LINK) ─────────────────────────────────────────────────── */
#define RTM_NEWLINK 16
#define RTM_DELLINK 17
#define RTM_GETLINK 18

struct ifinfomsg {
    unsigned char  ifi_family;
    unsigned char  __ifi_pad;
    unsigned short ifi_type;
    int            ifi_index;
    unsigned int   ifi_flags;
    unsigned int   ifi_change;
};

enum {
    IFLA_UNSPEC,
    IFLA_ADDRESS,
    IFLA_BROADCAST,
    IFLA_IFNAME,
    IFLA_MTU,
};

/* ── Address (RTM_*ADDR) ──────────────────────────────────────────────── */
#define RTM_NEWADDR 20
#define RTM_DELADDR 21
#define RTM_GETADDR 22

struct ifaddrmsg {
    unsigned char ifa_family;
    unsigned char ifa_prefixlen;
    unsigned char ifa_flags;
    unsigned char ifa_scope;
    int           ifa_index;
};

enum {
    IFA_UNSPEC,
    IFA_ADDRESS,
    IFA_LOCAL,
    IFA_LABEL,
    IFA_BROADCAST,
};

/* ── Route (RTM_*ROUTE) ───────────────────────────────────────────────── */
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26

struct rtmsg {
    unsigned char rtm_family;
    unsigned char rtm_dst_len;
    unsigned char rtm_src_len;
    unsigned char rtm_tos;
    unsigned char rtm_table;
    unsigned char rtm_protocol;
    unsigned char rtm_scope;
    unsigned char rtm_type;
    unsigned int  rtm_flags;
};

enum {
    RTA_UNSPEC,
    RTA_DST,
    RTA_SRC,
    RTA_IIF,
    RTA_OIF,
    RTA_GATEWAY,
    RTA_PRIORITY,
};

#define RT_TABLE_MAIN     254
#define RTPROT_UNSPEC     0
#define RTPROT_KERNEL     2
#define RTPROT_DHCP       16
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253
#define RT_SCOPE_HOST     254
#define RTN_UNICAST       1

#ifdef __cplusplus
}
#endif
