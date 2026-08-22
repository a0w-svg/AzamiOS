/* ============================================================================
 * AzamiOS Userland Libc — POSIX Socket Header (<sys/socket.h>)
 * File: userland/libc/include/sys/socket.h
 * ============================================================================ */
#pragma once

#include <sys/types.h>

typedef unsigned int socklen_t;
typedef unsigned short sa_family_t;

/* Address Families */
#define AF_UNSPEC   0
#define AF_UNIX     1
#define AF_LOCAL    1
#define AF_INET     2
#define AF_INET6    10

/* Socket Types */
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define SOCK_RAW        3
#define SOCK_NONBLOCK   00004000
#define SOCK_CLOEXEC    02000000

/* Protocol Levels */
#define SOL_SOCKET  1

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

/* Shutdown Constants */
#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* Generic Socket Address */
struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_storage {
    sa_family_t ss_family;
    char        __ss_pad1[6];
    long long   __ss_align;
    char        __ss_pad2[112];
};

/* Socket Syscall Wrappers */
int     socket(int domain, int type, int protocol);
int     bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int     listen(int sockfd, int backlog);
int     accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
int     shutdown(int sockfd, int how);
int     getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int     getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
int     socketpair(int domain, int type, int protocol, int sv[2]);
int     setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
int     getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
