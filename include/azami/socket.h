/* ============================================================================
 * AzamiOS — Kernel BSD Socket Interface Header (socket.h)
 * File: include/azami/socket.h
 *
 * Implements POSIX BSD socket data structures, sockaddr_in, socket options,
 * and VFS file operation integration.
 * ============================================================================ */
#pragma once

#include "types.h"
#include "tcp.h"
#include "udp.h"
#include "../../fs/vfs.h"

static inline u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
static inline u32 ntohl(u32 v) { return htonl(v); }

/* Address Families */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    1
#define AF_INET     2
#define AF_INET6    10

/* Socket Types */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/* Protocol Levels */
#define SOL_SOCKET  1
#define IPPROTO_IP  0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17

/* Socket Options */
#define SO_DEBUG        1
#define SO_REUSEADDR    2
#define SO_TYPE         3
#define SO_ERROR        4
#define SO_DONTROUTE    5
#define SO_BROADCAST    6
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_KEEPALIVE    9
#define SO_OOBINLINE    10
#define SO_NO_CHECK     11
#define SO_PRIORITY     12
#define SO_LINGER       13
#define SO_BSDCOMPAT    14
#define SO_REUSEPORT    15
#define SO_RCVLOWAT     18
#define SO_SNDLOWAT     19
#define SO_RCVTIMEO     20
#define SO_SNDTIMEO     21

/* TCP Socket Options */
#define TCP_NODELAY     1

/* Shutdown Constants */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

typedef u32 socklen_t;
typedef u16 sa_family_t;

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct in_addr {
    u32 s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    u16            sin_port;
    struct in_addr sin_addr;
    u8             sin_zero[8];
};

typedef struct socket {
    int         domain;
    int         type;
    int         protocol;
    union {
        tcp_sock_t *tcp;
        udp_sock_t *udp;
    };
    int         so_reuseaddr;
    u32         so_rcvtimeo;
    u32         so_sndtimeo;
    file_t     *file;
} socket_t;

/* Public Socket API */
socket_t *sock_alloc(int domain, int type, int protocol);
file_t   *sock_create_file(socket_t *sock);
void      sock_free(socket_t *sock);
int       sock_get_from_fd(int fd, socket_t **sock_out);
