/* ============================================================================
 * AzamiOS Userspace — Process Tracing Header (sys/ptrace.h)
 * File: userland/libc/include/sys/ptrace.h
 * ============================================================================ */
#pragma once

#include <sys/types.h>
#include <sys/user.h>

enum __ptrace_request {
    PTRACE_TRACEME       = 0,
    PTRACE_PEEKTEXT      = 1,
    PTRACE_PEEKDATA      = 2,
    PTRACE_PEEKUSER      = 3,
    PTRACE_POKETEXT      = 4,
    PTRACE_POKEDATA      = 5,
    PTRACE_POKEUSER      = 6,
    PTRACE_CONT          = 7,
    PTRACE_KILL          = 8,
    PTRACE_SINGLESTEP    = 9,
    PTRACE_GETREGS       = 12,
    PTRACE_SETREGS       = 13,
    PTRACE_GETFPREGS     = 14,
    PTRACE_SETFPREGS     = 15,
    PTRACE_ATTACH        = 16,
    PTRACE_DETACH        = 17,
    PTRACE_SYSCALL       = 24,
    PTRACE_SETOPTIONS    = 0x4200,
    PTRACE_GETEVENTMSG   = 0x4201,
    PTRACE_GETSIGINFO    = 0x4202,
    PTRACE_SETSIGINFO    = 0x4203,
    PTRACE_GETREGSET     = 0x4204,
    PTRACE_SETREGSET     = 0x4205,
    PTRACE_SEIZE         = 0x4206,
    PTRACE_INTERRUPT     = 0x4207,
    PTRACE_LISTEN        = 0x4208
};

#define PTRACE_TRACEME       PTRACE_TRACEME
#define PTRACE_PEEKTEXT      PTRACE_PEEKTEXT
#define PTRACE_PEEKDATA      PTRACE_PEEKDATA
#define PTRACE_PEEKUSER      PTRACE_PEEKUSER
#define PTRACE_POKETEXT      PTRACE_POKETEXT
#define PTRACE_POKEDATA      PTRACE_POKEDATA
#define PTRACE_POKEUSER      PTRACE_POKEUSER
#define PTRACE_CONT          PTRACE_CONT
#define PTRACE_KILL          PTRACE_KILL
#define PTRACE_SINGLESTEP    PTRACE_SINGLESTEP
#define PTRACE_GETREGS       PTRACE_GETREGS
#define PTRACE_SETREGS       PTRACE_SETREGS
#define PTRACE_GETFPREGS     PTRACE_GETFPREGS
#define PTRACE_SETFPREGS     PTRACE_SETFPREGS
#define PTRACE_ATTACH        PTRACE_ATTACH
#define PTRACE_DETACH        PTRACE_DETACH
#define PTRACE_SYSCALL       PTRACE_SYSCALL
#define PTRACE_SETOPTIONS    PTRACE_SETOPTIONS
#define PTRACE_GETEVENTMSG   PTRACE_GETEVENTMSG
#define PTRACE_GETSIGINFO    PTRACE_GETSIGINFO
#define PTRACE_SETSIGINFO    PTRACE_SETSIGINFO
#define PTRACE_GETREGSET     PTRACE_GETREGSET
#define PTRACE_SETREGSET     PTRACE_SETREGSET
#define PTRACE_SEIZE         PTRACE_SEIZE
#define PTRACE_INTERRUPT     PTRACE_INTERRUPT
#define PTRACE_LISTEN        PTRACE_LISTEN

#define PTRACE_O_TRACESYSGOOD   0x00000001
#define PTRACE_O_TRACEFORK      0x00000002
#define PTRACE_O_TRACEVFORK     0x00000004
#define PTRACE_O_TRACECLONE     0x00000008
#define PTRACE_O_TRACEEXEC      0x00000010
#define PTRACE_O_TRACEVFORKDONE 0x00000020
#define PTRACE_O_TRACEEXIT      0x00000040
#define PTRACE_O_EXITKILL       0x00100000

#define PTRACE_EVENT_FORK        1
#define PTRACE_EVENT_VFORK       2
#define PTRACE_EVENT_CLONE       3
#define PTRACE_EVENT_EXEC        4
#define PTRACE_EVENT_VFORK_DONE  5
#define PTRACE_EVENT_EXIT        6
#define PTRACE_EVENT_STOP        128

long ptrace(int request, pid_t pid, void *addr, void *data);
