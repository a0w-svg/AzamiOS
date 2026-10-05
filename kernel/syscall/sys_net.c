/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/* ============================================================================
 * AzamiOS — Networking and Socket Syscalls
 * File: kernel/syscall/sys_net.c
 * ============================================================================ */
#include "syscall_internal.h"


/* ── Sockets & Networking Syscalls ───────────────────────────────────────── */

s64 sys_socket_impl(pt_regs_t *r)
{
    int domain = (int)r->rdi;
    int type = (int)r->rsi;
    int protocol = (int)r->rdx;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    /* Strip non-standard flags like SOCK_CLOEXEC or SOCK_NONBLOCK */
    int base_type = type & 0x0F;

    /* Privilege check for RAW sockets. AF_PACKET is gated regardless of
     * `type` — packet_socket_create() (kernel/net/packet.c) doesn't offer a
     * narrower, unprivileged variant the way AF_INET's SOCK_DGRAM/SOCK_STREAM
     * do, so requesting it with SOCK_DGRAM instead of SOCK_RAW must not be a
     * way to dodge this check. AF_NETLINK is explicitly excluded even though
     * it also uses SOCK_RAW: real netlink route queries are unprivileged on
     * Linux (an ordinary `ip addr show` doesn't need root), and
     * netlink_socket_send() (kernel/net/netlink.c) only ever answers
     * read-only GETLINK/GETADDR/GETROUTE dumps — nothing it does needs
     * CAP_NET_RAW to justify. */
    if ((base_type == SOCK_RAW && domain != AF_NETLINK) || domain == AF_PACKET) {
        if (!security_check_permission(proc, CAP_NET_RAW)) {
            return -(s64)EPERM;
        }
    }

    socket_t *sock = sock_alloc(domain, base_type, protocol);
    if (!sock) return -(s64)ENOMEM;

    file_t *f = sock_create_file(sock);
    if (!f) {
        sock_free(sock);
        return -(s64)ENOMEM;
    }

    if (type & 00004000) { /* O_NONBLOCK / SOCK_NONBLOCK */
        f->f_flags |= O_NONBLOCK;
    }

    {
        s64 nfd = fd_install(proc, f, (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0);
        if (nfd >= 0) return nfd;
    }

    sock_free(sock);
    kfree(f);
    return -(s64)EMFILE;
}

/* struct sockaddr_in and struct sockaddr_un share nothing but sa_family at
 * offset 0 — a bind()/connect() on an AF_UNIX socket carries a path in
 * sun_path, not the fixed 16-byte sockaddr_in this file otherwise assumes
 * everywhere. Peeks the family, then copies out just the path (NUL-safe:
 * sun_path isn't guaranteed to be NUL-terminated by the caller, so this
 * always terminates the result itself from addrlen). Returns 0 with
 * *out_path set for AF_UNIX, or a negative errno; leaves *out_path
 * untouched (caller should fall back to sockaddr_in handling) when the
 * address isn't AF_UNIX at all — that isn't an error at this layer. */
static s64 copy_user_sockaddr_un_path(const struct sockaddr *uaddr, socklen_t addrlen,
                                       char out_path[UNIX_PATH_MAX], bool *out_is_unix)
{
    *out_is_unix = false;
    sa_family_t fam;
    if (copy_from_user(&fam, uaddr, sizeof(fam)) != 0) return -(s64)EFAULT;
    if (fam != AF_UNIX) return 0;

    *out_is_unix = true;
    if (addrlen < sizeof(sa_family_t) + 1) return -(s64)EINVAL; /* need at least a 1-char path */
    size_t path_len = addrlen - sizeof(sa_family_t);
    if (path_len >= UNIX_PATH_MAX) path_len = UNIX_PATH_MAX - 1;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    size_t copy_len = sizeof(sa_family_t) + path_len;
    if (copy_len > sizeof(sun)) copy_len = sizeof(sun);
    if (copy_from_user(&sun, uaddr, copy_len) != 0) return -(s64)EFAULT;
    sun.sun_path[UNIX_PATH_MAX - 1] = '\0';
    if (!sun.sun_path[0]) return -(s64)EINVAL; /* abstract-namespace sockets not supported */

    strncpy(out_path, sun.sun_path, UNIX_PATH_MAX - 1);
    out_path[UNIX_PATH_MAX - 1] = '\0';
    return 0;
}

s64 sys_bind_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->rsi;
    socklen_t addrlen = (socklen_t)r->rdx;

    if (!uaddr || addrlen < sizeof(sa_family_t)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        char path[UNIX_PATH_MAX];
        bool is_unix = false;
        s64 err = copy_user_sockaddr_un_path(uaddr, addrlen, path, &is_unix);
        if (err < 0) return err;
        if (!is_unix) return -(s64)EINVAL;
        return unix_socket_bind(sock->uds, path);
    }

    /* AF_NETLINK: struct sockaddr_nl (12 bytes) is smaller than
     * sockaddr_in, so it must be accepted before the generic size check
     * below rejects it outright. Binding is a no-op — this stack's
     * netlink_sock_t (kernel/net/netlink.c) doesn't track a bound pid or
     * multicast groups; every socket already gets exactly the reply its own
     * request asked for, which is all real callers (nl_pid left 0 for the
     * kernel to autoassign) actually need. */
    if (sock->domain == AF_NETLINK) {
        return 0;
    }

    if (addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, uaddr, sizeof(struct sockaddr_in)) != 0) {
        return -(s64)EFAULT;
    }

    u16 port = ntohs(sin.sin_port);
    const u8 *ip = (const u8 *)&sin.sin_addr.s_addr;

    /* Linux privileged port protection: ports < 1024 require CAP_NET_BIND_SERVICE or root */
    process_t *proc = sched_current_process();
    if (port > 0 && port < 1024) {
        if (proc && proc->euid != 0 && !security_check_permission(proc, CAP_NET_BIND_SERVICE)) {
            return -(s64)EACCES;
        }
    }

    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_bind(sock->tcp, ip, port);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return (s64)udp_bind(sock->udp, ip, port);
    }

    return -(s64)EOPNOTSUPP;
}

