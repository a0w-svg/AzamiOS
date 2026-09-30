#pragma once

#include <stdint.h>

#define LANDLOCK_ACCESS_FS_EXECUTE          (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE       (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE        (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR         (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR       (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE      (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR        (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR         (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG         (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK        (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO        (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK       (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM         (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER            (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE         (1ULL << 14)

#define LANDLOCK_ACCESS_NET_BIND_TCP        (1ULL << 0)
#define LANDLOCK_ACCESS_NET_CONNECT_TCP     (1ULL << 1)

struct landlock_ruleset_attr {
    uint64_t handled_access_fs;
    uint64_t handled_access_net;
};

struct landlock_path_beneath_attr {
    uint64_t allowed_access;
    int32_t parent_fd;
};

struct landlock_net_port_attr {
    uint64_t allowed_access;
    uint64_t port;
};
