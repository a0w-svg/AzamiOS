/* ============================================================================
 * AzamiOS Userland Libc — POSIX Socket & INET Helpers Implementation (socket.c)
 * File: userland/libc/socket.c
 * ============================================================================ */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int socket(int domain, int type, int protocol)
{
    long ret = syscall3(SYS_socket, domain, type, protocol);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    long ret = syscall3(SYS_bind, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    long ret = syscall3(SYS_connect, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int listen(int sockfd, int backlog)
{
    long ret = syscall2(SYS_listen, sockfd, backlog);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    long ret = syscall3(SYS_accept, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

ssize_t send(int sockfd, const void *buf, size_t len, int flags)
{
    return sendto(sockfd, buf, len, flags, NULL, 0);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
    long ret = syscall6(SYS_sendto, sockfd, (long)buf, len, flags, (long)dest_addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    return recvfrom(sockfd, buf, len, flags, NULL, NULL);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    long ret = syscall6(SYS_recvfrom, sockfd, (long)buf, len, flags, (long)src_addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (ssize_t)ret;
}

ssize_t sendmsg(int sockfd, const struct msghdr *msg, int flags)
{
    if (!msg) { errno = EINVAL; return -1; }
    if (msg->msg_iov && msg->msg_iovlen > 0) {
        return sendto(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                      (const struct sockaddr *)msg->msg_name, msg->msg_namelen);
    }
    return 0;
}

ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
{
    if (!msg) { errno = EINVAL; return -1; }
    if (msg->msg_iov && msg->msg_iovlen > 0) {
        return recvfrom(sockfd, msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, flags,
                        (struct sockaddr *)msg->msg_name, &msg->msg_namelen);
    }
    return 0;
}

int shutdown(int sockfd, int how)
{
    long ret = syscall2(SYS_shutdown, sockfd, how);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    long ret = syscall3(SYS_getsockname, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int getpeername(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    long ret = syscall3(SYS_getpeername, sockfd, (long)addr, (long)addrlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    long ret = syscall5(SYS_setsockopt, sockfd, level, optname, (long)optval, (long)optlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
{
    long ret = syscall5(SYS_getsockopt, sockfd, level, optname, (long)optval, (long)optlen);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

/* ── Byte Order Functions ─────────────────────────────────────────────────── */

uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }
uint32_t htonl(uint32_t v) { return (((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF)); }
uint32_t ntohl(uint32_t v) { return htonl(v); }

/* ── INET String & Conversion Helpers ─────────────────────────────────────── */

in_addr_t inet_addr(const char *cp)
{
    struct in_addr in;
    if (inet_aton(cp, &in) == 0) {
        return INADDR_NONE;
    }
    return in.s_addr;
}

int inet_aton(const char *cp, struct in_addr *inp)
{
    if (!cp || !inp) return 0;
    return inet_pton(AF_INET, cp, inp);
}

char *inet_ntoa(struct in_addr in)
{
    static char buf[INET_ADDRSTRLEN];
    const unsigned char *b = (const unsigned char *)&in.s_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

int inet_pton(int af, const char *src, void *dst)
{
    if (af != AF_INET || !src || !dst) {
        errno = EAFNOSUPPORT;
        return -1;
    }

    unsigned int bytes[4];
    char dummy;
    if (sscanf(src, "%u.%u.%u.%u%c", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &dummy) != 4) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        if (bytes[i] > 255) return 0;
    }

    unsigned char *out = (unsigned char *)dst;
    out[0] = (unsigned char)bytes[0];
    out[1] = (unsigned char)bytes[1];
    out[2] = (unsigned char)bytes[2];
    out[3] = (unsigned char)bytes[3];

    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size)
{
    if (af != AF_INET || !src || !dst || size < INET_ADDRSTRLEN) {
        errno = ENOSPC;
        return NULL;
    }

    const unsigned char *b = (const unsigned char *)src;
    snprintf(dst, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return dst;
}

in_addr_t inet_network(const char *cp)
{
    in_addr_t addr = inet_addr(cp);
    if (addr == INADDR_NONE) return INADDR_NONE;
    return ntohl(addr);
}

struct in_addr inet_makeaddr(in_addr_t net, in_addr_t host)
{
    struct in_addr in;
    if (net < 128) in.s_addr = htonl((net << 24) | (host & 0xFFFFFF));
    else if (net < 65536) in.s_addr = htonl((net << 16) | (host & 0xFFFF));
    else in.s_addr = htonl((net << 8) | (host & 0xFF));
    return in;
}

in_addr_t inet_lnaof(struct in_addr in)
{
    in_addr_t h = ntohl(in.s_addr);
    if ((h >> 24) < 128) return h & 0x00FFFFFF;
    if ((h >> 24) < 192) return h & 0x0000FFFF;
    return h & 0x000000FF;
}

in_addr_t inet_netof(struct in_addr in)
{
    in_addr_t h = ntohl(in.s_addr);
    if ((h >> 24) < 128) return (h >> 24) & 0xFF;
    if ((h >> 24) < 192) return (h >> 16) & 0xFFFF;
    return (h >> 8) & 0xFFFFFF;
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    long ret = syscall4(SYS_socketpair, domain, type, protocol, (long)sv);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

unsigned int if_nametoindex(const char *ifname)
{
    if (!ifname) return 0;
    if (strcmp(ifname, "lo") == 0 || strcmp(ifname, "lo0") == 0) return 1;
    if (strcmp(ifname, "net0") == 0 || strcmp(ifname, "eth0") == 0 || strcmp(ifname, "e1000") == 0) return 2;
    return 0;
}

char *if_indextoname(unsigned int ifindex, char *ifname)
{
    if (!ifname) return NULL;
    if (ifindex == 1) { strcpy(ifname, "lo"); return ifname; }
    if (ifindex == 2) { strcpy(ifname, "net0"); return ifname; }
    return NULL;
}