s64 sys_connect_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->rsi;
    socklen_t addrlen = (socklen_t)r->rdx;

    if (!uaddr || addrlen < sizeof(sa_family_t)) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

    if (sock->domain == AF_UNIX) {
        char path[UNIX_PATH_MAX];
        bool is_unix = false;
        s64 err = copy_user_sockaddr_un_path(uaddr, addrlen, path, &is_unix);
        if (err < 0) return err;
        if (!is_unix) return -(s64)EINVAL;
        return unix_socket_connect(sock->uds, path, nonblock);
    }

    if (addrlen < sizeof(struct sockaddr_in)) return -(s64)EINVAL;
    struct sockaddr_in sin;
    if (copy_from_user(&sin, uaddr, sizeof(struct sockaddr_in)) != 0) {
        return -(s64)EFAULT;
    }

    u16 port = ntohs(sin.sin_port);
    const u8 *ip = (const u8 *)&sin.sin_addr.s_addr;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_connect(sock->tcp, ip, port, nonblock);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return (s64)udp_connect(sock->udp, ip, port);
    }

    return -(s64)EOPNOTSUPP;
}

s64 sys_listen_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int backlog = (int)r->rsi;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        return unix_socket_listen(sock->uds, backlog);
    }
    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_listen(sock->tcp, backlog);
    }

    return -(s64)EOPNOTSUPP;
}

/* accept4(2) flags. Same bit values as the type flags socket(2) takes, which
 * is what lets a caller pass SOCK_NONBLOCK|SOCK_CLOEXEC through unchanged. */
#define SOCK_NONBLOCK_FLAG  00004000
#define SOCK_CLOEXEC_FLAG   02000000

/* accept(2) is accept4(2) with no flags; sharing one body is what stops the
 * two from drifting apart in the details that matter (blocking behaviour,
 * address copy-out, the fd's close-on-exec state). */
static s64 do_accept(pt_regs_t *r, int a4flags)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (a4flags & ~(SOCK_NONBLOCK_FLAG | SOCK_CLOEXEC_FLAG)) return -(s64)EINVAL;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    socket_t *listener = NULL;
    int ret = sock_get_from_fd(fd, &listener);
    if (ret < 0) return -(s64)ret;

    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = f ? ((f->f_flags & O_NONBLOCK) != 0) : false;

    if (listener->domain == AF_UNIX) {
        unix_sock_t *child_uds = unix_socket_create(SOCK_STREAM);
        if (!child_uds) return -(s64)ENOMEM;

        int aerr = unix_socket_accept(listener->uds, child_uds, nonblock);
        if (aerr < 0) {
            kfree(child_uds);
            return (s64)aerr;
        }

        socket_t *child_sock = (socket_t *)kzalloc(sizeof(socket_t));
        if (!child_sock) {
            unix_socket_close(child_uds);
            return -(s64)ENOMEM;
        }
        child_sock->domain = AF_UNIX;
        child_sock->type = SOCK_STREAM;
        child_sock->uds = child_uds;

        file_t *child_file = sock_create_file(child_sock);
        if (!child_file) {
            sock_free(child_sock);
            return -(s64)ENOMEM;
        }
        if (a4flags & SOCK_NONBLOCK_FLAG) child_file->f_flags |= O_NONBLOCK;

        int new_fd = (int)fd_install(proc, child_file,
                                     (a4flags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0);
        if (new_fd < 0) {
            sock_free(child_sock);
            kfree(child_file);
            return -(s64)EMFILE;
        }
        /* No meaningful peer address to fill in beyond AF_UNIX + this
         * listener's own bound path — real Linux reports the *connecting*
         * side's bind() path here (empty for an unbound/autobind client,
         * which is the overwhelmingly common case), and this
         * implementation doesn't track that on the accepted side at all.
         * Leaving *uaddrlen at whatever the caller passed in (unchanged) is
         * closer to "no information available" than fabricating a sockaddr. */
        return (s64)new_fd;
    }

    if (listener->type != SOCK_STREAM || !listener->tcp) {
        return -(s64)EOPNOTSUPP;
    }

    u8 client_ip[4];
    u16 client_port = 0;
    tcp_sock_t *child_tcp = tcp_accept(listener->tcp, client_ip, &client_port, nonblock);
    if (!child_tcp) {
        if (proc && (proc->sig_pending & ~proc->sig_blocked)) return -(s64)EINTR;
        return nonblock ? -(s64)EAGAIN : -(s64)EINVAL;
    }

    socket_t *child_sock = (socket_t *)kzalloc(sizeof(socket_t));
    if (!child_sock) {
        tcp_socket_close(child_tcp);
        return -(s64)ENOMEM;
    }

    child_sock->domain = listener->domain;
    child_sock->type = SOCK_STREAM;
    child_sock->protocol = IPPROTO_TCP;
    child_sock->tcp = child_tcp;

    file_t *child_file = sock_create_file(child_sock);
    if (!child_file) {
        sock_free(child_sock);
        return -(s64)ENOMEM;
    }

    if (a4flags & SOCK_NONBLOCK_FLAG) child_file->f_flags |= O_NONBLOCK;

    int new_fd = (int)fd_install(proc, child_file,
                                 (a4flags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0);
    if (new_fd < 0) {
        sock_free(child_sock);
        kfree(child_file);
        return -(s64)EMFILE;
    }

    /* Fill caller address if requested */
    if (uaddr && uaddrlen) {
        struct sockaddr_in sin;
        memset(&sin, 0, sizeof(sin));
        sin.sin_family = AF_INET;
        sin.sin_port = htons(client_port);
        memcpy(&sin.sin_addr.s_addr, client_ip, 4);

        copy_to_user(uaddr, &sin, sizeof(sin));
        socklen_t slen = sizeof(sin);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
    }

    return (s64)new_fd;
}

s64 sys_accept_impl(pt_regs_t *r)
{
    return do_accept(r, 0);
}

s64 sys_accept4_impl(pt_regs_t *r)
{
    return do_accept(r, (int)r->r10);
}

s64 sys_shutdown_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int how = (int)r->rsi;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    /* AF_UNIX must be checked before sock->type: `sock->tcp` and `sock->uds`
     * are the same union storage, so `sock->type == SOCK_STREAM && sock->tcp`
     * is true for an AF_UNIX stream socket too — tcp_shutdown() would then
     * run against a unix_sock_t reinterpreted as a tcp_sock_t. Every
     * function below in this file that dispatches on sock->type alone has
     * the same hazard; this one and getsockname/getpeername right after it
     * used to have it — caught by shutdown()/send()/recv() on a real
     * AF_UNIX socket landing in tcp_send() and getting a nonsense
     * -ENOTCONN back instead of actually writing. */
    if (sock->domain == AF_UNIX) {
        return 0; /* no half-close modeled for the pipe-backed byte stream */
    }
    if (sock->type == SOCK_STREAM && sock->tcp) {
        return (s64)tcp_shutdown(sock->tcp, how);
    }
    return 0;
}

