/* ============================================================================
 * AzamiOS Userspace — POSIX Memory Management (mman.c)
 * File: userland/libc/mman.c
 * ============================================================================ */

#include "include/sys/mman.h"
#include "include/sys/syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    long ret = syscall6(SYS_mmap, (long)addr, (long)length, prot, flags, fd, offset);
    if (ret < 0 && ret >= -4095) {
        return MAP_FAILED;
    }
    return (void *)ret;
}

int munmap(void *addr, size_t length)
{
    return (int)syscall2(SYS_munmap, (long)addr, (long)length);
}

int mprotect(void *addr, size_t len, int prot)
{
    return (int)syscall3(10 /* SYS_mprotect */, (long)addr, (long)len, prot);
}

int msync(void *addr, size_t length, int flags)
{
    return (int)syscall3(26 /* SYS_msync */, (long)addr, (long)length, flags);
}
