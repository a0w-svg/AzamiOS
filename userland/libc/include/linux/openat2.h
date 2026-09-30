/* Linux-compatible openat2(2) UAPI. */
#pragma once

#include <stdint.h>

struct open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

/* Path-resolution constraints. AzamiOS currently rejects non-zero resolve
 * values instead of silently weakening the requested confinement. */
#define RESOLVE_NO_XDEV       0x01ULL
#define RESOLVE_NO_MAGICLINKS 0x02ULL
#define RESOLVE_NO_SYMLINKS   0x04ULL
#define RESOLVE_BENEATH       0x08ULL
#define RESOLVE_IN_ROOT       0x10ULL
#define RESOLVE_CACHED        0x20ULL