s64 sys_getsockname_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (!uaddr || !uaddrlen) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= TASK_SIZE_MAX || (uintptr_t)uaddrlen >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, sock->uds->path, UNIX_PATH_MAX - 1);
        copy_to_user(uaddr, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
        return 0;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        sin.sin_port = htons(sock->tcp->local_port);
        memcpy(&sin.sin_addr.s_addr, sock->tcp->local_ip, 4);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        sin.sin_port = htons(sock->udp->local_port);
        memcpy(&sin.sin_addr.s_addr, sock->udp->local_ip, 4);
    }

    copy_to_user(uaddr, &sin, sizeof(sin));
    socklen_t slen = sizeof(sin);
    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));

    return 0;
}

s64 sys_getpeername_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct sockaddr *uaddr = (struct sockaddr *)r->rsi;
    socklen_t *uaddrlen = (socklen_t *)r->rdx;

    if (!uaddr || !uaddrlen) return -(s64)EINVAL;
    if ((uintptr_t)uaddr >= TASK_SIZE_MAX || (uintptr_t)uaddrlen >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (sock->domain == AF_UNIX) {
        /* This implementation doesn't track the *peer's* bound path on the
         * accepted/connected side (see do_accept()'s AF_UNIX branch comment
         * in this file) — report AF_UNIX with an empty path, same as real
         * Linux does for an unbound/autobind peer, rather than fabricating
         * one. Still validates the socket is actually connected. */
        if (sock->uds->state != UNIX_ST_CONNECTED) return -(s64)ENOTCONN;
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        copy_to_user(uaddr, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
        return 0;
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        if (sock->tcp->state != TCP_STATE_ESTABLISHED && sock->tcp->state != TCP_STATE_CLOSE_WAIT) {
            return -(s64)ENOTCONN;
        }
        sin.sin_port = htons(sock->tcp->remote_port);
        memcpy(&sin.sin_addr.s_addr, sock->tcp->remote_ip, 4);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (!sock->udp->connected) return -(s64)ENOTCONN;
        sin.sin_port = htons(sock->udp->remote_port);
        memcpy(&sin.sin_addr.s_addr, sock->udp->remote_ip, 4);
    }

    copy_to_user(uaddr, &sin, sizeof(sin));
    socklen_t slen = sizeof(sin);
    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));

    return 0;
}

s64 sys_setsockopt_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int level = (int)r->rsi;
    int optname = (int)r->rdx;
    const void *optval = (const void *)r->r10;
    socklen_t optlen = (socklen_t)r->r8;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (!optval || (uintptr_t)optval >= TASK_SIZE_MAX) return -(s64)EFAULT;

    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_reuseaddr = val;
            return 0;
        } else if (optname == SO_REUSEPORT && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_reuseport = val;
            return 0;
        } else if (optname == SO_BROADCAST && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->so_broadcast = val;
            return 0;
        } else if (optname == SO_RCVTIMEO) {
            if (optlen >= sizeof(struct linux_timeval)) {
                struct linux_timeval tv;
                copy_from_user(&tv, optval, sizeof(tv));
                sock->so_rcvtimeo = (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
            } else if (optlen >= sizeof(int)) {
                int ms = 0;
                copy_from_user(&ms, optval, sizeof(int));
                sock->so_rcvtimeo = (u32)ms;
            }
            return 0;
        } else if (optname == SO_SNDTIMEO) {
            if (optlen >= sizeof(struct linux_timeval)) {
                struct linux_timeval tv;
                copy_from_user(&tv, optval, sizeof(tv));
                sock->so_sndtimeo = (u32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
            } else if (optlen >= sizeof(int)) {
                int ms = 0;
                copy_from_user(&ms, optval, sizeof(int));
                sock->so_sndtimeo = (u32)ms;
            }
            return 0;
        } else if (optname == SO_KEEPALIVE || optname == SO_SNDBUF || optname == SO_RCVBUF) {
            return 0;
        }
    } else if (level == IPPROTO_TCP) {
        if (optname == TCP_NODELAY) {
            return 0;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == IP_TOS || optname == IP_TTL) {
            return 0;
        } else if (optname == IP_HDRINCL && optlen >= sizeof(int)) {
            int val = 0;
            copy_from_user(&val, optval, sizeof(int));
            sock->ip_hdrincl = val;
            return 0;
        } else if ((optname == IP_ADD_MEMBERSHIP || optname == IP_DROP_MEMBERSHIP) &&
                   optlen >= sizeof(struct ip_mreq)) {
            struct ip_mreq mreq;
            if (copy_from_user(&mreq, optval, sizeof(mreq)) != 0) return -(s64)EFAULT;
            const u8 *group = (const u8 *)&mreq.imr_multiaddr.s_addr;
            int rc = (optname == IP_ADD_MEMBERSHIP) ? ip_multicast_join(group)
                                                      : ip_multicast_leave(group);
            return rc == 0 ? 0 : -(s64)EADDRNOTAVAIL;
        }
    }
    return 0;
}

s64 sys_getsockopt_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    int level = (int)r->rsi;
    int optname = (int)r->rdx;
    void *optval = (void *)r->r10;
    socklen_t *optlen = (socklen_t *)r->r8;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    if (!optval || !optlen) return -(s64)EINVAL;
    if ((uintptr_t)optval >= TASK_SIZE_MAX || (uintptr_t)optlen >= TASK_SIZE_MAX) return -(s64)EFAULT;

    if (level == SOL_SOCKET) {
        if (optname == SO_REUSEADDR) {
            int val = sock->so_reuseaddr;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_REUSEPORT) {
            int val = sock->so_reuseport;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_BROADCAST) {
            int val = sock->so_broadcast;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_TYPE) {
            int val = sock->type;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_ERROR) {
            int val = sock->so_error;
            sock->so_error = 0;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        } else if (optname == SO_RCVBUF || optname == SO_SNDBUF) {
            int val = 65536;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        }
    } else if (level == IPPROTO_IP) {
        if (optname == IP_HDRINCL) {
            int val = sock->ip_hdrincl;
            copy_to_user(optval, &val, sizeof(int));
            socklen_t l = sizeof(int);
            copy_to_user(optlen, &l, sizeof(socklen_t));
            return 0;
        }
    }
    return 0;
}

