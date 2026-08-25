/* ============================================================================
 * AzamiOS Userspace — Endian Conversion Macros (endian.h)
 * File: userland/libc/include/endian.h
 * ============================================================================ */
#pragma once

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __PDP_ENDIAN    3412

#define __BYTE_ORDER    __LITTLE_ENDIAN

#define LITTLE_ENDIAN   __LITTLE_ENDIAN
#define BIG_ENDIAN      __BIG_ENDIAN
#define PDP_ENDIAN      __PDP_ENDIAN
#define BYTE_ORDER      __BYTE_ORDER

#include "byteswap.h"

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define htobe16(x) __bswap_16(x)
#define htole16(x) ((uint16_t)(x))
#define be16toh(x) __bswap_16(x)
#define le16toh(x) ((uint16_t)(x))

#define htobe32(x) __bswap_32(x)
#define htole32(x) ((uint32_t)(x))
#define be32toh(x) __bswap_32(x)
#define le32toh(x) ((uint32_t)(x))

#define htobe64(x) __bswap_64(x)
#define htole64(x) ((uint64_t)(x))
#define be64toh(x) __bswap_64(x)
#define le64toh(x) ((uint64_t)(x))
#else
#define htobe16(x) ((uint16_t)(x))
#define htole16(x) __bswap_16(x)
#define be16toh(x) ((uint16_t)(x))
#define le16toh(x) __bswap_16(x)

#define htobe32(x) ((uint32_t)(x))
#define htole32(x) __bswap_32(x)
#define be32toh(x) ((uint32_t)(x))
#define le32toh(x) __bswap_32(x)

#define htobe64(x) ((uint64_t)(x))
#define htole64(x) __bswap_64(x)
#define be64toh(x) ((uint64_t)(x))
#define le64toh(x) __bswap_64(x)
#endif
