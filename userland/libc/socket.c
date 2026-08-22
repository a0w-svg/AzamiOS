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

/* ── INET String & Conversion Helpers ─────────────────────────────────────── */

in_addr_t inet_addr(const char *cp)
{
    struct in_addr in;
    if (inet_pton(AF_INET, cp, &in) <= 0) {
        return INADDR_NONE;
    }
    return in.s_addr;
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

int socketpair(int domain, int type, int protocol, int sv[2])
{
    long ret = syscall4(SYS_socketpair, domain, type, protocol, (long)sv);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}