/* Upper bound on a single socket transfer's kernel bounce buffer. Matches the
 * 64 KB chunking read()/write() already use, and comfortably exceeds the
 * 65507-byte maximum UDP payload. */
#define SOCK_XFER_MAX  65536u

s64 sys_sendto_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const void *ubuf = (const void *)r->rsi;
    size_t len = (size_t)r->rdx;
    int flags = (int)r->r10;
    const struct sockaddr *uaddr = (const struct sockaddr *)r->r8;
    socklen_t uaddrlen = (socklen_t)r->r9;

    if (len && (!ubuf || (uintptr_t)ubuf >= TASK_SIZE_MAX)) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;
    /* UDP zero-length calls send/consume a datagram, not a stream no-op. */
    if (!len && !(sock->domain == AF_INET && sock->type == SOCK_DGRAM)) return 0;

    /* Clamp the bounce buffer the way read()/write() do. `len` is raw user
     * input and went straight to kmalloc(), which serves anything up to 1 GB:
     * an unprivileged process could pin arbitrary kernel memory per call.
     * A stream socket may legally send fewer bytes than asked, so clamping is
     * the correct short-send; a datagram larger than the buffer cannot be
     * truncated silently, so it gets EMSGSIZE. */
    if (len > SOCK_XFER_MAX) {
        if (sock->type == SOCK_DGRAM) return -(s64)EMSGSIZE;
        len = SOCK_XFER_MAX;
    }

    void *kbuf = kmalloc(len ? len : 1);
    if (!kbuf) return -(s64)ENOMEM;
    if (len && copy_from_user(kbuf, ubuf, len) != 0) {
        kfree(kbuf);
        return -(s64)EFAULT;
    }

    s64 res = -(s64)EOPNOTSUPP;

    /* AF_UNIX must be checked before sock->type: sock->tcp and sock->uds
     * are the same union storage, so `sock->type == SOCK_STREAM && sock->tcp`
     * below is true for an AF_UNIX stream socket too — this used to send an
     * AF_UNIX socket's data through tcp_send() reinterpreting its
     * unix_sock_t as a tcp_sock_t, which is how send()/sendto() on a real
     * connected AF_UNIX socket ended up failing with a nonsense -ENOTCONN
     * (tcp_send() read what it thought was TCP connection state out of
     * memory that was actually unix_sock_t fields) instead of writing. */
    if (sock->domain == AF_UNIX) {
        char dest_path[UNIX_PATH_MAX];
        bool have_dest = false;
        if (uaddr && uaddrlen >= sizeof(sa_family_t)) {
            s64 perr = copy_user_sockaddr_un_path(uaddr, uaddrlen, dest_path, &have_dest);
            if (perr < 0) { kfree(kbuf); return perr; }
        }
        file_t *f = sock->file;
        bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);
        res = unix_socket_sendmsg(sock->uds, have_dest ? dest_path : NULL, kbuf, len, NULL, 0, nonblock);
        kfree(kbuf);
        return res;
    }

    /* AF_PACKET, same reasoning as the AF_UNIX check above: its SOCK_RAW
     * type value collides with AF_INET's raw_sock_t dispatch below, and
     * sock->raw/sock->pkt are the same union storage — sock->raw->protocol
     * would read a pkt_sock_t's rx_queue as if it were that int. */
    if (sock->domain == AF_PACKET && sock->pkt) {
        res = packet_send(kbuf, len);
        kfree(kbuf);
        return res;
    }

    /* AF_NETLINK, same reasoning as AF_PACKET just above — sock->nl aliases
     * the same union storage sock->raw would read as a raw_sock_t. */
    if (sock->domain == AF_NETLINK && sock->nl) {
        res = netlink_socket_send(sock->nl, kbuf, len);
        kfree(kbuf);
        return res;
    }

    if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_send(sock->tcp, kbuf, len, flags);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (uaddr) {
            if (uaddrlen < sizeof(struct sockaddr_in)) {
                kfree(kbuf);
                return -(s64)EINVAL;
            }
            struct sockaddr_in sin;
            if (copy_from_user(&sin, uaddr, sizeof(sin)) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            res = udp_sendto(sock->udp, kbuf, len, (const u8 *)&sin.sin_addr.s_addr, ntohs(sin.sin_port));
        } else {
            res = udp_sendto(sock->udp, kbuf, len, NULL, 0);
        }
    } else if (sock->type == SOCK_RAW && sock->raw) {
        if (sock->ip_hdrincl) {
            /* IP_HDRINCL: `kbuf` is the complete datagram the caller built —
             * IPv4 header and all. No destination address needed from
             * sendto() at all (a real ping(8)/traceroute(8) sets one anyway,
             * but the header's own dst_ip is what actually gets used, the
             * same as Linux). See ipv4_send_prebuilt(). */
            net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + len);
            if (!buf) {
                kfree(kbuf);
                return -(s64)ENOMEM;
            }
            net_buf_reserve(buf, NET_BUF_HEADROOM);
            void *p = net_buf_put(buf, len);
            memcpy(p, kbuf, len);
            int err = ipv4_send_prebuilt(buf);
            res = (err < 0) ? (s64)err : (s64)len;
        } else if (uaddr) {
            struct sockaddr_in sin;
            if (copy_from_user(&sin, uaddr, sizeof(sin)) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            net_buf_t *buf = net_buf_alloc(NET_BUF_HEADROOM + len);
            if (!buf) {
                kfree(kbuf);
                return -(s64)ENOMEM;
            }
            net_buf_reserve(buf, NET_BUF_HEADROOM);
            void *p = net_buf_put(buf, len);
            memcpy(p, kbuf, len);
            int err = ipv4_send(buf, (const u8 *)&sin.sin_addr.s_addr, (u8)sock->raw->protocol);
            if (err < 0) res = (s64)err;
            else res = (s64)len;
        }
    }

    kfree(kbuf);
    if (res < 0 && res == -(s64)EPIPE && !(flags & 0x4000 /* MSG_NOSIGNAL */)) {
        process_t *proc = sched_current_process();
        if (proc) {
            sched_kill_process(proc->pid, 13 /* SIGPIPE */);
        }
    }
    return res;
}

