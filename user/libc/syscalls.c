/**
 * syscalls.c  –  Newlib Libgloss System Call Interface Stubs
 * Translates standard C library I/O and memory management requests into int $128 interrupts.
 */
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

int errno;
char **environ = (char**)0;

int _read(int file, char *ptr, int len) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_READ), "b"(file), "c"(ptr), "d"(len) : "memory");
    return ret;
}

int _write(int file, char *ptr, int len) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_WRITE), "b"(file), "c"(ptr), "d"(len) : "memory");
    return ret;
}

int _open(const char *name, int flags, int mode) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_OPEN), "b"(name), "c"(flags), "d"(mode) : "memory");
    return ret;
}

int _close(int file) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_CLOSE), "b"(file) : "memory");
    return ret;
}

void *_sbrk(int incr) {
    extern char __heap_start;
    static char *heap_end = (char*)0;
    char *prev_heap_end;

    if (heap_end == 0) {
        heap_end = &__heap_start;
    }
    prev_heap_end = heap_end;

    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_SBRK), "b"(incr) : "memory");
    if (ret < 0) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return (void *)prev_heap_end;
}

int _fstat(int file, struct stat *st) {
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) {
    return (file <= 2) ? 1 : 0;
}

int _lseek(int file, int ptr, int dir) {
    (void)file; (void)ptr; (void)dir;
    return 0;
}

int _getpid(void) {
    int ret;
    asm volatile("int $128" : "=a"(ret) : "a"(SYS_GETPID) : "memory");
    return ret;
}

int _kill(int pid, int sig) {
    (void)pid; (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int status) {
    asm volatile("int $128" : : "a"(SYS_EXIT), "b"(status) : "memory");
    while (1);
}

/* Standard POSIX non-underscore aliases */
ssize_t read(int fd, void *buf, size_t count) { return _read(fd, (char*)buf, (int)count); }
ssize_t write(int fd, const void *buf, size_t count) { return _write(fd, (char*)buf, (int)count); }
int open(const char *name, int flags, ...) { return _open(name, flags, 0); }
int close(int fd) { return _close(fd); }
void *sbrk(int incr) { return _sbrk(incr); }
pid_t getpid(void) { return _getpid(); }
void exit(int status) { _exit(status); }

void exec(const char *filename) {
    asm volatile("int $128" : : "a"(10), "b"(filename) : "memory");
}

