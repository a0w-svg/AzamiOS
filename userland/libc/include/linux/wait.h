#pragma once

#define WNOHANG     0x00000001
#define WUNTRACED   0x00000002
#define WSTOPPED    WUNTRACED
#define WEXITED     0x00000004
#define WCONTINUED  0x00000008
#define WNOWAIT     0x01000000

#define __WNOTHREAD 0x20000000
#define __WALL      0x40000000
#define __WCLONE    0x80000000

typedef enum {
    P_ALL,
    P_PID,
    P_PGID,
    P_PIDFD
} idtype_t;
