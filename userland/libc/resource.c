/* ============================================================================
 * AzamiOS Userspace — Resource Limits Implementation (resource.c)
 * File: userland/libc/resource.c
 * ============================================================================ */

#include "include/sys/resource.h"
#include "include/sys/syscall.h"
#include "include/string.h"

int getrlimit(int resource, struct rlimit *rlim)
{
    if (!rlim) return -1;
    long ret = syscall2(97 /* SYS_getrlimit */, resource, (long)rlim);
    if (ret != 0) {
        /* Fallback reasonable defaults */
        if (resource == RLIMIT_NOFILE) {
            rlim->rlim_cur = 1024;
            rlim->rlim_max = 4096;
        } else if (resource == RLIMIT_STACK) {
            rlim->rlim_cur = 8 * 1024 * 1024;
            rlim->rlim_max = RLIM_INFINITY;
        } else {
            rlim->rlim_cur = RLIM_INFINITY;
            rlim->rlim_max = RLIM_INFINITY;
        }
    }
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    if (!rlim) return -1;
    return (int)syscall2(160 /* SYS_setrlimit */, resource, (long)rlim);
}

int getrusage(int who, struct rusage *usage)
{
    if (!usage) return -1;
    memset(usage, 0, sizeof(*usage));
    return (int)syscall2(98 /* SYS_getrusage */, who, (long)usage);
}

int getpriority(int which, id_t who)
{
    long ret = syscall2(SYS_getpriority, which, who);
    return (int)ret;
}

int setpriority(int which, id_t who, int prio)
{
    return (int)syscall3(SYS_setpriority, which, who, prio);
}
