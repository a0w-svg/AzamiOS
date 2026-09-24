/* ============================================================================
 * AzamiOS — Linux AIO ABI definitions
 * File: include/azami/uapi/aio_abi.h
 *
 * This header defines the structures required for the Linux AIO syscalls.
 * ============================================================================ */
#pragma once

#include <stdint.h>

typedef uint64_t aio_context_t;

enum {
    IOCB_CMD_PREAD = 0,
    IOCB_CMD_PWRITE = 1,
    IOCB_CMD_FSYNC = 2,
    IOCB_CMD_FDSYNC = 3,
    IOCB_CMD_NOOP = 6,
    IOCB_CMD_PREADV = 7,
    IOCB_CMD_PWRITEV = 8,
};

struct io_event {
    uint64_t data;      /* the data field from the iocb */
    uint64_t obj;       /* what iocb this event came from */
    int64_t  res;       /* result code for this event */
    int64_t  res2;      /* secondary result */
};

struct iocb {
    uint64_t aio_data;  /* data to be returned in event's data */
    uint32_t aio_key;   /* the kernel sets aio_key to the req # */
    uint32_t aio_reserved1;
    uint16_t aio_lio_opcode;
    int16_t  aio_reqprio;
    uint32_t aio_fildes;
    uint64_t aio_buf;
    uint64_t aio_nbytes;
    int64_t  aio_offset;
    uint64_t aio_reserved2;
    uint32_t aio_flags;
    uint32_t aio_resfd;
};

struct aio_ring {
    uint32_t id;        /* kernel internal index number */
    uint32_t nr;        /* number of io_events */
    uint32_t head;      /* Written to by userland or under ring_lock */
    uint32_t tail;
    uint32_t magic;
    uint32_t compat_features;
    uint32_t incompat_features;
    uint32_t header_length; /* size of aio_ring */
    struct io_event io_events[0];
};

#define AIO_RING_MAGIC 0xa10a10a1
