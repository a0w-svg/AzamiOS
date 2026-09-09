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

/* Must stay field-for-field identical to userland/libc/include/sys/un.h's
 * struct sockaddr_un — same reasoning as struct linux_timex in
 * kernel/syscall/syscall.c: this is the layout copy_from_user() moves. */
#define UNIX_PATH_MAX 108
struct sockaddr_un {
    sa_family_t sun_family;
    char        sun_path[UNIX_PATH_MAX];
};

/* Ancillary-data (cmsghdr) cmsg_type this kernel actually backs: SCM_RIGHTS,
 * AF_UNIX fd-passing. Must match userland/libc/include/sys/socket.h's copy. */
#define SCM_RIGHTS 0x01

/* SCM_RIGHTS ancillary-data ceiling: how many fds one sendmsg() can hand
 * across an AF_UNIX socket in a single call. Matches this kernel's own
 * PROC_MAX_FDS-scale expectations rather than Linux's much higher
 * SCM_MAX_FD (253) — nothing in this codebase needs more per call, and a
 * fixed small array keeps unix_msg_t's allocation simple. */
#define UNIX_SCM_MAX_FDS 16

typedef struct raw_sock {
    int             protocol;
    net_buf_queue_t rx_queue;
    struct thread  *wait_thread;
    spinlock_t      lock;
    struct raw_sock *next;
} raw_sock_t;

/* ── AF_UNIX (kernel/net/unix_socket.c) ─────────────────────────────────────
 *
 * SOCK_STREAM: a connected pair is two pipe_t*s (fs/pipe.c), the same
 * plumbing socketpair(2) already builds via sockpair_create() — see
 * pipe_read_pipe()/pipe_write_pipe() in fs/pipe.h for why this reuses that
 * instead of a bespoke ring buffer. tx_pipe/rx_pipe are only valid once
 * state == UNIX_ST_CONNECTED.
 *
 * SOCK_DGRAM (and a SOCK_STREAM listener's pending-connection backlog): a
 * simple intrusive singly-linked queue, guarded by the same per-socket lock
 * as everything else here — there is no high-throughput requirement pushing
 * toward net_buf_queue_t's zero-copy design.
 */
typedef enum {
    UNIX_ST_UNBOUND = 0,
    UNIX_ST_BOUND,       /* has a path in the registry, not yet listening/connected */
    UNIX_ST_LISTENING,   /* SOCK_STREAM only */
    UNIX_ST_CONNECTED,   /* SOCK_STREAM only */
} unix_sock_state_t;

/* One SCM_RIGHTS-bearing datagram, or one pending SOCK_STREAM connection
 * request sitting in a listener's backlog (in which case `data`/`len` are
 * unused and `pending_tx`/`pending_rx` carry the pipe ends accept() hands to
 * the new connection — see unix_socket_connect()'s comment for why the pair
 * is built at connect() time rather than accept() time). */
typedef struct unix_msg {
    struct unix_msg *next;
    char             src_path[UNIX_PATH_MAX]; /* sender's bound path, or "" */
    int              nfds;
    file_t          *fds[UNIX_SCM_MAX_FDS];   /* refcounted; recvmsg() installs, closes on discard */
    struct pipe     *pending_tx;
    struct pipe     *pending_rx;
    size_t           len;
    u8               data[];
} unix_msg_t;

typedef struct unix_sock {
    int               type;   /* SOCK_STREAM or SOCK_DGRAM */
    unix_sock_state_t state;
    char              path[UNIX_PATH_MAX]; /* bound path; empty if unbound */

    /* SOCK_STREAM connected pair (see block comment above). */
    struct pipe      *tx_pipe;
    struct pipe      *rx_pipe;

    /* SOCK_STREAM listener backlog, or SOCK_DGRAM receive queue — a given
     * unix_sock_t is only ever one or the other, so one queue suffices. */
    unix_msg_t       *msg_head;
    unix_msg_t       *msg_tail;
    int               msg_count;
    int               backlog_max; /* SOCK_STREAM listener only */
    struct thread    *recv_wait;   /* blocked accept() (listener) or recvmsg() (dgram) */
    /* Set by unix_socket_recvmsg() when the message it just dequeued still
     * has SCM_RIGHTS fds pending pickup — see unix_socket_recvmsg_take_fds()
     * and unix_socket_recvmsg()'s own comment for why this two-step handoff
     * exists instead of a bigger recvmsg() signature. Single-slot because a
     * caller is expected to take the fds immediately, before any other call
     * on this same socket could dequeue a second message. */
    struct unix_msg  *last_recv_msg;

    spinlock_t        lock;
    struct unix_sock *registry_next; /* g_unix_registry link, when path[0] != 0 */
} unix_sock_t;

