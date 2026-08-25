/* ============================================================================
 * AzamiOS Userspace — I/O Event Notification Facility (sys/epoll.h)
 * File: userland/libc/include/sys/epoll.h
 * ============================================================================ */
#pragma once

#include "types.h"
#include <stdint.h>

enum EPOLL_EVENTS {
    EPOLLIN      = 0x0001,
    EPOLLPRI     = 0x0002,
    EPOLLOUT     = 0x0004,
    EPOLLRDNORM  = 0x0040,
    EPOLLRDBAND  = 0x0080,
    EPOLLWRNORM  = 0x0100,
    EPOLLWRBAND  = 0x0200,
    EPOLLMSG     = 0x0400,
    EPOLLERR     = 0x0008,
    EPOLLHUP     = 0x0010,
    EPOLLRDHUP   = 0x2000,
    EPOLLEXCLUSIVE = 1u << 28,
    EPOLLWAKEUP  = 1u << 29,
    EPOLLONESHOT = 1u << 30,
    EPOLLET      = 1u << 31
};

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_CLOEXEC  02000000
#define EPOLL_NONBLOCK 00004000

typedef union epoll_data {
    void    *ptr;
    int      fd;
    uint32_t u32;
    uint64_t u64;
} epoll_data_t;

struct epoll_event {
    uint32_t     events;
    epoll_data_t data;
} __attribute__((packed));

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
