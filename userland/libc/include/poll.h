/* ============================================================================
 * AzamiOS Userspace — Polling Header (poll.h)
 * File: userland/libc/include/poll.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"

typedef unsigned long nfds_t;

struct pollfd {
    int   fd;       /* File descriptor */
    short events;   /* Requested events */
    short revents;  /* Returned events */
};

/* Event types */
#define POLLIN      0x0001
#define POLLPRI     0x0002
#define POLLOUT     0x0004
#define POLLERR     0x0008
#define POLLHUP     0x0010
#define POLLNVAL    0x0020
#define POLLRDNORM  0x0040
#define POLLRDBAND  0x0080
#define POLLWRNORM  0x0100
#define POLLWRBAND  0x0200

int poll(struct pollfd *fds, nfds_t nfds, int timeout);