s64 sys_recvfrom_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    void *ubuf = (void *)r->rsi;
    size_t len = (size_t)r->rdx;
    int flags = (int)r->r10;
    struct sockaddr *uaddr = (struct sockaddr *)r->r8;
    socklen_t *uaddrlen = (socklen_t *)r->r9;
    (void)flags;

    if (len && (!ubuf || (uintptr_t)ubuf >= TASK_SIZE_MAX)) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;
    /* UDP zero-length calls send/consume a datagram, not a stream no-op. */
    if (!len && !(sock->domain == AF_INET && sock->type == SOCK_DGRAM)) return 0;

    process_t *proc = sched_current_process();
    file_t *f = sock->file;
    bool nonblock = (f ? ((f->f_flags & O_NONBLOCK) != 0) : false) || ((flags & 0x40 /* MSG_DONTWAIT */) != 0);

    /* AF_UNIX must be checked before sock->type — see sys_sendto_impl's
     * identical comment just above; the same tcp/uds union aliasing bug
     * applied here on the receive side too. */
    if (sock->domain == AF_UNIX) {
        size_t clen = len > SOCK_XFER_MAX ? SOCK_XFER_MAX : len;
        void *kbuf = kmalloc(clen);
        if (!kbuf) return -(s64)ENOMEM;
        char src_path[UNIX_PATH_MAX];
        s64 res = unix_socket_recvmsg(sock->uds, kbuf, clen, src_path, NULL, nonblock);
        if (res > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            if (uaddr && uaddrlen) {
                struct sockaddr_un sun;
                memset(&sun, 0, sizeof(sun));
                sun.sun_family = AF_UNIX;
                strncpy(sun.sun_path, src_path, UNIX_PATH_MAX - 1);
                copy_to_user(uaddr, &sun, sizeof(sun));
                socklen_t slen = sizeof(sun);
                copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
            }
        }
        /* Any SCM_RIGHTS fds this datagram carried are silently dropped —
         * plain recvfrom(2) has no ancillary-data channel to return them
         * through, matching real Linux's own behavior here exactly (the
         * fds are simply closed, never leaked to any process). */
        kfree(kbuf);
        return res;
    }

    /* AF_PACKET — same union-aliasing reasoning as sys_sendto_impl's
     * identical check. `uaddr` (a struct sockaddr_ll on real Linux) is left
     * unfilled: nothing here builds one, so a caller wanting the frame's
     * source interface/protocol has to get it from the frame itself (this
     * kernel has exactly one interface anyway) rather than from recvfrom()'s
     * address output — a real limitation, not silently wrong data. */
    if (sock->domain == AF_PACKET && sock->pkt) {
        for (;;) {
            net_buf_t *frame = net_buf_queue_pop(&sock->pkt->rx_queue);
            if (frame) {
                size_t clen = (frame->len < len) ? frame->len : len;
                if (copy_to_user(ubuf, frame->data, clen) != 0) {
                    net_buf_free(frame);
                    return -(s64)EFAULT;
                }
                net_buf_free(frame);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            irqflags_t pflags = spinlock_lock_irqsave(&sock->pkt->lock);
            if (net_buf_queue_len(&sock->pkt->rx_queue) == 0) {
                sock->pkt->wait_thread = sched_current_thread();
                spinlock_unlock_irqrestore(&sock->pkt->lock, pflags);
                sched_block(THREAD_BLOCKED_PENDING);

                if (proc && (proc->sig_pending & ~proc->sig_blocked)) {
                    pflags = spinlock_lock_irqsave(&sock->pkt->lock);
                    if (sock->pkt->wait_thread == sched_current_thread())
                        sock->pkt->wait_thread = NULL;
                    spinlock_unlock_irqrestore(&sock->pkt->lock, pflags);
                    return -(s64)EINTR;
                }
            } else {
                spinlock_unlock_irqrestore(&sock->pkt->lock, pflags);
            }
        }
    }

    /* AF_NETLINK, same union-aliasing reasoning as its sys_sendto_impl
     * check. `uaddr` is left unfilled the same way AF_PACKET's is above —
     * nothing here builds a struct sockaddr_nl, and a real netlink client
     * doesn't need one to make sense of the reply (it's just the kernel's
     * well-known nl_pid 0, always). */
    if (sock->domain == AF_NETLINK && sock->nl) {
        for (;;) {
            net_buf_t *msg = net_buf_queue_pop(&sock->nl->rx_queue);
            if (msg) {
                size_t clen = (msg->len < len) ? msg->len : len;
                if (copy_to_user(ubuf, msg->data, clen) != 0) {
                    net_buf_free(msg);
                    return -(s64)EFAULT;
                }
                net_buf_free(msg);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            irqflags_t nflags = spinlock_lock_irqsave(&sock->nl->lock);
            if (net_buf_queue_len(&sock->nl->rx_queue) == 0) {
                sock->nl->wait_thread = sched_current_thread();
                spinlock_unlock_irqrestore(&sock->nl->lock, nflags);
                sched_block(THREAD_BLOCKED_PENDING);

                if (proc && (proc->sig_pending & ~proc->sig_blocked)) {
                    nflags = spinlock_lock_irqsave(&sock->nl->lock);
                    if (sock->nl->wait_thread == sched_current_thread())
                        sock->nl->wait_thread = NULL;
                    spinlock_unlock_irqrestore(&sock->nl->lock, nflags);
                    return -(s64)EINTR;
                }
            } else {
                spinlock_unlock_irqrestore(&sock->nl->lock, nflags);
            }
        }
    }

    /* Same unbounded-kmalloc guard as sendto. Returning fewer bytes than the
     * caller's buffer size is always valid for recv, so a plain clamp is a
     * correct short read here. */
    if (len > SOCK_XFER_MAX) len = SOCK_XFER_MAX;

    void *kbuf = kmalloc(len ? len : 1);
    if (!kbuf) return -(s64)ENOMEM;

    if (sock->type == SOCK_STREAM && sock->tcp) {
        s64 res = tcp_recv(sock->tcp, kbuf, len, nonblock);
        if (res > 0) {
            if (copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
        }
        kfree(kbuf);
        return res;
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        u8 src_ip[4];
        u16 src_port = 0;
        s64 res = udp_recvfrom(sock->udp, kbuf, len, src_ip, &src_port, nonblock);
        if (res >= 0) {
            if (res && copy_to_user(ubuf, kbuf, (size_t)res) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            if (uaddr && uaddrlen) {
                struct sockaddr_in sin;
                memset(&sin, 0, sizeof(sin));
                sin.sin_family = AF_INET;
                sin.sin_port = htons(src_port);
                memcpy(&sin.sin_addr.s_addr, src_ip, 4);

                socklen_t capacity, slen = sizeof(sin);
                if (copy_from_user(&capacity, uaddrlen, sizeof(capacity)) ||
                    copy_to_user(uaddr, &sin, capacity < slen ? capacity : slen) ||
                    copy_to_user(uaddrlen, &slen, sizeof(slen))) {
                    kfree(kbuf);
                    return -(s64)EFAULT;
                }
            }
        }
        kfree(kbuf);
        return res;
    } else if (sock->type == SOCK_RAW && sock->raw) {
        kfree(kbuf);
        for (;;) {
            net_buf_t *pkt = net_buf_queue_pop(&sock->raw->rx_queue);
            if (pkt) {
                if (pkt->len < 6) {
                    net_buf_free(pkt);
                    continue;
                }
                size_t psize = pkt->len - 6;
                size_t clen = (psize < len) ? psize : len;
                if (copy_to_user(ubuf, pkt->data + 6, clen) != 0) {
                    net_buf_free(pkt);
                    return -(s64)EFAULT;
                }
                if (uaddr && uaddrlen) {
                    struct sockaddr_in sin;
                    memset(&sin, 0, sizeof(sin));
                    sin.sin_family = AF_INET;
                    memcpy(&sin.sin_addr.s_addr, pkt->data, 4);
                    copy_to_user(uaddr, &sin, sizeof(sin));
                    socklen_t slen = sizeof(sin);
                    copy_to_user(uaddrlen, &slen, sizeof(socklen_t));
                }
                net_buf_free(pkt);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            spinlock_lock(&sock->raw->lock);
            if (net_buf_queue_len(&sock->raw->rx_queue) == 0) {
                sock->raw->wait_thread = sched_current_thread();
                spinlock_unlock(&sock->raw->lock);
                sched_block(THREAD_BLOCKED_PENDING);
            } else {
                spinlock_unlock(&sock->raw->lock);
            }
        }
    }

    kfree(kbuf);
    return -(s64)EOPNOTSUPP;
}

/* Must stay field-for-field identical to `struct msghdr`/`struct cmsghdr` in
 * userland/libc/include/sys/socket.h — same reasoning as struct linux_timex
 * above. */
struct linux_msghdr {
    void         *msg_name;
    socklen_t     msg_namelen;
    struct iovec *msg_iov;
    size_t        msg_iovlen;
    void         *msg_control;
    size_t        msg_controllen;
    int           msg_flags;
};

struct linux_cmsghdr {
    size_t cmsg_len;
    int    cmsg_level;
    int    cmsg_type;
};

/* Real sendmsg(2)/recvmsg(2): gather/scatter across every iovec (not just
 * iov[0], which is all SYS_sendmsg/SYS_recvmsg used to do when they were
 * literal aliases for sys_sendto_impl/sys_recvfrom_impl — see the reg()
 * calls below), plus SCM_RIGHTS fd-passing for AF_UNIX
 * (kernel/net/unix_socket.c). A handful of iovecs is the overwhelmingly
 * common case for real callers (X11/Wayland-style protocols, systemd/dbus
 * fd-passing), so both cap at a fixed, on-stack SENDMSG_MAX_IOV rather than
 * readv/writev's 1024 — a caller past that gets -EINVAL rather than a
 * kmalloc'd array, matching this file's general preference for bounded
 * stack allocations in these paths. */
#define SENDMSG_MAX_IOV 16

s64 sys_sendmsg_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    const struct linux_msghdr *umsg = (const struct linux_msghdr *)r->rsi;
    int flags = (int)r->rdx;

    if (!umsg) return -(s64)EFAULT;
    if ((uintptr_t)umsg >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    struct linux_msghdr msg;
    if (copy_from_user(&msg, umsg, sizeof(msg)) != 0) return -(s64)EFAULT;
    if (msg.msg_iovlen > SENDMSG_MAX_IOV) return -(s64)EINVAL;

    struct iovec kiov[SENDMSG_MAX_IOV];
    size_t total = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        if (copy_from_user(kiov, msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec)) != 0)
            return -(s64)EFAULT;
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (kiov[i].iov_len > 0x7FFFFFFF) return -(s64)EINVAL;
            total += kiov[i].iov_len;
        }
    }
    if (total > SOCK_XFER_MAX) {
        if (sock->type == SOCK_DGRAM) return -(s64)EMSGSIZE;
        total = SOCK_XFER_MAX; /* stream socket: short send, same as sendto's own clamp */
    }

    void *kbuf = NULL;
    if (total > 0) {
        kbuf = kmalloc(total);
        if (!kbuf) return -(s64)ENOMEM;
        size_t off = 0;
        for (size_t i = 0; i < msg.msg_iovlen && off < total; i++) {
            size_t take = kiov[i].iov_len;
            if (off + take > total) take = total - off;
            if (take > 0 && copy_from_user((u8 *)kbuf + off, kiov[i].iov_base, take) != 0) {
                kfree(kbuf);
                return -(s64)EFAULT;
            }
            off += take;
        }
    }

    /* SCM_RIGHTS is only meaningful — and only parsed — for AF_UNIX; a
     * non-unix socket's msg_control is silently ignored, matching real
     * Linux (ancillary data is a per-protocol-family feature there too). */
    file_t *out_fds[UNIX_SCM_MAX_FDS];
    int out_nfds = 0;
    process_t *proc = sched_current_process();
    if (sock->domain == AF_UNIX && msg.msg_control && msg.msg_controllen >= sizeof(struct linux_cmsghdr)) {
        size_t cctrllen = msg.msg_controllen;
        if (cctrllen > 4096) cctrllen = 4096; /* bound the control-buffer copy */
        u8 *ctrl = (u8 *)kmalloc(cctrllen);
        if (!ctrl) { if (kbuf) kfree(kbuf); return -(s64)ENOMEM; }
        if (copy_from_user(ctrl, msg.msg_control, cctrllen) != 0) {
            kfree(ctrl); if (kbuf) kfree(kbuf); return -(s64)EFAULT;
        }
        size_t off = 0;
        while (off + sizeof(struct linux_cmsghdr) <= cctrllen) {
            struct linux_cmsghdr *ch = (struct linux_cmsghdr *)(ctrl + off);
            if (ch->cmsg_len < sizeof(struct linux_cmsghdr) || off + ch->cmsg_len > cctrllen) break;
            if (ch->cmsg_level == SOL_SOCKET && ch->cmsg_type == SCM_RIGHTS) {
                int *fdp = (int *)(void *)(ch + 1);
                size_t nfd_here = (ch->cmsg_len - sizeof(struct linux_cmsghdr)) / sizeof(int);
                for (size_t i = 0; i < nfd_here && out_nfds < UNIX_SCM_MAX_FDS; i++) {
                    file_t *uf = fget(proc, fdp[i]);
                    if (!uf) {
                        for (int j = 0; j < out_nfds; j++) fput(out_fds[j]);
                        kfree(ctrl); if (kbuf) kfree(kbuf);
                        return -(s64)EBADF;
                    }
                    out_fds[out_nfds++] = uf;
                }
            }
            size_t adv = (ch->cmsg_len + sizeof(size_t) - 1) & ~(sizeof(size_t) - 1);
            if (adv == 0) break;
            off += adv;
        }
        kfree(ctrl);
    }

    char dest_path[UNIX_PATH_MAX];
    bool have_dest = false;
    if (sock->domain == AF_UNIX && msg.msg_name && msg.msg_namelen >= sizeof(sa_family_t)) {
        s64 perr = copy_user_sockaddr_un_path((const struct sockaddr *)msg.msg_name, msg.msg_namelen,
                                               dest_path, &have_dest);
        if (perr < 0) { for (int j = 0; j < out_nfds; j++) fput(out_fds[j]); if (kbuf) kfree(kbuf); return perr; }
    }

    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);

    s64 res;
    if (sock->domain == AF_UNIX) {
        res = unix_socket_sendmsg(sock->uds, have_dest ? dest_path : NULL, kbuf, total,
                                   out_fds, out_nfds, nonblock);
        if (res < 0) for (int j = 0; j < out_nfds; j++) fput(out_fds[j]);
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_send(sock->tcp, kbuf, total, flags);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in sin;
            if (copy_from_user(&sin, msg.msg_name, sizeof(sin)) != 0) {
                if (kbuf) kfree(kbuf);
                return -(s64)EFAULT;
            }
            res = udp_sendto(sock->udp, kbuf, total, (const u8 *)&sin.sin_addr.s_addr, ntohs(sin.sin_port));
        } else {
            res = udp_sendto(sock->udp, kbuf, total, NULL, 0);
        }
    } else {
        res = -(s64)EOPNOTSUPP;
    }

    if (kbuf) kfree(kbuf);
    if (res < 0 && res == -(s64)EPIPE && !(flags & 0x4000 /* MSG_NOSIGNAL */)) {
        if (proc) sched_kill_process(proc->pid, 13 /* SIGPIPE */);
    }
    return res;
}

