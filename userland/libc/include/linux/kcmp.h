#pragma once

enum kcmp_type {
    KCMP_FILE,
    KCMP_VM,
    KCMP_FILES,
    KCMP_FS,
    KCMP_SIGHAND,
    KCMP_IO,
    KCMP_SYSVSEM,
    KCMP_EPOLL_TFD,
};

struct kcmp_epoll_slot {
    uint32_t efd;
    uint32_t tfd;
    uint32_t toff;
};
