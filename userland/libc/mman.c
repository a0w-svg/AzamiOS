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
    long ret = syscall3(SYS_mprotect, (long)addr, (long)len, prot);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int pkey_alloc(unsigned int flags, unsigned int access_rights)
{
    long ret = syscall2(SYS_pkey_alloc, (long)flags, (long)access_rights);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return (int)ret;
}

int pkey_free(int pkey)
{
    long ret = syscall1(SYS_pkey_free, pkey);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int pkey_mprotect(void *addr, size_t len, int prot, int pkey)
{
    long ret = syscall4(SYS_pkey_mprotect, (long)addr, (long)len, prot, pkey);
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return 0;
}

int mseal(void *addr, size_t len, unsigned long flags)
{
    long ret = syscall3(SYS_mseal, (long)addr, (long)len, (long)flags);
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

int mlock2(const void *addr, size_t len, int flags)
{
    long ret = syscall3(SYS_mlock2, (long)addr, (long)len, flags);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int madvise(void *addr, size_t length, int advice)
{
    long ret = syscall3(SYS_madvise, (long)addr, (long)length, advice);
    if (ret < 0) { errno = (int)-ret; return -1; }
    return 0;
}

int posix_madvise(void *addr, size_t len, int advice)
{
    long ret = syscall3(SYS_madvise, (long)addr, (long)len, advice);
    if (ret < 0) return (int)-ret;
    return 0;
}

/* ── POSIX Shared Memory (POSIX.1b / IEEE Std 1003.1b) ───────────────────── */

#include "include/fcntl.h"
#include "include/unistd.h"
#include "include/stdio.h"
#include "include/string.h"
#include "include/sys/stat.h"

int shm_open(const char *name, int oflag, mode_t mode)
{
    if (!name || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    char path[256];
    while (*name == '/') name++;

    /* Ensure /dev/shm directory exists */
    (void)mkdir("/dev/shm", 0777);

    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    int fd = open(path, oflag | O_CLOEXEC, mode);
    if (fd < 0 && (errno == ENOENT || errno == ENOTDIR)) {
        /* Fallback to /tmp/.shm */
        (void)mkdir("/tmp/.shm", 0777);
        snprintf(path, sizeof(path), "/tmp/.shm/%s", name);
        fd = open(path, oflag | O_CLOEXEC, mode);
    }
    return fd;
}

int shm_unlink(const char *name)
{
    if (!name || name[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    char path[256];
    while (*name == '/') name++;

    snprintf(path, sizeof(path), "/dev/shm/%s", name);
    int ret = unlink(path);
    if (ret < 0) {
        snprintf(path, sizeof(path), "/tmp/.shm/%s", name);
        ret = unlink(path);
    }
    return ret;
}
