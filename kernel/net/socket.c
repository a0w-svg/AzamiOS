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

static file_operations_t g_socket_fops = {
    .read = sock_fop_read,
    .write = sock_fop_write,
    .readdir = NULL,
    .ioctl = NULL,
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
