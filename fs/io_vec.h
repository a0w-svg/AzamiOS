#pragma once
#include "../include/azami/types.h"

/* ABI-compatible imported descriptor. Storage belongs to the kernel; bases
 * remain user addresses until copied by a user-aware backend. */
struct iovec {
    void *iov_base;
    size_t iov_len;
};
typedef struct iovec vfs_iovec_t;
