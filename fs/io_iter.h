#pragma once
#include "io_vec.h"
#include "../kernel/uaccess.h"

/* offset is committed by the caller only after its backend transaction succeeds.
 * User faults can allocate/block, so neither helper may run under a spinlock. */
static inline size_t io_iter_copy_to_user(const vfs_iovec_t *iov, u32 count,
                                          size_t offset, const void *src, size_t len)
{
    size_t copied = 0;
    for (u32 i = 0; i < count && copied < len; i++) {
        if (offset >= iov[i].iov_len) { offset -= iov[i].iov_len; continue; }
        size_t n = iov[i].iov_len - offset;
        if (n > len - copied) n = len - copied;
        size_t left = copy_to_user((void *)((uintptr_t)iov[i].iov_base + offset),
                                  (const u8 *)src + copied, n);
        copied += n - left;
        if (left) break;
        offset = 0;
    }
    return copied;
}

static inline size_t io_iter_copy_from_user(const vfs_iovec_t *iov, u32 count,
                                            size_t offset, void *dst, size_t len)
{
    size_t copied = 0;
    for (u32 i = 0; i < count && copied < len; i++) {
        if (offset >= iov[i].iov_len) { offset -= iov[i].iov_len; continue; }
        size_t n = iov[i].iov_len - offset;
        if (n > len - copied) n = len - copied;
        size_t left = copy_from_user((u8 *)dst + copied,
                                    (void *)((uintptr_t)iov[i].iov_base + offset), n);
        copied += n - left;
        if (left) break;
        offset = 0;
    }
    return copied;
}
