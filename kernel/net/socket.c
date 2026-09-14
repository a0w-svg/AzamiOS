/* ============================================================================
 * AzamiOS — Kernel BSD Socket Layer & VFS File Operations (socket.c)
 * File: kernel/net/socket.c
 *
 * Implements socket allocation, VFS read/write/poll/release file operations,
 * and descriptor lookup.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "../../include/azami/socket.h"
#include "../../include/azami/net.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"

#ifndef POLLIN
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020
#endif

static raw_sock_t *g_raw_sockets = NULL;
static spinlock_t  g_raw_lock = SPINLOCK_INIT;

/* g_raw_lock and each raw_sock_t's own ->lock use spinlock_lock_irqsave()/
 * _irqrestore() throughout this section, for the same reason as
 * kernel/net/tcp.c's identical note: raw_input() runs from a timer
 * interrupt on any CPU (net_poll() -> ... -> ipv4_input() -> raw_input()),
 * and takes both locks, while raw_socket_create()/raw_socket_close() and
 * sock_fop_read()'s SOCK_RAW wait path take them from ordinary process
 * context. A plain spinlock_lock() left interrupts enabled across those
 * process-context critical sections, so that timer interrupt could land on
 * the CPU already holding one of these locks and spin forever inside
 * raw_input()'s own acquire. See kernel/net/tcp.c for the full writeup. */

raw_sock_t *raw_socket_create(int protocol)
{
    raw_sock_t *r = (raw_sock_t *)kzalloc(sizeof(raw_sock_t));
    if (!r) return NULL;
    r->protocol = protocol;
    net_buf_queue_init(&r->rx_queue);
    spinlock_init(&r->lock);
    r->wait_thread = NULL;

    irqflags_t flags = spinlock_lock_irqsave(&g_raw_lock);
    r->next = g_raw_sockets;
    g_raw_sockets = r;
    spinlock_unlock_irqrestore(&g_raw_lock, flags);
    return r;
}