typedef struct socket {
    int         domain;
    int         type;
    int         protocol;
    union {
        tcp_sock_t  *tcp;
        udp_sock_t  *udp;
        raw_sock_t  *raw;
        unix_sock_t *uds;
    };
    int         so_reuseaddr;
    int         so_reuseport;
    int         so_broadcast;
    int         so_error;
    u32         so_rcvtimeo;
    u32         so_sndtimeo;
    file_t     *file;
} socket_t;

/* Public Socket API */
socket_t   *sock_alloc(int domain, int type, int protocol);
file_t     *sock_create_file(socket_t *sock);
void        sock_free(socket_t *sock);
int         sock_get_from_fd(int fd, socket_t **sock_out);
raw_sock_t *raw_socket_create(int protocol);
void        raw_socket_close(raw_sock_t *raw);
void        raw_input(net_buf_t *buf, const ipv4_hdr_t *ip);

/* AF_UNIX API (kernel/net/unix_socket.c) — mirrors the tcp_* and udp_*
 * shape elsewhere in this header so sys_*_impl in kernel/syscall/syscall.c
 * can dispatch on sock->domain == AF_UNIX the same way it already
 * dispatches on sock->type for tcp/udp. */
unix_sock_t *unix_socket_create(int type);
void         unix_socket_close(unix_sock_t *u);
int          unix_socket_bind(unix_sock_t *u, const char *path);
/* Returns 0 on success. `nonblock` matters only in that a full backlog on a
 * blocking caller waits for room rather than failing immediately. */
int          unix_socket_connect(unix_sock_t *client, const char *path, bool nonblock);
int          unix_socket_listen(unix_sock_t *u, int backlog);
/* Pops one pending connection into `child` (already allocated by the
 * caller, as sys_accept_impl's do_accept() does for tcp/udp children too).
 * Returns 0 on success, -EAGAIN if nonblock and none pending. */
int          unix_socket_accept(unix_sock_t *listener, unix_sock_t *child, bool nonblock);
/* SOCK_STREAM: writes to the connected pipe. SOCK_DGRAM: `dest_path` (NULL
 * for a connected/unbound send — not supported for dgram, matches real
 * AF_UNIX requiring an explicit destination per datagram) looks the target
 * up in the registry and queues a message. `fds`/`nfds` is SCM_RIGHTS —
 * pass nfds==0 for a plain send. Returns bytes written, or -errno. */
s64          unix_socket_sendmsg(unix_sock_t *u, const char *dest_path, const void *buf, size_t len,
                                  file_t **fds, int nfds, bool nonblock);
/* Returns bytes read, or -errno. `src_path_out` (may be NULL) gets the
 * sender's bound path for SOCK_DGRAM (empty string if the sender was
 * unbound). `fds_out`/`nfds_out` receive any SCM_RIGHTS fds the message
 * carried (already dup'd into *this* process's fd table by the time this
 * returns — see the implementation's comment on why that happens here
 * rather than being left to the caller). */
s64          unix_socket_recvmsg(unix_sock_t *u, void *buf, size_t len, char *src_path_out,
                                  int *nfds_out, bool nonblock);
/* Call immediately after unix_socket_recvmsg() when *nfds_out came back
 * non-zero: copies the received message's SCM_RIGHTS fds into fds_out
 * (truncating — and closing whatever didn't fit — at max_fds) and frees the
 * message. Returns how many fds were copied. A no-op (returns 0) if there
 * was nothing pending, so it's always safe to call. */
int          unix_socket_recvmsg_take_fds(unix_sock_t *u, file_t **fds_out, int max_fds);
int          unix_socket_poll(unix_sock_t *u);
