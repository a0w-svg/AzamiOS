/* ============================================================================
 * AzamiOS — AF_UNIX Domain Sockets
 * File: kernel/net/unix_socket.c
 *
 * Real local-domain sockets, replacing sock_alloc()'s previous behavior of
 * silently routing any AF_UNIX socket() call to the TCP or UDP backend
 * based on `type` alone (an AF_UNIX SOCK_STREAM request got a real TCP
 * socket, which happens to work for byte-stream semantics but can never
 * carry SCM_RIGHTS, has no bind()-to-a-path addressing, and reports the
 * wrong domain to getsockname()).
 *
 * SOCK_STREAM connections reuse fs/pipe.c's byte-stream buffering — a
 * connected pair is just two pipe_t*s, the same plumbing socketpair(2)
 * already builds via sockpair_create() — via the pipe_read_pipe()/
 * pipe_write_pipe() entry points pipe.h exports for exactly this. SOCK_DGRAM
 * (and a SOCK_STREAM listener's backlog) use a small intrusive queue.
 *
 * SCM_RIGHTS fd-passing is implemented for SOCK_DGRAM only: a message
 * naturally carries its own fds[]/nfds alongside its payload in this design,
 * so there's nowhere else the reference needs to live. SOCK_STREAM's
 * sendmsg()/recvmsg() get real multi-iovec gather/scatter (see
 * kernel/syscall/syscall.c's sys_sendmsg_impl/sys_recvmsg_impl) but return
 * -EOPNOTSUPP for a non-empty SCM_RIGHTS list — attaching ancillary data to
 * an arbitrary byte offset in a live byte stream needs a side-channel
 * threaded through pipe_t itself, which pipe.c's shared, plain-pipe(2)-used
 * structure doesn't carry and this change doesn't touch. This is a
 * deliberate, scoped-down piece of the feature, not an oversight.
 * ============================================================================ */

#include "../../include/azami/socket.h"
#include "../../include/azami/defs.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/lib/string.h"
#include "../../fs/pipe.h"

/* ── Path registry: every bound (or listening) unix_sock_t, by path ────── */
static unix_sock_t *g_unix_registry = NULL;
static spinlock_t   g_unix_registry_lock = SPINLOCK_INIT;

static unix_sock_t *unix_registry_find_locked(const char *path)
{
    for (unix_sock_t *u = g_unix_registry; u; u = u->registry_next) {
        if (strcmp(u->path, path) == 0) return u;
    }
    return NULL;
}

unix_sock_t *unix_socket_create(int type)
{
    unix_sock_t *u = (unix_sock_t *)kzalloc(sizeof(unix_sock_t));
    if (!u) return NULL;
    u->type = type;
    u->state = UNIX_ST_UNBOUND;
    spinlock_init(&u->lock);
    return u;
}

/* Frees every message on a discarded queue (a listener's unpicked-up backlog,
 * or a dgram socket's unread mailbox), releasing any SCM_RIGHTS fds and
 * closing any pending connection's pipe ends nobody will ever accept(). */
static void unix_msg_queue_purge(unix_msg_t *head)
{
    while (head) {
        unix_msg_t *next = head->next;
        for (int i = 0; i < head->nfds; i++) {
            if (head->fds[i]) vfs_close(head->fds[i]);
        }
        if (head->pending_tx) pipe_close_end(head->pending_tx, false);
        if (head->pending_rx) pipe_close_end(head->pending_rx, true);
        kfree(head);
        head = next;
    }
}

void unix_socket_close(unix_sock_t *u)
{
    if (!u) return;

    if (u->path[0]) {
        spinlock_lock(&g_unix_registry_lock);
        unix_sock_t **pp = &g_unix_registry;
        while (*pp) {
            if (*pp == u) { *pp = u->registry_next; break; }
            pp = &(*pp)->registry_next;
        }
        spinlock_unlock(&g_unix_registry_lock);
    }

    spinlock_lock(&u->lock);
    unix_msg_t *orphaned = u->msg_head;
    u->msg_head = u->msg_tail = NULL;
    u->msg_count = 0;
    /* A blocked accept()/recvmsg() must wake up and see the closed state,
     * not spin forever waiting on a queue nothing will ever fill again. */
    thread_t *waiter = u->recv_wait;
    u->recv_wait = NULL;
    struct pipe *tx = u->tx_pipe, *rx = u->rx_pipe;
    u->tx_pipe = u->rx_pipe = NULL;
    spinlock_unlock(&u->lock);

    if (waiter) sched_unblock(waiter);
    unix_msg_queue_purge(orphaned);
    if (tx) pipe_close_end(tx, false);
    if (rx) pipe_close_end(rx, true);

    kfree(u);
}

