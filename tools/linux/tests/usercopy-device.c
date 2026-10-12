/* Check structured eventfd writes remain whole 8-byte transactions. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    const unsigned prefixes[] = {1, 4, 7, 8};
    for (unsigned unmapped = 0; unmapped < 2; unmapped++)
        for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
            check(unmapped, prefixes[i]);
    printf("== probe complete: %u passed, 0 failed ==\n", passed);
    if (!getenv("UCOPY_OBSERVE_EXIT")) for (;;) pause();
    return 0;
}
