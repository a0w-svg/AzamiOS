#pragma once

#include <stdint.h>

#define IORING_SETUP_IOPOLL (1U << 0)
#define IORING_SETUP_SQPOLL (1U << 1)
#define IORING_SETUP_SQ_AFF (1U << 2)
#define IORING_SETUP_CQSIZE (1U << 3)
#define IORING_SETUP_CLAMP  (1U << 4)
#define IORING_SETUP_ATTACH_WQ (1U << 5)
#define IORING_SETUP_R_DISABLED (1U << 6)
#define IORING_SETUP_SUBMIT_ALL (1U << 7)

#define IORING_OP_NOP           0
#define IORING_OP_READV         1
#define IORING_OP_WRITEV        2
#define IORING_OP_FSYNC         3
#define IORING_OP_READ_FIXED    4
#define IORING_OP_WRITE_FIXED   5
#define IORING_OP_POLL_ADD      6
#define IORING_OP_POLL_REMOVE   7
#define IORING_OP_SYNC_FILE_RANGE 8
#define IORING_OP_SENDMSG       9
#define IORING_OP_RECVMSG       10
#define IORING_OP_TIMEOUT       11
#define IORING_OP_TIMEOUT_REMOVE 12
#define IORING_OP_ACCEPT        13
#define IORING_OP_ASYNC_CANCEL  14
#define IORING_OP_LINK_TIMEOUT  15
#define IORING_OP_CONNECT       16
#define IORING_OP_FALLOCATE     17
#define IORING_OP_OPENAT        18
#define IORING_OP_CLOSE         19
#define IORING_OP_FILES_UPDATE  20
#define IORING_OP_STATX         21
#define IORING_OP_READ          22
#define IORING_OP_WRITE         23
#define IORING_OP_FADVISE       24
#define IORING_OP_MADVISE       25
#define IORING_OP_SEND          26
#define IORING_OP_RECV          27
#define IORING_OP_OPENAT2       28
#define IORING_OP_EPOLL_CTL     29
#define IORING_OP_SPLICE        30
#define IORING_OP_PROVIDE_BUFFERS 31
#define IORING_OP_REMOVE_BUFFERS 32
#define IORING_OP_TEE           33
#define IORING_OP_SHUTDOWN      34
#define IORING_OP_RENAMEAT      35
#define IORING_OP_UNLINKAT      36
#define IORING_OP_MKDIRAT       37
#define IORING_OP_SYMLINKAT     38
#define IORING_OP_LINKAT        39

#define IORING_ENTER_GETEVENTS      (1U << 0)
#define IORING_ENTER_SQ_WAKEUP      (1U << 1)
#define IORING_ENTER_SQ_WAIT        (1U << 2)
#define IORING_ENTER_EXT_ARG        (1U << 3)
#define IORING_ENTER_REGISTERED_RING (1U << 4)

struct io_uring_sqe {
    uint8_t opcode;
    uint8_t flags;
    uint16_t ioprio;
    int32_t fd;
    union {
        uint64_t off;
        uint64_t addr2;
    };
    union {
        uint64_t addr;
        uint64_t splice_off_in;
    };
    uint32_t len;
    union {
        int32_t rw_flags;
        uint32_t fsync_flags;
        uint16_t poll_events;
        uint32_t poll32_events;
        uint32_t sync_range_flags;
        uint32_t msg_flags;
        uint32_t timeout_flags;
        uint32_t accept_flags;
        uint32_t cancel_flags;
        uint32_t open_flags;
        uint32_t statx_flags;
        uint32_t fadvise_advice;
        uint32_t splice_flags;
        uint32_t rename_flags;
        uint32_t unlink_flags;
        uint32_t hardlink_flags;
    };
    uint64_t user_data;
    union {
        struct {
            union {
                uint16_t buf_index;
                uint16_t buf_group;
            } __attribute__((packed));
            uint16_t personality;
            int32_t splice_fd_in;
        };
        uint64_t __pad2[3];
    };
};

struct io_uring_cqe {
    uint64_t user_data;
    int32_t res;
    uint32_t flags;
};

struct io_sqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t flags;
    uint32_t dropped;
    uint32_t array;
    uint32_t resv1;
    uint64_t resv2;
};

struct io_cqring_offsets {
    uint32_t head;
    uint32_t tail;
    uint32_t ring_mask;
    uint32_t ring_entries;
    uint32_t overflow;
    uint32_t cqes;
    uint32_t flags;
    uint32_t resv1;
    uint64_t resv2;
};

struct io_uring_params {
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t flags;
    uint32_t sq_thread_cpu;
    uint32_t sq_thread_idle;
    uint32_t features;
    uint32_t wq_fd;
    uint32_t resv[3];
    struct io_sqring_offsets sq_off;
    struct io_cqring_offsets cq_off;
};
