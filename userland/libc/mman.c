/* ============================================================================
 * AzamiOS Userspace — POSIX Memory Management (mman.c)
 * File: userland/libc/mman.c
 * ============================================================================ */

#include "include/sys/mman.h"
#include "include/sys/syscall.h"
#include "include/errno.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    long ret = syscall6(SYS_mmap, (long)addr, (long)length, prot, flags, fd, offset);
    if (ret < 0 && ret >= -4095) {
        errno = (int)-ret;
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    long ret = syscall2(SYS_munmap, (long)addr, (long)length);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    long ret = syscall3(10 /* SYS_mprotect */, (long)addr, (long)len, prot);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int msync(void *addr, size_t length, int flags)
{
    long ret = syscall3(26 /* SYS_msync */, (long)addr, (long)length, flags);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int mlock(const void *addr, size_t len)
{
    long ret = syscall2(SYS_mlock, (long)addr, (long)len);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int munlock(const void *addr, size_t len)
{
    long ret = syscall2(SYS_munlock, (long)addr, (long)len);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int mlockall(int flags)
{
    long ret = syscall1(SYS_mlockall, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int munlockall(void)
{
    long ret = syscall1(SYS_munlockall, 0);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}