s64 sys_recvmsg_impl(pt_regs_t *r)
{
    int fd = (int)r->rdi;
    struct linux_msghdr *umsg = (struct linux_msghdr *)r->rsi;
    int flags = (int)r->rdx;

    if (!umsg) return -(s64)EFAULT;
    if ((uintptr_t)umsg >= TASK_SIZE_MAX) return -(s64)EFAULT;

    socket_t *sock = NULL;
    int ret = sock_get_from_fd(fd, &sock);
    if (ret < 0) return -(s64)ret;

    struct linux_msghdr msg;
    if (copy_from_user(&msg, umsg, sizeof(msg)) != 0) return -(s64)EFAULT;
    if (msg.msg_iovlen > SENDMSG_MAX_IOV) return -(s64)EINVAL;

    struct iovec kiov[SENDMSG_MAX_IOV];
    size_t total = 0;
    if (msg.msg_iov && msg.msg_iovlen > 0) {
        if (copy_from_user(kiov, msg.msg_iov, msg.msg_iovlen * sizeof(struct iovec)) != 0)
            return -(s64)EFAULT;
        for (size_t i = 0; i < msg.msg_iovlen; i++) {
            if (kiov[i].iov_len > 0x7FFFFFFF) return -(s64)EINVAL;
            total += kiov[i].iov_len;
        }
    }
    if (total > SOCK_XFER_MAX) total = SOCK_XFER_MAX;

    void *kbuf = total ? kmalloc(total) : NULL;
    if (total && !kbuf) return -(s64)ENOMEM;

    process_t *proc = sched_current_process();
    file_t *f = (file_t *)proc->handle_table[fd];
    bool nonblock = (f && (f->f_flags & O_NONBLOCK)) || (flags & 0x40 /* MSG_DONTWAIT */);

    char src_path[UNIX_PATH_MAX] = {0};
    int uds_nfds = 0;
    file_t *uds_fds[UNIX_SCM_MAX_FDS];
    s64 res;

    if (sock->domain == AF_UNIX) {
        res = unix_socket_recvmsg(sock->uds, kbuf, total, src_path, &uds_nfds, nonblock);
        if (res >= 0 && uds_nfds > 0) {
            uds_nfds = unix_socket_recvmsg_take_fds(sock->uds, uds_fds, UNIX_SCM_MAX_FDS);
        }
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        res = tcp_recv(sock->tcp, kbuf, total, nonblock);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        u8 src_ip[4]; u16 src_port = 0;
        res = udp_recvfrom(sock->udp, kbuf, total, src_ip, &src_port, nonblock);
        if (res >= 0 && msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_in)) {
            struct sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = AF_INET;
            sin.sin_port = htons(src_port);
            memcpy(&sin.sin_addr.s_addr, src_ip, 4);
            copy_to_user(msg.msg_name, &sin, sizeof(sin));
            socklen_t slen = sizeof(sin);
            copy_to_user(&umsg->msg_namelen, &slen, sizeof(slen));
        }
    } else {
        res = -(s64)EOPNOTSUPP;
    }

    if (res < 0) {
        if (kbuf) kfree(kbuf);
        return res;
    }

    /* Scatter the received bytes back out across the caller's iovecs. */
    size_t remaining = (size_t)res;
    size_t off = 0;
    for (size_t i = 0; i < msg.msg_iovlen && remaining > 0; i++) {
        size_t take = kiov[i].iov_len;
        if (take > remaining) take = remaining;
        if (take > 0) {
            if (copy_to_user(kiov[i].iov_base, (u8 *)kbuf + off, take) != 0) {
                if (kbuf) kfree(kbuf);
                return -(s64)EFAULT;
            }
            off += take;
            remaining -= take;
        }
    }
    if (kbuf) kfree(kbuf);

    if (sock->domain == AF_UNIX && msg.msg_name && msg.msg_namelen >= sizeof(struct sockaddr_un)) {
        struct sockaddr_un sun;
        memset(&sun, 0, sizeof(sun));
        sun.sun_family = AF_UNIX;
        strncpy(sun.sun_path, src_path, UNIX_PATH_MAX - 1);
        copy_to_user(msg.msg_name, &sun, sizeof(sun));
        socklen_t slen = sizeof(sun);
        copy_to_user(&umsg->msg_namelen, &slen, sizeof(slen));
    }

    /* SCM_RIGHTS: install each received fd into *this* (receiving)
     * process's own fd table (syscall_install_fd, exported for exactly this
     * kind of "mint an fd for something that isn't a fresh open()" case —
     * see its declaration in kernel/syscall/syscall.h) and describe them to
     * the caller in msg_control, same cmsghdr layout CMSG_FIRSTHDR()/
     * CMSG_DATA() in userland's sys/socket.h expect. */
    int out_flags = 0;
    if (uds_nfds > 0 && msg.msg_control && msg.msg_controllen >= sizeof(struct linux_cmsghdr)) {
        int installed[UNIX_SCM_MAX_FDS];
        int n_installed = 0;
        for (int i = 0; i < uds_nfds; i++) {
            s64 nfd = syscall_install_fd(proc, uds_fds[i], 0);
            if (nfd < 0) { vfs_close(uds_fds[i]); continue; } /* no room: drop rather than leak */
            installed[n_installed++] = (int)nfd;
        }
        int fit = n_installed;
        while (fit > 0 && sizeof(struct linux_cmsghdr) + (size_t)fit * sizeof(int) > msg.msg_controllen) fit--;
        if (fit < n_installed) out_flags |= 0x08; /* MSG_CTRUNC */

        u8 cbuf[sizeof(struct linux_cmsghdr) + UNIX_SCM_MAX_FDS * sizeof(int)];
        struct linux_cmsghdr *ch = (struct linux_cmsghdr *)(void *)cbuf;
        ch->cmsg_len = sizeof(struct linux_cmsghdr) + (size_t)fit * sizeof(int);
        ch->cmsg_level = SOL_SOCKET;
        ch->cmsg_type = SCM_RIGHTS;
        memcpy(cbuf + sizeof(struct linux_cmsghdr), installed, (size_t)fit * sizeof(int));
        copy_to_user(msg.msg_control, cbuf, ch->cmsg_len);
        size_t clen = ch->cmsg_len;
        copy_to_user(&umsg->msg_controllen, &clen, sizeof(clen));
    } else {
        if (uds_nfds > 0) {
            /* Fds were dequeued but the caller gave no control buffer to
             * receive them in: close rather than leak the reference. */
            for (int i = 0; i < uds_nfds; i++) vfs_close(uds_fds[i]);
        }
        if (msg.msg_controllen > 0) {
            size_t zero = 0;
            copy_to_user(&umsg->msg_controllen, &zero, sizeof(zero));
        }
    }
    copy_to_user(&umsg->msg_flags, &out_flags, sizeof(out_flags));

    return res;
}

