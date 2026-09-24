/* ============================================================================
 * AzamiOS — Linux Userfaultfd ABI definitions
 * File: include/azami/uapi/userfaultfd.h
 *
 * This header defines the structures and ioctls required for userfaultfd.
 * ============================================================================ */
#pragma once

#include "../types.h"

#define UFFD_API ((u64)0xAA)
#define UFFD_API_FEATURES 0

#define UFFDIO_REGISTER 0xAA01
#define UFFDIO_UNREGISTER 0xAA02
#define UFFDIO_API 0xAA03
#define UFFDIO_COPY 0xAA04
#define UFFDIO_ZEROPAGE 0xAA05

struct uffdio_api {
    u64 api;
    u64 features;
    u64 ioctls;
};

struct uffdio_register {
    struct {
        u64 start;
        u64 len;
    } range;
    u64 mode;
    u64 ioctls;
};

struct uffdio_copy {
    u64 dst;
    u64 src;
    u64 len;
    u64 mode;
    s64 copy;
};

struct uffd_msg {
    u8 event;
    u8 reserved1;
    u16 reserved2;
    u32 reserved3;
    union {
        struct {
            u64 flags;
            u64 address;
            union {
                u32 ptid;
            } feat;
        } pagefault;
    } arg;
};

#define UFFD_EVENT_PAGEFAULT 0x12
