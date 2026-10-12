/* Check structured eventfd writes remain whole 8-byte transactions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/syscall.h>
#include <unistd.h>

static unsigned passed;
static void require(bool ok, const char *what)
{
    if (ok) return;
    printf("DEVICEFAIL %s errno=%d\n", what, errno);
    printf("== probe complete: %u passed, 1 failed ==\n", passed);
    _Exit(1);
}

static void access_modes(void)
{
    sigset_t mask;
    require(sigemptyset(&mask) == 0, "signal mask");
    int fds[] = {syscall(SYS_eventfd2, 0, O_NONBLOCK),
                 timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK),
                 signalfd(-1, &mask, SFD_NONBLOCK)};
    const char *names[] = {"eventfd", "timerfd", "signalfd"};
    for (unsigned i = 0; i < 3; i++) {
        require(fds[i] >= 0, "anonymous descriptor");
        int mode = fcntl(fds[i], F_GETFL) & O_ACCMODE;
        require(mode == O_RDWR, "anonymous access mode");
        uint64_t value = 1;
        errno = 0;
        ssize_t result = write(fds[i], &value, sizeof(value));
        int error = result < 0 ? errno : 0;
        require(i == 0 ? result == 8 : result == -1 && error == EINVAL, "anonymous write result");
        printf("DEVICECASE {\"name\":\"%s.access\",\"mode\":%d,\"return\":%zd,\"errno\":%d}\n",
               names[i], mode, result, error);
        require(close(fds[i]) == 0, "anonymous close");
        passed++;
    }
}

static void closed_standard_descriptors(void)
{
    for (int fd = 0; fd < 3; fd++) {
        int saved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        require(saved >= 0 && close(fd) == 0, "save standard descriptor");
        errno = 0;
        long read_result = syscall(SYS_read, fd, NULL, 0);
        int read_error = errno;
        errno = 0;
        long write_result = syscall(SYS_write, fd, NULL, 0);
        int write_error = errno;
        int restored = dup2(saved, fd);
        require(restored == fd && close(saved) == 0, "restore standard descriptor");
        require(read_result == -1 && read_error == EBADF &&
                write_result == -1 && write_error == EBADF, "closed standard descriptor");
        printf("DEVICECASE {\"name\":\"closed.%d\",\"read_errno\":%d,\"write_errno\":%d}\n",
               fd, read_error, write_error);
        passed++;
    }
}

static void check(bool unmapped, unsigned prefix)
{
    int fd = syscall(SYS_eventfd2, 7, O_NONBLOCK | O_CLOEXEC);
    require(fd >= 0, "eventfd2");
    unsigned char *map = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(map != MAP_FAILED, "map");
    memset(map, 0xc7, 8192);
    unsigned char *pointer = map + 4096 - prefix;
    uint64_t value = 3, observed = 0;
    memcpy(pointer, &value, sizeof(value));
    if (unmapped) require(munmap(map + 4096, 4096) == 0, "unmap boundary");
    else if (prefix < 8) require(mprotect(map + 4096, 4096, PROT_NONE) == 0, "protect");
    errno = 0;
    long result = syscall(SYS_write, fd, pointer, 8);
    int error = result < 0 ? errno : 0;
    require(prefix == 8 ? result == 8 : result == -1 && error == EFAULT, "write result");
    require(read(fd, &observed, sizeof(observed)) == sizeof(observed), "read counter");
    require(observed == (prefix == 8 ? 10 : 7), "counter changed on fault");
    require(!memcmp(pointer, &value, prefix), "source changed");
    for (unsigned i = 1; i <= 8; i++) require(pointer[-(int)i] == 0xc7, "preceding canary");
    printf("DEVICECASE {\"name\":\"eventfd.write.%s.%u\",\"return\":%ld,\"errno\":%d,"
           "\"counter\":%llu,\"canary\":true}\n", unmapped ? "unmapped" : "none", prefix,
           result, error, (unsigned long long)observed);
    passed++;
    require(close(fd) == 0 && munmap(map, 8192) == 0, "cleanup");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    require(sysconf(_SC_PAGESIZE) == 4096, "page size");
    access_modes();
    closed_standard_descriptors();
    const unsigned prefixes[] = {1, 4, 7, 8};
    for (unsigned unmapped = 0; unmapped < 2; unmapped++)
        for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
            check(unmapped, prefixes[i]);
    printf("== probe complete: %u passed, 0 failed ==\n", passed);
    if (!getenv("UCOPY_OBSERVE_EXIT")) for (;;) pause();
    return 0;
}