int unix_socket_bind(unix_sock_t *u, const char *path)
{
    if (!u || !path || !path[0]) return -EINVAL;
    if (strlen(path) >= UNIX_PATH_MAX) return -ENAMETOOLONG;
    if (u->state != UNIX_ST_UNBOUND) return -EINVAL;

    spinlock_lock(&g_unix_registry_lock);
    if (unix_registry_find_locked(path)) {
        spinlock_unlock(&g_unix_registry_lock);
        return -EADDRINUSE;
    }
    strncpy(u->path, path, UNIX_PATH_MAX - 1);
    u->path[UNIX_PATH_MAX - 1] = '\0';
    u->registry_next = g_unix_registry;
    g_unix_registry = u;
    spinlock_unlock(&g_unix_registry_lock);

    u->state = UNIX_ST_BOUND;
    return 0;
}

int unix_socket_listen(unix_sock_t *u, int backlog)
{
    if (!u) return -EINVAL;
    if (u->type != SOCK_STREAM) return -EOPNOTSUPP;
    if (u->state != UNIX_ST_BOUND) return -EINVAL; /* must bind() first: no autobind */
    if (backlog < 1) backlog = 1;
    if (backlog > 128) backlog = 128;
    u->backlog_max = backlog;
    u->state = UNIX_ST_LISTENING;
    return 0;
}

int unix_socket_connect(unix_sock_t *client, const char *path, bool nonblock)
{
    if (!client || !path || !path[0]) return -EINVAL;
    if (client->type != SOCK_STREAM) return -EOPNOTSUPP; /* dgram "connect" not modeled */
    if (client->state == UNIX_ST_CONNECTED) return -EISCONN;

    spinlock_lock(&g_unix_registry_lock);
    unix_sock_t *listener = unix_registry_find_locked(path);
    spinlock_unlock(&g_unix_registry_lock);
    if (!listener || listener->type != SOCK_STREAM) return -ECONNREFUSED;

    spinlock_lock(&listener->lock);
    if (listener->state != UNIX_ST_LISTENING) {
        spinlock_unlock(&listener->lock);
        return -ECONNREFUSED;
    }
    if (listener->msg_count >= listener->backlog_max) {
        spinlock_unlock(&listener->lock);
        /* A full backlog on a real AF_UNIX listener is a connection refusal,
         * not something connect() retries — matches accept() being expected
         * to drain the backlog promptly rather than this side waiting. */
        return -ECONNREFUSED;
    }
    spinlock_unlock(&listener->lock);
    (void)nonblock; /* nothing left to block on: the backlog had room */

    /* Build the bidirectional pipe pair up front — the exact same
     * ep1/ep2 wiring fs/pipe.c's sockpair_create() uses (see its comment):
     * pipe #1 carries client->accepted, pipe #2 carries accepted->client. */
    file_t *r1 = NULL, *w1 = NULL, *r2 = NULL, *w2 = NULL;
    int err = pipe_create(&r1, &w1);
    if (err < 0) return err;
    err = pipe_create(&r2, &w2);
    if (err < 0) {
        vfs_close(r1); vfs_close(w1);
        return err;
    }

    unix_msg_t *pending = (unix_msg_t *)kzalloc(sizeof(unix_msg_t));
    if (!pending) {
        vfs_close(r1); vfs_close(w1);
        vfs_close(r2); vfs_close(w2);
        return -ENOMEM;
    }
    pending->pending_tx = (struct pipe *)w2->private_data; /* accepted side writes here */
    pending->pending_rx = (struct pipe *)r1->private_data; /* accepted side reads here */

    client->tx_pipe = (struct pipe *)w1->private_data;
    client->rx_pipe = (struct pipe *)r2->private_data;

    /* The two ends' file_t wrappers were only a vehicle for pipe_create()'s
     * API — same cleanup sockpair_create() does once it has pulled the
     * pipe_t*s out of ->private_data. */
    kfree(r1); kfree(w1); kfree(r2); kfree(w2);

    bool need_wake;
    spinlock_lock(&listener->lock);
    if (listener->msg_tail) listener->msg_tail->next = pending;
    else listener->msg_head = pending;
    listener->msg_tail = pending;
    listener->msg_count++;
    need_wake = (listener->recv_wait != NULL);
    thread_t *waiter = listener->recv_wait;
    listener->recv_wait = NULL;
    spinlock_unlock(&listener->lock);
    if (need_wake) sched_unblock(waiter);

    client->state = UNIX_ST_CONNECTED;
    return 0;
}

