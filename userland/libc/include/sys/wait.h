/* ============================================================================
 * AzamiOS Userspace — Process Wait Header (sys/wait.h)
 * File: userland/libc/include/sys/wait.h
 * ============================================================================ */
#pragma once

#include "types.h"

#define WNOHANG    1
#define WUNTRACED  2
#define WSTOPPED   2
#define WEXITED    4
#define WCONTINUED 8
#define WNOWAIT    0x01000000

#define WIFEXITED(status)    (((status) & 0x7f) == 0)
#define WEXITSTATUS(status)  (((status) & 0xff00) >> 8)
#define WIFSIGNALED(status)  (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)
#define WTERMSIG(status)     ((status) & 0x7f)
#define WIFSTOPPED(status)   (((status) & 0xff) == 0x7f)
#define WSTOPSIG(status)     (WEXITSTATUS(status))
#define WIFCONTINUED(status) ((status) == 0xffff)

typedef enum {
    P_ALL,
    P_PID,
    P_PGID
} idtype_t;

typedef struct siginfo siginfo_t;

int wait(int *status);
int waitpid(int pid, int *status, int options);
int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options);
