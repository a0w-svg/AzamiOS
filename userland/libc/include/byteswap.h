/* ============================================================================
 * AzamiOS Userspace — Byte Swapping Functions (byteswap.h)
 * File: userland/libc/include/byteswap.h
 * ============================================================================ */
#pragma once

#include <stdint.h>

#define __bswap_16(x) __builtin_bswap16((uint16_t)(x))
#define __bswap_32(x) __builtin_bswap32((uint32_t)(x))
#define __bswap_64(x) __builtin_bswap64((uint64_t)(x))

#define bswap_16(x) __bswap_16(x)
#define bswap_32(x) __bswap_32(x)
#define bswap_64(x) __bswap_64(x)
