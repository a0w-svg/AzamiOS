/* ============================================================================
 * AzamiOS Userspace — Synchronous I/O Multiplexing (sys/select.h)
 * File: userland/libc/include/sys/select.h
 * ============================================================================ */
#pragma once

#include "types.h"
#include "time.h"
#include "../signal.h"

#define FD_SETSIZE 1024

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set) \
    do { \
        unsigned int __i; \
        for (__i = 0; __i < sizeof((set)->fds_bits) / sizeof(unsigned long); __i++) \
            (set)->fds_bits[__i] = 0UL; \
    } while (0)

#define FD_SET(fd, set) \
    do { \
        if ((unsigned int)(fd) < FD_SETSIZE) \
            (set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] |= (1UL << ((fd) % (8 * sizeof(unsigned long)))); \
    } while (0)

#define FD_CLR(fd, set) \
    do { \
        if ((unsigned int)(fd) < FD_SETSIZE) \
            (set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] &= ~(1UL << ((fd) % (8 * sizeof(unsigned long)))); \
    } while (0)

#define FD_ISSET(fd, set) \
    (((unsigned int)(fd) < FD_SETSIZE) ? \
        !!((set)->fds_bits[(fd) / (8 * sizeof(unsigned long))] & (1UL << ((fd) % (8 * sizeof(unsigned long))))) : 0)

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask);
