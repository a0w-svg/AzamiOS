/* ============================================================================
 * AzamiOS Userland Libc — Internet Protocol Header (<netinet/in.h>)
 * File: userland/libc/include/netinet/in.h
 * ============================================================================ */
#pragma once

#include <sys/types.h>
#include <sys/socket.h>

typedef unsigned short in_port_t;
typedef unsigned int   in_addr_t;

/* Internet Protocols */
#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17
#define IPPROTO_RAW  255

/* Standard IPv4 Addresses */
#define INADDR_ANY       ((in_addr_t)0x00000000)
#define INADDR_LOOPBACK  ((in_addr_t)0x7f000001)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_NONE      ((in_addr_t)0xffffffff)

#define INET_ADDRSTRLEN  16

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};
