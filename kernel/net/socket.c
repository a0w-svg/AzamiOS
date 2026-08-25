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

raw_sock_t *raw_socket_create(int protocol)
{
    raw_sock_t *r = (raw_sock_t *)kzalloc(sizeof(raw_sock_t));
    if (!r) return NULL;
    r->protocol = protocol;
    net_buf_queue_init(&r->rx_queue);
    spinlock_init(&r->lock);
    r->wait_thread = NULL;

    spinlock_lock(&g_raw_lock);
    r->next = g_raw_sockets;
    g_raw_sockets = r;
    spinlock_unlock(&g_raw_lock);
    return r;
}

void raw_socket_close(raw_sock_t *raw)
{
    if (!raw) return;
    spinlock_lock(&g_raw_lock);
    raw_sock_t **curr = &g_raw_sockets;
    while (*curr) {
        if (*curr == raw) {
            *curr = raw->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock(&g_raw_lock);

    spinlock_lock(&raw->lock);
    net_buf_queue_purge(&raw->rx_queue);
    if (raw->wait_thread) {
        sched_unblock(raw->wait_thread);
        raw->wait_thread = NULL;
    }
    spinlock_unlock(&raw->lock);
    kfree(raw);
}

void raw_input(net_buf_t *buf, const ipv4_hdr_t *ip)
{
    if (!buf || !ip) return;
    spinlock_lock(&g_raw_lock);
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
                    spinlock_lock(&curr->lock);
                    if (curr->wait_thread) {
                        sched_unblock(curr->wait_thread);
                        curr->wait_thread = NULL;
                    }
                    spinlock_unlock(&curr->lock);
                } else {
                    net_buf_free(clone);
                }
            }
        }
        curr = curr->next;
    }
    spinlock_unlock(&g_raw_lock);
}

static s64 sock_fop_read(struct file *filp, void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    socket_t *sock = (socket_t *)filp->private_data;
    bool nonblock = (filp->f_flags & O_NONBLOCK) != 0;

    if (sock->type == SOCK_STREAM && sock->tcp) {
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
            spinlock_lock(&sock->raw->lock);
            if (net_buf_queue_len(&sock->raw->rx_queue) == 0) {
                sock->raw->wait_thread = sched_current_thread();
                sched_block(THREAD_BLOCKED);
            }
            spinlock_unlock(&sock->raw->lock);
            sched_yield();
        }
    }
    return -EINVAL;
}

static s64 sock_fop_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)offset;
    if (!filp || !filp->private_data || !buf || len == 0) return 0;
    socket_t *sock = (socket_t *)filp->private_data;

    if (sock->type == SOCK_STREAM && sock->tcp) {
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

    if (sock->type == SOCK_STREAM && sock->tcp) {
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
        if (sock->type == SOCK_STREAM && sock->tcp) {
            bytes = (int)sock->tcp->rx_len;
        } else if (sock->type == SOCK_DGRAM && sock->udp) {
            bytes = (int)net_buf_queue_len(&sock->udp->rx_queue);
        } else if (sock->type == SOCK_RAW && sock->raw) {
            bytes = (int)net_buf_queue_len(&sock->raw->rx_queue);
        }
        if (copy_to_user((void *)(uintptr_t)arg, &bytes, sizeof(int)) != 0) return -(s64)EFAULT;
        return 0;
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

    if (sock->type == SOCK_STREAM && sock->tcp) {
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
    if (fd < 0 || fd >= 64 || !sock_out) return -EBADF;

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