int unix_socket_accept(unix_sock_t *listener, unix_sock_t *child, bool nonblock)
{
    if (!listener || !child) return -EINVAL;
    if (listener->type != SOCK_STREAM || listener->state != UNIX_ST_LISTENING) return -EINVAL;

    for (;;) {
        spinlock_lock(&listener->lock);
        if (listener->msg_head) break;
        if (listener->state != UNIX_ST_LISTENING) {
            /* Closed out from under a blocked accept() (unix_socket_close()). */
            spinlock_unlock(&listener->lock);
            return -EINVAL;
        }
        if (nonblock) {
            spinlock_unlock(&listener->lock);
            return -EAGAIN;
        }
        listener->recv_wait = sched_current_thread();
        spinlock_unlock(&listener->lock);
        sched_block(THREAD_BLOCKED_PENDING);
        /* Woken by a new connection (unix_socket_connect) or by
         * unix_socket_close() tearing the listener down; loop and recheck
         * either way rather than assuming what woke us. */
    }

    unix_msg_t *pending = listener->msg_head;
    listener->msg_head = pending->next;
    if (!listener->msg_head) listener->msg_tail = NULL;
    listener->msg_count--;
    spinlock_unlock(&listener->lock);

    child->type = SOCK_STREAM;
    child->tx_pipe = pending->pending_tx;
    child->rx_pipe = pending->pending_rx;
    child->state = UNIX_ST_CONNECTED;
    kfree(pending);
    return 0;
}

s64 unix_socket_sendmsg(unix_sock_t *u, const char *dest_path, const void *buf, size_t len,
                         file_t **fds, int nfds, bool nonblock)
{
    if (!u) return -EINVAL;
    if (nfds < 0 || nfds > UNIX_SCM_MAX_FDS) return -EINVAL;

    if (u->type == SOCK_STREAM) {
        if (nfds > 0) return -EOPNOTSUPP; /* see this file's top-of-file comment */
        if (u->state != UNIX_ST_CONNECTED || !u->tx_pipe) return -ENOTCONN;
        return pipe_write_pipe(u->tx_pipe, buf, len, nonblock);
    }

    /* SOCK_DGRAM */
    if (!dest_path || !dest_path[0]) return -EDESTADDRREQ;
    spinlock_lock(&g_unix_registry_lock);
    unix_sock_t *target = unix_registry_find_locked(dest_path);
    spinlock_unlock(&g_unix_registry_lock);
    if (!target || target->type != SOCK_DGRAM) return -ECONNREFUSED;

    unix_msg_t *msg = (unix_msg_t *)kmalloc(sizeof(unix_msg_t) + len);
    if (!msg) return -ENOMEM;
    memset(msg, 0, sizeof(unix_msg_t));
    if (len > 0) memcpy(msg->data, buf, len);
    msg->len = len;
    strncpy(msg->src_path, u->path, UNIX_PATH_MAX - 1);
    msg->nfds = nfds;
    for (int i = 0; i < nfds; i++) msg->fds[i] = fds[i];

    bool need_wake;
    spinlock_lock(&target->lock);
    if (target->msg_tail) target->msg_tail->next = msg;
    else target->msg_head = msg;
    target->msg_tail = msg;
    target->msg_count++;
    need_wake = (target->recv_wait != NULL);
    thread_t *waiter = target->recv_wait;
    target->recv_wait = NULL;
    spinlock_unlock(&target->lock);
    if (need_wake) sched_unblock(waiter);

    return (s64)len;
}