s64 sys_socketpair_impl(pt_regs_t *r)
{
    int domain = (int)r->rdi;
    int type = (int)r->rsi;
    int protocol = (int)r->rdx;
    int *user_sv = (int *)r->r10;

    (void)protocol;
    if (domain != 1 /* AF_UNIX */) return -(s64)EAFNOSUPPORT;
    if (!user_sv || (uintptr_t)user_sv >= TASK_SIZE_MAX) return -(s64)EFAULT;

    process_t *proc = sched_current_process();
    if (!proc) return -(s64)EPERM;

    file_t *rf = NULL, *wf = NULL;
    int err = sockpair_create(&rf, &wf);
    if (err < 0) return (s64)err;

    int fd0, fd1;
    if (fd_install_pair(proc, rf, wf, (type & 02000000 /* SOCK_CLOEXEC */) ? FD_CLOEXEC : 0,
                        &fd0, &fd1) < 0) {
        vfs_close(rf);
        vfs_close(wf);
        return -(s64)EMFILE;
    }

    int sv[2] = { fd0, fd1 };
    if (copy_to_user(user_sv, sv, sizeof(sv)) != 0) {
        vfs_close(fd_detach(proc, fd0));
        vfs_close(fd_detach(proc, fd1));
        return -(s64)EFAULT;
    }
    return 0;
}

