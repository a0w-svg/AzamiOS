/**
 * syscalls.c  –  Newlib Libgloss & C Library System Call Interface Stubs
 * Optimized for direct microkernel ABI fast-paths and zero-copy data routing.
 */
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>

int errno;
char **environ = (char**)0;

static inline int lpc_syscall_fast(uint32_t num, uint64_t a1, uint64_t a2, uint64_t a3) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

int _read(int file, char *ptr, int len) {
    if (file < 0 || !ptr || len < 0) return -1;
    int ret = lpc_syscall_fast(SYS_READ, (uint64_t)file, (uint64_t)(uintptr_t)ptr, (uint64_t)len);
    if (ret < 0) errno = 5; /* EIO */
    return ret;
}

int _write(int file, char *ptr, int len) {
    if (file < 0 || !ptr || len < 0) return -1;
    int ret = lpc_syscall_fast(SYS_WRITE, (uint64_t)file, (uint64_t)(uintptr_t)ptr, (uint64_t)len);
    if (ret < 0) errno = 5; /* EIO */
    return ret;
}

int _open(const char *name, int flags, int mode) {
    if (!name) return -1;
    int ret = lpc_syscall_fast(SYS_OPEN, (uint64_t)(uintptr_t)name, (uint64_t)flags, (uint64_t)mode);
    if (ret < 0) errno = 2; /* ENOENT */
    return ret;
}

int _close(int file) {
    if (file < 0) return -1;
    return lpc_syscall_fast(SYS_CLOSE, (uint64_t)file, 0, 0);
}

void *_sbrk(int incr) {
    extern char __heap_start;
    static char *heap_end = (char*)0;
    char *prev_heap_end;

    if (heap_end == 0) {
        heap_end = &__heap_start;
    }
    prev_heap_end = heap_end;

    int ret = lpc_syscall_fast(SYS_SBRK, (uint64_t)incr, 0, 0);
    if (ret < 0) {
        errno = 12; /* ENOMEM */
        return (void *)-1;
    }
    heap_end += incr;
    return (void *)prev_heap_end;
}

int _fstat(int file, struct stat *st) {
    if (file < 0 || !st) return -1;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) {
    return (file >= 0 && file <= 2) ? 1 : 0;
}

int _lseek(int file, int ptr, int dir) {
    (void)file; (void)ptr; (void)dir;
    return 0;
}

int _getpid(void) {
    return lpc_syscall_fast(SYS_GETPID, 0, 0, 0);
}

int _kill(int pid, int sig) {
    (void)pid; (void)sig;
    errno = 22; /* EINVAL */
    return -1;
}

void _exit(int status) {
    lpc_syscall_fast(SYS_EXIT, (uint64_t)status, 0, 0);
    while (1);
}

/* Standard POSIX non-underscore aliases */
#ifndef _READ_WRITE_RETURN_TYPE
#define _READ_WRITE_RETURN_TYPE ssize_t
#endif

_READ_WRITE_RETURN_TYPE read(int fd, void *buf, size_t count) { return _read(fd, (char*)buf, (int)count); }
_READ_WRITE_RETURN_TYPE write(int fd, const void *buf, size_t count) { return _write(fd, (char*)buf, (int)count); }
int open(const char *name, int flags, ...) { return _open(name, flags, 0); }
int close(int fd) { return _close(fd); }
void *sbrk(ptrdiff_t incr) { return _sbrk((int)incr); }
pid_t getpid(void) { return _getpid(); }
void exit(int status) { _exit(status); }

void exec(const char *filename) {
    if (filename) lpc_syscall_fast(10, (uint64_t)(uintptr_t)filename, 0, 0);
}

int fork(void) {
    return lpc_syscall_fast(SYS_FORK, 0, 0, 0);
}

void yield(void) {
    lpc_syscall_fast(SYS_YIELD, 0, 0, 0);
}

int thread_create(void (*entry)(void*), void *arg, void *user_stack) {
    if (!entry || !user_stack) return -1;
    return lpc_syscall_fast(SYS_THREAD_CREATE, (uint64_t)(uintptr_t)entry, (uint64_t)(uintptr_t)arg, (uint64_t)(uintptr_t)user_stack);
}

int mount(const char *dev, const char *dir, const char *type) {
    if (!dev || !dir || !type) return -1;
    return lpc_syscall_fast(SYS_MOUNT, (uint64_t)(uintptr_t)dev, (uint64_t)(uintptr_t)dir, (uint64_t)(uintptr_t)type);
}

int unmount(const char *dir) {
    if (!dir) return -1;
    return lpc_syscall_fast(SYS_UNMOUNT, (uint64_t)(uintptr_t)dir, 0, 0);
}

off_t lseek(int fd, off_t off, int whence) { return _lseek(fd, (int)off, whence); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int isatty(int fd) { return _isatty(fd); }
int kill(pid_t pid, int sig) { return _kill((int)pid, sig); }

void rtc_get_time(rtc_time_t *time) {
    if (time) lpc_syscall_fast(SYS_TIME, (uint64_t)(uintptr_t)time, 0, 0);
}

extern int main(int argc, char **argv) __attribute__((weak));

void __attribute__((weak)) _start(void) {
    int ret = 0;
    if (main) {
        ret = main(0, (char**)0);
    }
    _exit(ret);
}
