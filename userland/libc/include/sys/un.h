/* ============================================================================
 * AzamiOS Userspace — UNIX Domain Sockets (sys/un.h)
 * File: userland/libc/include/sys/un.h
 * ============================================================================ */
#pragma once

#include "types.h"
#include "socket.h"

struct sockaddr_un {
    sa_family_t sun_family;     /* AF_UNIX */
    char        sun_path[108];  /* Socket pathname */
};

#define SUN_LEN(ptr) ((size_t)(((struct sockaddr_un *)0)->sun_path) + strlen((ptr)->sun_path))
