/* ============================================================================
 * AzamiOS Userspace — Netlink Protocol (linux/netlink.h)
 * File: userland/libc/include/linux/netlink.h
 *
 * Matches Linux's uapi/linux/netlink.h layout field-for-field — this is
 * what a real netlink client (this OS's own userland/apps/ip, or an
 * unmodified static Linux binary) sends to and parses from
 * kernel/net/netlink.c's AF_NETLINK support.
 * ============================================================================ */
#pragma once

#include "../stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETLINK_ROUTE 0

struct sockaddr_nl {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int   nl_pid;
    unsigned int   nl_groups;
};

struct nlmsghdr {
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;
    uint32_t nlmsg_pid;
};

#define NLMSG_ALIGNTO 4U
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_HDRLEN     ((int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((len) + NLMSG_HDRLEN)
#define NLMSG_DATA(nlh)  ((void *)((char *)(nlh) + NLMSG_HDRLEN))
#define NLMSG_OK(nlh, len) \
    ((len) >= (int)sizeof(struct nlmsghdr) && \
     (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
     (int)(nlh)->nlmsg_len <= (len))
#define NLMSG_NEXT(nlh, len) \
    ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), \
     (struct nlmsghdr *)((char *)(nlh) + NLMSG_ALIGN((nlh)->nlmsg_len)))

#define NLMSG_NOOP    0x1
#define NLMSG_ERROR   0x2
#define NLMSG_DONE    0x3

#define NLM_F_REQUEST 0x0001
#define NLM_F_MULTI   0x0002
#define NLM_F_ACK     0x0004
#define NLM_F_ROOT    0x0100
#define NLM_F_MATCH   0x0200
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)

struct nlmsgerr {
    int error;
    struct nlmsghdr msg;
};

struct rtattr {
    unsigned short rta_len;
    unsigned short rta_type;
};

#define RTA_ALIGNTO 4U
#define RTA_ALIGN(len) (((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_OK(rta, len) \
    ((len) >= (int)sizeof(struct rtattr) && \
     (rta)->rta_len >= sizeof(struct rtattr) && \
     (int)(rta)->rta_len <= (len))
#define RTA_NEXT(rta, len) \
    ((len) -= RTA_ALIGN((rta)->rta_len), \
     (struct rtattr *)((char *)(rta) + RTA_ALIGN((rta)->rta_len)))
#define RTA_LENGTH(len) (RTA_ALIGN(sizeof(struct rtattr)) + (len))
#define RTA_DATA(rta)   ((void *)((char *)(rta) + RTA_ALIGN(sizeof(struct rtattr))))
#define RTA_PAYLOAD(rta) ((int)(rta)->rta_len - (int)RTA_ALIGN(sizeof(struct rtattr)))

#ifdef __cplusplus
}
#endif
