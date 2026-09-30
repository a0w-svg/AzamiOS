#pragma once

#include <stdint.h>

#define SIGRTMIN  32
#define SIGRTMAX  64

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

#define SA_RESTORER  0x04000000

#define SS_ONSTACK   1
#define SS_DISABLE   2

#define SS_AUTODISARM (1U << 31)
#define SS_FLAG_BITS SS_AUTODISARM

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        int _pad[28];
        struct {
            int _pid;
            unsigned int _uid;
        } _kill;
        struct {
            int _tid;
            int _overrun;
            union {
                int _pad[4];
                void *_sigval;
            } _sigval;
            int _sys_private;
        } _timer;
        struct {
            int _pid;
            unsigned int _uid;
            union {
                int _pad[4];
                void *_sigval;
            } _sigval;
        } _rt;
        struct {
            int _pid;
            unsigned int _uid;
            int _status;
            int _utime;
            int _stime;
        } _sigchld;
        struct {
            void *_addr;
            short _addr_lsb;
            union {
                struct {
                    void *_lower;
                    void *_upper;
                } _addr_bnd;
                unsigned int _pkey;
            };
        } _sigfault;
        struct {
            long _band;
            int _fd;
        } _sigpoll;
        struct {
            void *_call_addr;
            int _syscall;
            unsigned int _arch;
        } _sigsys;
    } _sifields;
} siginfo_t;
