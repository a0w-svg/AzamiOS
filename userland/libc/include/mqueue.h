/* ============================================================================
 * AzamiOS Userspace — POSIX Message Queues (mqueue.h)
 * File: userland/libc/include/mqueue.h
 *
 * POSIX.1-2008, the Message Passing option. A queue is named "/name", holds
 * prioritised messages, and is addressed through an mqd_t — which on this
 * system, as on Linux, is a file descriptor, so poll(), select() and epoll()
 * work on it and close() is a valid (if non-portable) way to close it.
 *
 * Typical use:
 *
 *     struct mq_attr a = { .mq_maxmsg = 16, .mq_msgsize = 256 };
 *     mqd_t q = mq_open("/jobs", O_CREAT | O_RDWR, 0600, &a);
 *     mq_send(q, buf, len, 5);
 *     char in[256]; unsigned prio;
 *     mq_receive(q, in, sizeof in, &prio);
 *     mq_close(q);
 *     mq_unlink("/jobs");
 *
 * The receive buffer must be at least mq_msgsize bytes: a message is never
 * delivered truncated, so a short buffer is an EMSGSIZE error.
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "fcntl.h"
#include "time.h"
#include "signal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int mqd_t;

struct mq_attr {
    long mq_flags;     /* 0 or O_NONBLOCK (per descriptor)          */
    long mq_maxmsg;    /* messages the queue can hold               */
    long mq_msgsize;   /* largest message, in bytes                 */
    long mq_curmsgs;   /* messages currently queued                 */
    long __reserved[4];
};

#define MQ_PRIO_MAX 32768

/* mq_open() takes (name, oflag) or (name, oflag, mode, attr); the four-argument
 * form is required whenever O_CREAT is set. Declared variadic so both call
 * shapes compile, exactly as POSIX specifies it. */
mqd_t mq_open(const char *name, int oflag, ...);
int   mq_close(mqd_t mqdes);
int   mq_unlink(const char *name);

int   mq_getattr(mqd_t mqdes, struct mq_attr *attr);
int   mq_setattr(mqd_t mqdes, const struct mq_attr *newattr,
                 struct mq_attr *oldattr);

int     mq_send(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
                unsigned int msg_prio);
ssize_t mq_receive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                   unsigned int *msg_prio);

int     mq_timedsend(mqd_t mqdes, const char *msg_ptr, size_t msg_len,
                     unsigned int msg_prio,
                     const struct timespec *abs_timeout);
ssize_t mq_timedreceive(mqd_t mqdes, char *msg_ptr, size_t msg_len,
                        unsigned int *msg_prio,
                        const struct timespec *abs_timeout);

int   mq_notify(mqd_t mqdes, const struct sigevent *sevp);

#ifdef __cplusplus
}
#endif
