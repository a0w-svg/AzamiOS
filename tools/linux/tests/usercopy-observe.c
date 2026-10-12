/* Collect fault-side effects with the same static Linux binary on both OSes.
 * A successful collection verdict is not a compatibility verdict: the runner
 * compares every observation with the Linux oracle and reports differences. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

enum { NONE, READONLY, UNMAPPED, VECTOR_DATA, VECTOR_DESC,
       ZERO_SCALAR, ZERO_VECTOR, NULL_DATA, VALID };
static const char *const modes[] = {"none", "readonly", "unmapped", "vector-data",
    "vector-desc", "zero-scalar", "zero-vector", "null-data", "valid"};
static unsigned collected;
static unsigned char data[128];

static void require(bool ok, const char *what)
{
    if (ok) return;
    printf("UCOPYFAIL %s errno=%d\n", what, errno);
    printf("== probe complete: %u passed, 1 failed ==\n", collected);
    _Exit(1);
}

static void observe(bool pipe_file, bool writing, unsigned mode, unsigned prefix)
{
    int fd, ends[2] = {-1, -1};
    if (pipe_file) {
        require(pipe2(ends, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
        fd = ends[writing ? 1 : 0];
        if (!writing) require(write(ends[1], data, sizeof(data)) == sizeof(data), "seed pipe");
    } else {
        fd = memfd_create("usercopy-observation", MFD_CLOEXEC);
        require(fd >= 0, "memfd_create");
        if (!writing) {
            require(write(fd, data, sizeof(data)) == sizeof(data), "seed memfd");
            require(lseek(fd, 0, SEEK_SET) == 0, "rewind memfd");
        }
    }
    unsigned char *map = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(map != MAP_FAILED, "data mapping");
    memset(map, 0xc7, 8192);
    unsigned char *pointer = map + 4096 - prefix;
    size_t count = 2 * prefix;
    if (writing) memcpy(pointer, data, count);
    bool canary = true;
    unsigned char *descriptors = NULL;
    struct iovec vector[2];
    struct iovec *iov = vector;
    bool vectored = false;
    int vectors = 2;
    if (mode <= UNMAPPED) {
        if (mode == UNMAPPED)
            require(munmap(map + 4096, 4096) == 0, "unmap boundary");
        else
            require(mprotect(map + 4096, 4096, mode == READONLY ? PROT_READ : PROT_NONE) == 0,
                    "protect boundary");
    } else if (mode == VECTOR_DATA || mode == VECTOR_DESC) {
        prefix = 8;
        pointer = map + 64;
        if (writing) memcpy(pointer, data, prefix);
        require(mprotect(map + 4096, 4096, PROT_NONE) == 0, "protect vector payload");
        vector[0] = (struct iovec){.iov_base = pointer, .iov_len = prefix};
        vector[1] = (struct iovec){.iov_base = map + 4096, .iov_len = 8};
        vectored = true;
        if (mode == VECTOR_DESC) {
            descriptors = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            require(descriptors != MAP_FAILED, "descriptor mapping");
            iov = (struct iovec *)(descriptors + 4096 - sizeof(*iov));
            iov[0] = vector[0];
            iov[1] = vector[1];
            require(mprotect(descriptors + 4096, 4096, PROT_NONE) == 0,
                    "protect later descriptor");
        }
    } else if (mode == ZERO_SCALAR || mode == ZERO_VECTOR || mode == NULL_DATA) {
        pointer = mode == NULL_DATA ? NULL : (void *)1;
        prefix = 0;
        count = mode == NULL_DATA ? 1 : 0;
        if (mode == ZERO_VECTOR) { vectored = true; vectors = 0; iov = (void *)1; }
    } else {
        pointer = map + 64;
        prefix = 32; count = 32;
        if (writing) memcpy(pointer, data, count);
    }
    errno = 0;
    long result = vectored ? syscall(writing ? SYS_writev : SYS_readv, fd, iov, vectors) :
                            syscall(writing ? SYS_write : SYS_read, fd, pointer, count);
    int error = result < 0 ? errno : 0;
    long long offset = -1;
    if (!pipe_file) {
        offset = lseek(fd, 0, SEEK_CUR);
        require(offset >= 0, "capture file position");
        require(lseek(fd, 0, SEEK_SET) == 0, "rewind after observation");
    } else {
        require(close(ends[1]) == 0, "close pipe writer");
        ends[1] = -1;
    }
    unsigned copied = 0;
    if (prefix) {
        for (unsigned i = 1; i <= 8; i++) if (pointer[-(int)i] != 0xc7) canary = false;
        if (!writing) while (copied < prefix && pointer[copied] == data[copied]) copied++;
        else require(!memcmp(pointer, data, prefix), "source bytes changed");
    }
    unsigned char contents[256];
    ssize_t remaining = read(pipe_file ? ends[0] : fd, contents, sizeof(contents));
    require(remaining >= 0 && remaining <= 128, "capture stored/remaining data");
    unsigned checksum = 0;
    for (ssize_t i = 0; i < remaining; i++) checksum += (unsigned)(i + 1) * contents[i];
    printf("UCOPYCASE {\"name\":\"%s.%s.%s.%u\",\"return\":%ld,\"errno\":%d,"
           "\"offset\":%lld,\"remaining_or_size\":%zd,\"data_checksum\":%u,"
           "\"copied_prefix\":%u,\"canary\":%s}\n",
           pipe_file ? "pipe" : "memfd", writing ? "write" : "read", modes[mode],
           mode <= UNMAPPED ? prefix : 0, result, error, offset, remaining,
           checksum, copied, canary ? "true" : "false");
    collected++;
    require(munmap(map, mode == UNMAPPED ? 4096 : 8192) == 0, "release data mapping");
    if (descriptors) require(munmap(descriptors, 8192) == 0, "release descriptor mapping");
    if (pipe_file) require(close(ends[0]) == 0, "close pipe reader");
    else require(close(fd) == 0, "close memfd");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    require(sysconf(_SC_PAGESIZE) == 4096, "4KiB page prerequisite");
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i + 1);
    printf("UCOPYENV {\"mode\":\"observation\",\"page_bytes\":4096,\"cases\":60}\n");
    const unsigned prefixes[] = {1, 16, 63};
    for (unsigned pipe_file = 0; pipe_file < 2; pipe_file++)
        for (unsigned writing = 0; writing < 2; writing++) {
            for (unsigned mode = NONE; mode <= UNMAPPED; mode++)
                for (unsigned i = 0; i < 3; i++) observe(pipe_file, writing, mode, prefixes[i]);
            for (unsigned mode = VECTOR_DATA; mode <= VALID; mode++) observe(pipe_file, writing, mode, 16);
        }
    printf("== usercopy observations complete: %u collected ==\n", collected);
    printf("== probe complete: %u passed, 0 failed ==\n", collected);
    if (!getenv("UCOPY_OBSERVE_EXIT")) for (;;) pause();
    return 0;
}
