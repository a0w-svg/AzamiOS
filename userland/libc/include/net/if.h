/* ============================================================================
 * AzamiOS Userspace — Network Interfaces (net/if.h)
 * File: userland/libc/include/net/if.h
 * ============================================================================ */
#pragma once

#include "../sys/types.h"
#include "../sys/socket.h"

#define IFNAMSIZ 16

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifru_addr;
        struct sockaddr ifru_dstaddr;
        struct sockaddr ifru_broadaddr;
        struct sockaddr ifru_netmask;
        struct sockaddr ifru_hwaddr;
        short           ifru_flags;
        int             ifru_ivalue;
        int             ifru_mtu;
        char            ifru_data[24];
    } ifr_ifru;
};

#define ifr_addr      ifr_ifru.ifru_addr
#define ifr_dstaddr   ifr_ifru.ifru_dstaddr
#define ifr_broadaddr ifr_ifru.ifru_broadaddr
#define ifr_netmask   ifr_ifru.ifru_netmask
#define ifr_hwaddr    ifr_ifru.ifru_hwaddr
#define ifr_flags     ifr_ifru.ifru_flags
#define ifr_metric    ifr_ifru.ifru_ivalue
#define ifr_mtu       ifr_ifru.ifru_mtu

/* Interface flags */
#define IFF_UP          0x1     /* Interface is running */
#define IFF_BROADCAST   0x2     /* Valid broadcast address set */
#define IFF_DEBUG       0x4     /* Internal debugging flag */
#define IFF_LOOPBACK    0x8     /* Interface is a loopback */
#define IFF_POINTOPOINT 0x10    /* Point-to-point link */
#define IFF_RUNNING     0x40    /* Resources allocated */
#define IFF_NOARP       0x80    /* No ARP protocol */
#define IFF_PROMISC     0x100   /* Promiscuous mode */
#define IFF_MULTICAST   0x1000  /* Multicast supported */

struct ifconf {
    int ifc_len;
    union {
        char         *ifcu_buf;
        struct ifreq *ifcu_req;
    } ifc_ifcu;
};

#define ifc_buf ifc_ifcu.ifcu_buf
#define ifc_req ifc_ifcu.ifcu_req

unsigned int if_nametoindex(const char *ifname);
char        *if_indextoname(unsigned int ifindex, char *ifname);