void raw_socket_close(raw_sock_t *raw)
{
    if (!raw) return;
    irqflags_t list_flags = spinlock_lock_irqsave(&g_raw_lock);
    raw_sock_t **curr = &g_raw_sockets;
    while (*curr) {
        if (*curr == raw) {
            *curr = raw->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock_irqrestore(&g_raw_lock, list_flags);

    irqflags_t sock_flags = spinlock_lock_irqsave(&raw->lock);
    net_buf_queue_purge(&raw->rx_queue);
    if (raw->wait_thread) {
        sched_unblock(raw->wait_thread);
        raw->wait_thread = NULL;
    }
    spinlock_unlock_irqrestore(&raw->lock, sock_flags);
    kfree(raw);
}

void raw_input(net_buf_t *buf, const ipv4_hdr_t *ip)
{
    if (!buf || !ip) return;
    irqflags_t list_flags = spinlock_lock_irqsave(&g_raw_lock);
    raw_sock_t *curr = g_raw_sockets;
    while (curr) {
        if (curr->protocol == 0 || curr->protocol == ip->protocol) {
            net_buf_t *clone = net_buf_clone(buf);
            if (clone) {
                /* Prepend 6-byte header containing [src_ip 4B][protocol 2B] */
                u8 *hdr = (u8 *)net_buf_push(clone, 6);
                if (hdr) {
                    memcpy(hdr, ip->src_ip, 4);
                    hdr[4] = (u8)(ip->protocol & 0xFF);
                    hdr[5] = 0;
                    net_buf_queue_push(&curr->rx_queue, clone);
                    irqflags_t sock_flags = spinlock_lock_irqsave(&curr->lock);
                    if (curr->wait_thread) {
                        sched_unblock(curr->wait_thread);
                        curr->wait_thread = NULL;
                    }
                    spinlock_unlock_irqrestore(&curr->lock, sock_flags);
                } else {
                    net_buf_free(clone);
                }
            }
        }
        curr = curr->next;
    }
    spinlock_unlock_irqrestore(&g_raw_lock, list_flags);
}

static s64 sock_fop_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    socket_t *sock = (socket_t *)filp->private_data;
    bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;

    if (sock->domain == AF_UNIX && sock->uds) {
        return unix_socket_recvmsg(sock->uds, buf, len, NULL, NULL, nonblock);
    } else if (sock->domain == AF_PACKET && sock->pkt) {
        for (;;) {
            net_buf_t *frame = net_buf_queue_pop(&sock->pkt->rx_queue);
            if (frame) {
                size_t clen = (frame->len < len) ? frame->len : len;
                memcpy(buf, frame->data, clen);
                net_buf_free(frame);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            irqflags_t flags = spinlock_lock_irqsave(&sock->pkt->lock);
            if (net_buf_queue_len(&sock->pkt->rx_queue) == 0) {
                sock->pkt->wait_thread = sched_current_thread();
                spinlock_unlock_irqrestore(&sock->pkt->lock, flags);
                sched_block(THREAD_BLOCKED_PENDING);

                process_t *p = sched_current_process();
                if (p && (p->sig_pending & ~p->sig_blocked)) {
                    flags = spinlock_lock_irqsave(&sock->pkt->lock);
                    if (sock->pkt->wait_thread == sched_current_thread())
                        sock->pkt->wait_thread = NULL;
                    spinlock_unlock_irqrestore(&sock->pkt->lock, flags);
                    return -(s64)EINTR;
                }
            } else {
                spinlock_unlock_irqrestore(&sock->pkt->lock, flags);
            }
        }
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        return tcp_recv(sock->tcp, buf, len, nonblock);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return udp_recvfrom(sock->udp, buf, len, NULL, NULL, nonblock);
    } else if (sock->type == SOCK_RAW && sock->raw) {
        for (;;) {
            net_buf_t *pkt = net_buf_queue_pop(&sock->raw->rx_queue);
            if (pkt) {
                size_t psize = pkt->len > 6 ? (pkt->len - 6) : pkt->len;
                size_t clen = (psize < len) ? psize : len;
                memcpy(buf, pkt->data + 6, clen);
                net_buf_free(pkt);
                return (s64)clen;
            }
            if (nonblock) return -(s64)EAGAIN;
            irqflags_t flags = spinlock_lock_irqsave(&sock->raw->lock);
            if (net_buf_queue_len(&sock->raw->rx_queue) == 0) {
                sock->raw->wait_thread = sched_current_thread();
                spinlock_unlock_irqrestore(&sock->raw->lock, flags);
                sched_block(THREAD_BLOCKED_PENDING);

                /* See the matching TCP/UDP fix: a pending signal must break
                 * a blocked read with -EINTR, not loop back to sleep. */
                process_t *p = sched_current_process();
                if (p && (p->sig_pending & ~p->sig_blocked)) {
                    flags = spinlock_lock_irqsave(&sock->raw->lock);
                    if (sock->raw->wait_thread == sched_current_thread())
                        sock->raw->wait_thread = NULL;
                    spinlock_unlock_irqrestore(&sock->raw->lock, flags);
                    return -(s64)EINTR;
                }
            } else {
                spinlock_unlock_irqrestore(&sock->raw->lock, flags);
            }
        }
    }
    return -EINVAL;
}

static s64 sock_fop_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    socket_t *sock = (socket_t *)filp->private_data;

    if (sock->domain == AF_UNIX && sock->uds) {
        bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;
        return unix_socket_sendmsg(sock->uds, NULL, buf, len, NULL, 0, nonblock);
    } else if (sock->domain == AF_PACKET && sock->pkt) {
        return packet_send(buf, len);
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        return tcp_send(sock->tcp, buf, len, 0);
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        return udp_sendto(sock->udp, buf, len, NULL, 0);
    }
    return -EINVAL;
}

static int sock_fop_poll(struct file *filp)
{
    if (!filp || !filp->private_data) return POLLNVAL;
    socket_t *sock = (socket_t *)filp->private_data;
    int mask = 0;

    if (sock->domain == AF_UNIX && sock->uds) {
        mask = unix_socket_poll(sock->uds);
    } else if (sock->domain == AF_PACKET && sock->pkt) {
        if (net_buf_queue_len(&sock->pkt->rx_queue) > 0) mask |= (POLLIN | POLLPRI);
        mask |= POLLOUT;
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        if (tcp_poll_in(sock->tcp)) mask |= (POLLIN | POLLPRI);
        if (tcp_poll_out(sock->tcp)) mask |= POLLOUT;
        if (sock->tcp->state == TCP_STATE_CLOSED) mask |= POLLHUP;
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        if (udp_poll(sock->udp)) mask |= (POLLIN | POLLPRI);
        mask |= POLLOUT;
    } else if (sock->type == SOCK_RAW && sock->raw) {
        if (net_buf_queue_len(&sock->raw->rx_queue) > 0) mask |= (POLLIN | POLLPRI);
        mask |= POLLOUT;
    }
    return mask;
}

static s64 sock_fop_release(struct inode *inode, struct file *filp)
{
    (void)inode;
    if (!filp || !filp->private_data) return 0;
    socket_t *sock = (socket_t *)filp->private_data;
    sock_free(sock);
    filp->private_data = NULL;
    return 0;
}

#include "../../kernel/uaccess.h"

/* ── Linux-ABI ifreq/ifconf ioctls on an AF_INET socket ─────────────────────
 *
 * A real Linux program never opens a private device node to ask about an
 * interface; it always goes through one of these standard ioctls on an
 * ordinary socket, with a struct ifreq (or struct ifconf, for "list every
 * interface") as the argument — that's how busybox's ifconfig/udhcpc, and
 * every iproute2-alike, do it. Getting the *numbers* right is necessary but
 * not sufficient: SIOCGIFINDEX, for instance, happens to share its numeric
 * value with this kernel's own /dev/netN convention (see net_ioctl() in
 * net.c, still used unchanged by that device node and by this OS's native
 * ifconfig.elf/dhcpcd.elf), but the two disagree on what the argument even
 * means there — an interface *name* to resolve to an index, per Linux,
 * versus an index to hand back the whole net_device_t for. Mixing the two
 * conventions on the same wire number is why a stock binary asking a
 * socket fd for SIOCGIFINDEX got EPERM instead of an index: this kernel
 * read the first four bytes of its ifreq's name as if they were the index
 * it was supposed to return.
 *
 * This implements the real wire format instead, scoped to socket fds only
 * — sock_fop_ioctl() below is the only caller, so the /dev/netN path and
 * this OS's own apps built against azami/net.h's ioctl numbers (several of
 * which do *not* match Linux's, e.g. SIOCGIFHWADDR/SIOCGIFNAME/SIOCGIFMTU)
 * are untouched. struct ifreq is 40 bytes on x86_64 (a 16-byte ifr_name
 * followed by a 24-byte union whose members all start at offset 16); this
 * only ever reads/writes at that fixed offset; it isn't a declared struct
 * here to avoid multiply-defining `struct ifreq` against a redefinition
 * risk. */
#define LX_IFNAMSIZ       16
#define LX_IFREQ_SIZE     40
#define LX_IFR_UNION_OFF  16

#define LX_SIOCGIFCONF    0x8912
#define LX_SIOCGIFFLAGS   0x8913
#define LX_SIOCSIFFLAGS   0x8914
#define LX_SIOCGIFADDR    0x8915
#define LX_SIOCSIFADDR    0x8916
#define LX_SIOCGIFNETMASK 0x891b
#define LX_SIOCSIFNETMASK 0x891c
#define LX_SIOCGIFBRDADDR 0x8919
#define LX_SIOCGIFMTU     0x8921
#define LX_SIOCSIFMTU     0x8922
#define LX_SIOCGIFHWADDR  0x8927
#define LX_SIOCGIFINDEX   0x8933

/* "eth0"/"net0"/"e1000" all name the one NIC this kernel currently brings
 * up; matches the same aliasing userland/libc/socket.c's if_nametoindex()
 * already does on the native side, so a real binary asking for "eth0" (the
 * name any Linux DHCP client or `ifconfig -a` expects) finds the device
 * this OS registered as "net0". */
static net_device_t *ifreq_lookup_device(const char *name)
{
    if (!name || !name[0]) return NULL;
    if (strcmp(name, "lo") == 0 || strcmp(name, "lo0") == 0) return net_get_device_by_name("lo");
    if (strcmp(name, "eth0") == 0 || strcmp(name, "net0") == 0 || strcmp(name, "e1000") == 0)
        return net_get_default_device();
    return net_get_device_by_name(name);
}

/* net_get_device_by_index()'s own numbering (0 == loopback, 1..N the rest)
 * has no reverse lookup; walk it rather than inventing a second scheme that
 * could drift out of sync with it. */
static int ifreq_device_index(net_device_t *dev)
{
    int count = net_get_device_count();
    for (int i = 0; i < count; i++) {
        if (net_get_device_by_index(i) == dev) return i;
    }
    return 0;
}

/* Returns -ENOTTY for any cmd this function doesn't handle, so the caller
 * can fall back to net_ioctl()'s own (differently-numbered-in-places)
 * command set. */
static s64 linux_ifreq_ioctl(u32 cmd, u64 arg)
{
    if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EFAULT;

    if (cmd == LX_SIOCGIFCONF) {
        /* struct ifconf { int ifc_len; union { char *ifc_buf; struct ifreq
         * *ifc_req; } ifc_ifcu; } — 8-byte aligned, so 16 bytes wide with
         * padding after the int. */
        struct { s32 ifc_len; s32 pad; u64 ifc_buf; } ifc;
        if (copy_from_user(&ifc, (const void *)(uintptr_t)arg, sizeof(ifc)) != 0)
            return -(s64)EFAULT;

        int ndev = net_get_device_count();
        int max_entries = (ifc.ifc_buf && ifc.ifc_len > 0) ? (int)(ifc.ifc_len / LX_IFREQ_SIZE) : 0;
        int written = 0;
        for (int i = 0; i < ndev && written < max_entries; i++) {
            net_device_t *dev = net_get_device_by_index(i);
            if (!dev) continue;
            u8 entry[LX_IFREQ_SIZE];
            memset(entry, 0, sizeof(entry));
            size_t nlen = strlen(dev->name);
            if (nlen >= LX_IFNAMSIZ) nlen = LX_IFNAMSIZ - 1;
            memcpy(entry, dev->name, nlen);
            u16 fam = AF_INET;
            memcpy(entry + LX_IFR_UNION_OFF, &fam, 2);
            memcpy(entry + LX_IFR_UNION_OFF + 4, dev->ip, 4);
            if (copy_to_user((void *)(uintptr_t)(ifc.ifc_buf + (u64)written * LX_IFREQ_SIZE),
                              entry, sizeof(entry)) != 0)
                return -(s64)EFAULT;
            written++;
        }
        s32 out_len = (s32)(written * LX_IFREQ_SIZE);
        if (copy_to_user((void *)(uintptr_t)arg, &out_len, sizeof(out_len)) != 0)
            return -(s64)EFAULT;
        return 0;
    }

    switch (cmd) {
    case LX_SIOCGIFFLAGS: case LX_SIOCSIFFLAGS:
    case LX_SIOCGIFADDR:  case LX_SIOCSIFADDR:
    case LX_SIOCGIFNETMASK: case LX_SIOCSIFNETMASK:
    case LX_SIOCGIFBRDADDR:
    case LX_SIOCGIFHWADDR:
    case LX_SIOCGIFMTU: case LX_SIOCSIFMTU:
    case LX_SIOCGIFINDEX:
        break;
    default:
        return -(s64)ENOTTY;
    }

    /* Every command reaching here takes a plain struct ifreq: an
     * interface name (always present on input) and one union member
     * (input for the SIOCS* commands, output for the SIOCG* ones). */
    u8 ifr[LX_IFREQ_SIZE];
    if (copy_from_user(ifr, (const void *)(uintptr_t)arg, sizeof(ifr)) != 0)
        return -(s64)EFAULT;
    char name[LX_IFNAMSIZ + 1];
    memcpy(name, ifr, LX_IFNAMSIZ);
    name[LX_IFNAMSIZ] = '\0';

    net_device_t *dev = ifreq_lookup_device(name);
    if (!dev) return -(s64)ENODEV;

    switch (cmd) {
    case LX_SIOCGIFINDEX: {
        s32 idx = (s32)ifreq_device_index(dev);
        memcpy(ifr + LX_IFR_UNION_OFF, &idx, 4);
        break;
    }
    case LX_SIOCGIFFLAGS: {
        s16 flags = (s16)dev->flags;
        memcpy(ifr + LX_IFR_UNION_OFF, &flags, 2);
        break;
    }
    case LX_SIOCSIFFLAGS: {
        s16 flags; memcpy(&flags, ifr + LX_IFR_UNION_OFF, 2);
        dev->flags = (u32)(u16)flags;
        return 0;
    }
    case LX_SIOCGIFADDR: {
        u16 fam = AF_INET;
        memset(ifr + LX_IFR_UNION_OFF, 0, 16);
        memcpy(ifr + LX_IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + LX_IFR_UNION_OFF + 4, dev->ip, 4);
        break;
    }
    case LX_SIOCSIFADDR: {
        memcpy(dev->ip, ifr + LX_IFR_UNION_OFF + 4, 4);
        if (dev == net_get_default_device()) net_set_ip(dev->ip);
        return 0;
    }
    case LX_SIOCGIFNETMASK: {
        u16 fam = AF_INET;
        memset(ifr + LX_IFR_UNION_OFF, 0, 16);
        memcpy(ifr + LX_IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + LX_IFR_UNION_OFF + 4, dev->netmask, 4);
        break;
    }
    case LX_SIOCSIFNETMASK: {
        memcpy(dev->netmask, ifr + LX_IFR_UNION_OFF + 4, 4);
        if (dev == net_get_default_device()) net_set_netmask(dev->netmask);
        return 0;
    }
    case LX_SIOCGIFBRDADDR: {
        u8 brd[4] = {
            (u8)(dev->ip[0] | (u8)~dev->netmask[0]), (u8)(dev->ip[1] | (u8)~dev->netmask[1]),
            (u8)(dev->ip[2] | (u8)~dev->netmask[2]), (u8)(dev->ip[3] | (u8)~dev->netmask[3]),
        };
        u16 fam = AF_INET;
        memset(ifr + LX_IFR_UNION_OFF, 0, 16);
        memcpy(ifr + LX_IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + LX_IFR_UNION_OFF + 4, brd, 4);
        break;
    }
    case LX_SIOCGIFHWADDR: {
        u16 fam = 1; /* ARPHRD_ETHER */
        memset(ifr + LX_IFR_UNION_OFF, 0, 16);
        memcpy(ifr + LX_IFR_UNION_OFF, &fam, 2);
        memcpy(ifr + LX_IFR_UNION_OFF + 2, dev->mac, 6);
        break;
    }
    case LX_SIOCGIFMTU: {
        s32 mtu = (s32)dev->mtu;
        memcpy(ifr + LX_IFR_UNION_OFF, &mtu, 4);
        break;
    }
    case LX_SIOCSIFMTU: {
        s32 mtu; memcpy(&mtu, ifr + LX_IFR_UNION_OFF, 4);
        dev->mtu = (u32)mtu;
        return 0;
    }
    }

    if (copy_to_user((void *)(uintptr_t)arg, ifr, sizeof(ifr)) != 0) return -(s64)EFAULT;
    return 0;
}

static s64 sock_fop_ioctl(struct file *filp, u32 cmd, u64 arg)
{
    if (!filp || !filp->private_data) return -(s64)EBADF;
    socket_t *sock = (socket_t *)filp->private_data;

    if (cmd == 0x5421 /* FIONBIO */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int val = 0;
        if (copy_from_user(&val, (const void *)(uintptr_t)arg, sizeof(int)) != 0) return -(s64)EFAULT;
        if (val) filp->f_flags |= O_NONBLOCK;
        else filp->f_flags &= ~O_NONBLOCK;
        return 0;
    }

    if (cmd == 0x541B /* FIONREAD */) {
        if (!arg || (uintptr_t)arg >= 0x8000000000000000ULL) return -(s64)EINVAL;
        int bytes = 0;
        /* AF_UNIX checked first — sock->tcp/sock->uds alias the same union
         * storage, so `sock->type == SOCK_STREAM && sock->tcp` below would
         * otherwise read a unix_sock_t's bytes as if they were a
         * tcp_sock_t's rx_len field. */
        if (sock->domain == AF_UNIX && sock->uds) {
            bytes = 0; /* not tracked precisely; a real byte count would need
                        * pipe_t's own count field exposed for the STREAM
                        * case and a per-message length sum for DGRAM. */
        } else if (sock->type == SOCK_STREAM && sock->tcp) {
            bytes = (int)sock->tcp->rx_len;
        } else if (sock->type == SOCK_DGRAM && sock->udp) {
            bytes = (int)net_buf_queue_len(&sock->udp->rx_queue);
        } else if (sock->domain == AF_PACKET && sock->pkt) {
            bytes = (int)net_buf_queue_len(&sock->pkt->rx_queue);
        } else if (sock->type == SOCK_RAW && sock->raw) {
            bytes = (int)net_buf_queue_len(&sock->raw->rx_queue);
        }
        if (copy_to_user((void *)(uintptr_t)arg, &bytes, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
    }

    {
        s64 lr = linux_ifreq_ioctl(cmd, arg);
        if (lr != -(s64)ENOTTY) return lr;
    }

    extern int net_ioctl(u32 cmd, u64 arg);
    return (s64)net_ioctl(cmd, arg);
}

static file_operations_t g_socket_fops = {
    .read = sock_fop_read,
    .write = sock_fop_write,
    .readdir = NULL,
    .ioctl = sock_fop_ioctl,
    .mmap = NULL,
    .open = NULL,
    .release = sock_fop_release,
    .poll = sock_fop_poll,
};

socket_t *sock_alloc(int domain, int type, int protocol)
{
    socket_t *sock = (socket_t *)kzalloc(sizeof(socket_t));
    if (!sock) return NULL;

    sock->domain = domain;
    sock->type = type;
    sock->protocol = protocol;

    /* AF_UNIX must be checked before the type-based dispatch below: an
     * AF_UNIX SOCK_STREAM request used to silently fall into the very same
     * `type == SOCK_STREAM` branch a real AF_INET socket does, handing back
     * a TCP socket that happened to work for byte-stream I/O but could
     * never be bound to a path, never carry SCM_RIGHTS, and reported the
     * wrong domain to getsockname(). */
    if (domain == AF_UNIX || domain == AF_LOCAL) {
        if (type != SOCK_STREAM && type != SOCK_DGRAM) {
            kfree(sock);
            return NULL;
        }
        sock->uds = unix_socket_create(type);
        if (!sock->uds) {
            kfree(sock);
            return NULL;
        }
        return sock;
    }

    /* AF_PACKET, like AF_UNIX above, must be checked before the type-based
     * dispatch below: SOCK_RAW's value collides with AF_INET's raw IP
     * sockets, and packet_socket_create() doesn't care about `protocol` the
     * way raw_socket_create() does (see pkt_sock_t's comment). */
    if (domain == AF_PACKET) {
        sock->pkt = packet_socket_create();
        if (!sock->pkt) {
            kfree(sock);
            return NULL;
        }
        return sock;
    }

    if (type == SOCK_STREAM || protocol == IPPROTO_TCP) {
        sock->tcp = tcp_socket_create();
        if (!sock->tcp) {
            kfree(sock);
            return NULL;
        }
    } else if (type == SOCK_DGRAM || protocol == IPPROTO_UDP) {
        sock->udp = udp_socket_create();
        if (!sock->udp) {
            kfree(sock);
            return NULL;
        }
    } else if (type == SOCK_RAW) {
        sock->raw = raw_socket_create(protocol);
        if (!sock->raw) {
            kfree(sock);
            return NULL;
        }
    }

    return sock;
}

void sock_free(socket_t *sock)
{
    if (!sock) return;

    if (sock->domain == AF_UNIX && sock->uds) {
        unix_socket_close(sock->uds);
        sock->uds = NULL;
    } else if (sock->domain == AF_PACKET && sock->pkt) {
        packet_socket_close(sock->pkt);
        sock->pkt = NULL;
    } else if (sock->type == SOCK_STREAM && sock->tcp) {
        tcp_socket_close(sock->tcp);
        sock->tcp = NULL;
    } else if (sock->type == SOCK_DGRAM && sock->udp) {
        udp_socket_close(sock->udp);
        sock->udp = NULL;
    } else if (sock->type == SOCK_RAW && sock->raw) {
        raw_socket_close(sock->raw);
        sock->raw = NULL;
    }

    kfree(sock);
}

file_t *sock_create_file(socket_t *sock)
{
    if (!sock) return NULL;

    file_t *f = (file_t *)kzalloc(sizeof(file_t));
    if (!f) return NULL;

    f->f_op = &g_socket_fops;
    f->private_data = sock;
    f->f_count = 1;
    f->f_flags = O_RDWR;
    sock->file = f;

    return f;
}

int sock_get_from_fd(int fd, socket_t **sock_out)
{
    if (fd < 0 || fd >= PROC_MAX_FDS || !sock_out) return -EBADF;

    process_t *proc = sched_current_process();
    if (!proc) return -EPERM;

    file_t *f = (file_t *)proc->handle_table[fd];
    if (!f) return -EBADF;

    if (f->f_op != &g_socket_fops || !f->private_data) {
        return -ENOTSOCK;
    }

    *sock_out = (socket_t *)f->private_data;
    return 0;
}
