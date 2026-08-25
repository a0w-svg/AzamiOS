/* ============================================================================
 * AzamiOS — Loopback Block Device Driver (loop0..loop7)
 * File: drivers/block/loop.h
 * ============================================================================ */

#ifndef _AZAMI_DRIVERS_LOOP_H
#define _AZAMI_DRIVERS_LOOP_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <azami/defs.h>

#define LOOP_SET_FD         0x4C00
#define LOOP_CLR_FD         0x4C01
#define LOOP_SET_STATUS     0x4C02
#define LOOP_GET_STATUS     0x4C03
#define LOOP_SET_STATUS64   0x4C04
#define LOOP_GET_STATUS64   0x4C05
#define LOOP_CHANGE_FD      0x4C06
#define LOOP_SET_CAPACITY   0x4C07
#define LOOP_CTL_ADD        0x4C80
#define LOOP_CTL_REMOVE     0x4C81
#define LOOP_CTL_GET_FREE   0x4C82

#define LO_FLAGS_READ_ONLY  1
#define LO_FLAGS_AUTOCLEAR  4
#define LO_FLAGS_PARTSCAN    8

#define LO_NAME_SIZE        64
#define LO_KEY_SIZE         32

struct loop_info64 {
    u64 lo_device;
    u64 lo_inode;
    u64 lo_rdevice;
    u64 lo_offset;
    u64 lo_sizelimit;
    u32 lo_number;
    u32 lo_encrypt_type;
    u32 lo_encrypt_key_size;
    u32 lo_flags;
    u8  lo_file_name[LO_NAME_SIZE];
    u8  lo_crypt_name[LO_NAME_SIZE];
    u8  lo_encrypt_key[LO_KEY_SIZE];
    u64 lo_init[2];
};

void loop_init(void);

#endif /* _AZAMI_DRIVERS_LOOP_H */