/* sendmmsg/recvmmsg — iterate the mmsghdr array over sendmsg/recvmsg. */
static s64 sys_mmsg(pt_regs_t *r, bool send)
{
    int fd = (int)r->rdi;
    u8 *umsgvec = (u8 *)r->rsi;
    unsigned vlen = (unsigned)r->rdx;
    unsigned flags = (unsigned)r->r10;
    if (!umsgvec || (uintptr_t)umsgvec >= TASK_SIZE_MAX) return -(s64)EFAULT;
    if (vlen > 1024) vlen = 1024;

    /* struct mmsghdr { struct msghdr msg_hdr; unsigned msg_len; }; msghdr is
     * 56 bytes on x86_64, so the element stride is 64 (with padding). */
    const unsigned STRIDE = 64, MSGLEN_OFF = 56;
    unsigned done = 0;
    for (; done < vlen; done++) {
        u8 *elem = umsgvec + (u64)done * STRIDE;
        pt_regs_t s = *r;
        s.rdi = (u64)fd;
        s.rsi = (u64)(uintptr_t)elem;                   /* &msg_hdr */
        s.rdx = (u64)flags;
        s64 n = send ? sys_sendto_impl(&s) : sys_recvfrom_impl(&s);
        if (n < 0) return done ? (s64)done : n;
        u32 msglen = (u32)n;
        if (copy_to_user(elem + MSGLEN_OFF, &msglen, sizeof msglen) != 0)
            return done ? (s64)done : -(s64)EFAULT;
        if (!send && n == 0) { done++; break; }
    }
    return (s64)done;
}
s64 sys_sendmmsg_impl(pt_regs_t *r) { return sys_mmsg(r, true);  }
s64 sys_recvmmsg_impl(pt_regs_t *r) { return sys_mmsg(r, false); }