s64 unix_socket_recvmsg(unix_sock_t *u, void *buf, size_t len, char *src_path_out,
                         int *nfds_out, bool nonblock)
{
    if (!u) return -EINVAL;
    if (nfds_out) *nfds_out = 0;
    if (src_path_out) src_path_out[0] = '\0';

    if (u->type == SOCK_STREAM) {
        if (u->state != UNIX_ST_CONNECTED || !u->rx_pipe) return -ENOTCONN;
        return pipe_read_pipe(u->rx_pipe, buf, len, nonblock);
    }

    /* SOCK_DGRAM */
    for (;;) {
        spinlock_lock(&u->lock);
        if (u->msg_head) break;
        if (nonblock) {
            spinlock_unlock(&u->lock);
            return -EAGAIN;
        }
        u->recv_wait = sched_current_thread();
        spinlock_unlock(&u->lock);
        sched_block(THREAD_BLOCKED_PENDING);
    }

    unix_msg_t *msg = u->msg_head;
    u->msg_head = msg->next;
    if (!u->msg_head) u->msg_tail = NULL;
    u->msg_count--;
    spinlock_unlock(&u->lock);

    size_t clen = (msg->len < len) ? msg->len : len;
    memcpy(buf, msg->data, clen);
    if (src_path_out) strncpy(src_path_out, msg->src_path, UNIX_PATH_MAX - 1);

    if (nfds_out) *nfds_out = msg->nfds;

    /* A message with SCM_RIGHTS fds stays alive (u->last_recv_msg) until the
     * caller retrieves them via unix_socket_recvmsg_take_fds(), which frees
     * it; a plain message (the overwhelmingly common case) is freed right
     * here instead of forcing every caller through a two-step handoff. */
    if (msg->nfds == 0) {
        kfree(msg);
    } else {
        u->last_recv_msg = msg;
    }

    return (s64)clen;
}

/* Called immediately after unix_socket_recvmsg() when *nfds_out came back
 * non-zero: copies the pending message's fds into the caller's array and
 * frees the message. Kept as a separate step (rather than an extra
 * file_t***-style out-parameter on unix_socket_recvmsg() itself) so the
 * common no-SCM_RIGHTS path — every plain recv()/read() on a dgram socket —
 * doesn't need to pass an unused array through every call. */
int unix_socket_recvmsg_take_fds(unix_sock_t *u, file_t **fds_out, int max_fds)
{
    if (!u || !u->last_recv_msg) return 0;
    unix_msg_t *msg = u->last_recv_msg;
    u->last_recv_msg = NULL;
    int n = msg->nfds;
    if (n > max_fds) n = max_fds;
    for (int i = 0; i < n; i++) fds_out[i] = msg->fds[i];
    /* Any fds beyond max_fds are dropped (closed), matching Linux's own
     * MSG_CTRUNC behavior for a control buffer too small for what arrived. */
    for (int i = n; i < msg->nfds; i++) {
        if (msg->fds[i]) vfs_close(msg->fds[i]);
    }
    kfree(msg);
    return n;
}

int unix_socket_poll(unix_sock_t *u)
{
    if (!u) return 0;
    int mask = 0;
    if (u->type == SOCK_STREAM) {
        if (u->state != UNIX_ST_CONNECTED) return 0;
        if (u->rx_pipe && u->rx_pipe->count > 0) mask |= 0x0001 /* POLLIN */;
        if (u->rx_pipe && u->rx_pipe->writers == 0) mask |= 0x0010 /* POLLHUP */ | 0x0001;
        if (u->tx_pipe && (u->tx_pipe->readers == 0 || u->tx_pipe->count < PIPE_BUFFER_SIZE)) mask |= 0x0004 /* POLLOUT */;
    } else {
        spinlock_lock(&u->lock);
        if (u->msg_head) mask |= 0x0001;
        spinlock_unlock(&u->lock);
        mask |= 0x0004; /* dgram send never blocks in this implementation */
    }
    return mask;
}
