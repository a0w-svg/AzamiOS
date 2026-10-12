/* Linux oracle for argument precedence and page-sized fault effects.
 * Collection success is separate from compatibility: compare every field. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

static unsigned collected;
static unsigned char data[8192];
static void require(bool ok, const char *what)
{
    if (ok) return;
    printf("UCOPYFAIL %s errno=%d\n", what, errno);
    printf("== probe complete: %u passed, 1 failed ==\n", collected);
    _Exit(1);
}

static void record(const char *name, long result, int error, int fd, int ends[2],
                   bool writing, unsigned char *pointer, size_t prefix)
{
    long long offset = -1;
    bool pipe_file = ends[0] >= 0;
    if (pipe_file) {
        require(close(ends[1]) == 0, "close writer");
        ends[1] = -1;
    } else {
        offset = lseek(fd, 0, SEEK_CUR);
        require(offset >= 0 && lseek(fd, 0, SEEK_SET) == 0, "capture offset");
    }
    unsigned copied = 0;
    bool canary = true;
    if (prefix) {
        for (unsigned i = 1; i <= 8; i++) if (pointer[-(int)i] != 0xc7) canary = false;
        if (!writing) while (copied < prefix && pointer[copied] == data[copied]) copied++;
        else require(memcmp(pointer, data, prefix) == 0, "source modified");
    }
    unsigned char contents[8192];
    ssize_t size = read(pipe_file ? ends[0] : fd, contents, sizeof(contents));
    require(size >= 0 && size <= (pipe_file ? 4096 : 8192), "capture data");
    uint64_t checksum = 0;
    for (ssize_t i = 0; i < size; i++) checksum += (uint64_t)(i + 1) * contents[i];
    printf("UCOPYCASE {\"name\":\"%s\",\"return\":%ld,\"errno\":%d,"
           "\"offset\":%lld,\"remaining_or_size\":%zd,\"data_checksum\":%llu,"
           "\"copied_prefix\":%u,\"canary\":%s}\n", name, result, error,
           offset, size, (unsigned long long)checksum, copied, canary ? "true" : "false");
    collected++;
    require(close(pipe_file ? ends[0] : fd) == 0, "close reader/file");
}

static int fresh(bool pipe_file, bool writing, int ends[2])
{
    int fd;
    ends[0] = ends[1] = -1;
    if (pipe_file) {
        require(pipe2(ends, O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
        fd = ends[writing ? 1 : 0];
        if (!writing) require(write(ends[1], data, 4096) == 4096, "seed pipe buffer");
    } else {
        fd = memfd_create("usercopy-compat", MFD_CLOEXEC);
        require(fd >= 0, "memfd_create");
        if (!writing) {
            require(write(fd, data, sizeof(data)) == sizeof(data), "seed memfd");
            require(lseek(fd, 0, SEEK_SET) == 0, "rewind");
        }
    }
    return fd;
}

static const char *const argument_names[] = {
    "invalid-zero", "invalid-null", "invalid-huge", "zero", "null", "bad", "huge", "above-user",
    "v-invalid-zero", "v-invalid-negative", "v-invalid-too-many", "v-zero", "v-negative",
    "v-too-many", "v-null", "v-bad", "v-zero-payload", "v-overflow", "v-max-zero"
};
static void argument(bool writing, unsigned which)
{
    int ends[2], fd = fresh(false, writing, ends), target = fd;
    void *pointer = data;
    size_t count = 1;
    struct iovec vector[1024] = {{0}};
    struct iovec *iov = vector;
    int vectors = 1;
    switch (which) {
        case 0: target = -1; pointer = (void *)1; count = 0; break;
        case 1: target = -1; pointer = NULL; break;
        case 2: target = -1; pointer = (void *)1; count = SIZE_MAX; break;
        case 3: pointer = (void *)1; count = 0; break;
        case 4: pointer = NULL; break;
        case 5: pointer = (void *)1; break;
        case 6: count = SIZE_MAX; break;
        case 7: pointer = (void *)(uintptr_t)0x800000000000ULL; break;
        case 8: target = -1; iov = (void *)1; vectors = 0; break;
        case 9: target = -1; iov = (void *)1; vectors = -1; break;
        case 10: target = -1; iov = (void *)1; vectors = 1025; break;
        case 11: iov = (void *)1; vectors = 0; break;
        case 12: iov = (void *)1; vectors = -1; break;
        case 13: iov = (void *)1; vectors = 1025; break;
        case 14: iov = NULL; break;
        case 15: iov = (void *)1; break;
        case 16: vector[0] = (struct iovec){(void *)1, 0}; break;
        case 17: vector[0] = (struct iovec){data, (size_t)SSIZE_MAX + 1}; break;
        case 18: vectors = 1024; break;
    }
    errno = 0;
    long result = which < 8 ? syscall(writing ? SYS_write : SYS_read, target, pointer, count) :
                             syscall(writing ? SYS_writev : SYS_readv, target, iov, vectors);
    int error = result < 0 ? errno : 0;
    char name[96];
    snprintf(name, sizeof(name), "memfd.%s.args.%s", writing ? "write" : "read", argument_names[which]);
    record(name, result, error, fd, ends, writing, NULL, 0);
}

static void access_mode(bool writing)
{
    int ends[2], fd = fresh(true, writing, ends);
    (void)fd;
    errno = 0;
    long result = syscall(writing ? SYS_write : SYS_read, ends[writing ? 0 : 1], data, 0);
    int error = result < 0 ? errno : 0;
    record(writing ? "pipe.write.args.wrong-end-zero" : "pipe.read.args.wrong-end-zero",
           result, error, ends[writing ? 1 : 0], ends, writing, NULL, 0);
}

static void boundary(bool pipe_file, bool writing, unsigned mode, unsigned prefix)
{
    int ends[2], fd = fresh(pipe_file, writing, ends);
    unsigned char *map = mmap(NULL, 16384, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(map != MAP_FAILED, "map");
    memset(map, 0xc7, 16384);
    unsigned char *pointer = map + 8192 - prefix;
    size_t count = prefix == 4096 ? 8192 : 4096;
    if (writing) memcpy(pointer, data, count);
    if (mode == 2) require(munmap(map + 8192, 4096) == 0, "unmap boundary");
    else if (mode < 3 || mode == 4)
        require(mprotect(map + 8192, 4096, mode == 1 ? PROT_READ : PROT_NONE) == 0, "protect boundary");
    if (mode == 3) count = 4096;
    struct iovec iov[2] = {{pointer, 4096}, {map + 8192, 4096}};
    errno = 0;
    long result = mode == 4 ? syscall(writing ? SYS_writev : SYS_readv, fd, iov, 2) :
                             syscall(writing ? SYS_write : SYS_read, fd, pointer, count);
    int error = result < 0 ? errno : 0;
    char name[96];
    const char *modes[] = {"none", "readonly", "unmapped", "valid", "vector-page"};
    snprintf(name, sizeof(name), "%s.%s.page.%s.%u", pipe_file ? "pipe" : "memfd",
             writing ? "write" : "read", modes[mode], prefix);
    record(name, result, error, fd, ends, writing, pointer, prefix);
    require(munmap(map, 16384) == 0, "release mapping");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    require(sysconf(_SC_PAGESIZE) == 4096, "4KiB prerequisite");
    for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i % 251 + 1);
    printf("UCOPYENV {\"mode\":\"compat-extension\",\"page_bytes\":4096,\"cases\":84}\n");
    for (unsigned writing = 0; writing < 2; writing++) {
        for (unsigned which = 0; which < sizeof(argument_names)/sizeof(argument_names[0]); which++)
            argument(writing, which);
        access_mode(writing);
    }
    const unsigned prefixes[] = {1, 2048, 4095};
    for (unsigned pipe_file = 0; pipe_file < 2; pipe_file++)
        for (unsigned writing = 0; writing < 2; writing++) {
            for (unsigned mode = 0; mode < 3; mode++)
                for (unsigned i = 0; i < 3; i++) boundary(pipe_file, writing, mode, prefixes[i]);
            boundary(pipe_file, writing, 3, 4096);
        }
    for (unsigned writing = 0; writing < 2; writing++) {
        boundary(false, writing, 0, 4096);
        boundary(false, writing, 4, 4096);
    }
    printf("== usercopy observations complete: %u collected ==\n", collected);
    printf("== probe complete: %u passed, 0 failed ==\n", collected);
    if (!getenv("UCOPY_OBSERVE_EXIT")) for (;;) pause();
    return 0;
}
