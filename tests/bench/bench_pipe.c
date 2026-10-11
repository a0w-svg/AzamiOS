#include "common.h"
#include <signal.h>

/* Returns 0 on complete I/O, 1 on EOF, -1 on an error. */
static int transfer(int fd, unsigned char *buffer, size_t length, int writing)
{
    size_t done = 0;
    while (done < length) {
        ssize_t n = writing ? write(fd, buffer + done, length - done)
                            : read(fd, buffer + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) return 1;
        done += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint64_t count = bench_count(argc, argv, 10000, 100000000);
    uint64_t warmup = count < 100 ? count : 100;
    uint64_t resolution = bench_resolution();
    int request[2], reply[2];
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) bench_die("signal");
    if (pipe(request)) bench_die("request pipe");
    if (pipe(reply)) { close(request[0]); close(request[1]); bench_die("reply pipe"); }
    pid_t child = fork();
    if (child < 0) {
        close(request[0]); close(request[1]); close(reply[0]); close(reply[1]);
        bench_die("fork");
    }
    if (!child) {
        close(request[1]); close(reply[0]);
        for (uint64_t i = 0; i < count + warmup; i++) {
            unsigned char byte;
            if (transfer(request[0], &byte, 1, 0) || byte != (unsigned char)(i % 251))
                _exit(1);
            byte ^= 0xff;
            if (transfer(reply[1], &byte, 1, 1)) _exit(1);
        }
        unsigned char extra;
        int eof = transfer(request[0], &extra, 1, 0);
        int failed = close(request[0]) | close(reply[1]);
        _exit(eof == 1 && !failed ? 0 : 1);
    }
    close(request[0]); close(reply[1]);
    uint64_t start = 0, checksum = 0;
    int failed = 0;
    for (uint64_t i = 0; i < count + warmup; i++) {
        if (i == warmup) start = bench_now();
        unsigned char byte = (unsigned char)(i % 251);
        if (transfer(request[1], &byte, 1, 1) || transfer(reply[0], &byte, 1, 0) ||
            byte != ((unsigned char)(i % 251) ^ 0xff)) {
            fprintf(stderr, "pipe round trip failed at iteration %" PRIu64 "\n", i);
            failed = 1;
            break;
        }
        if (i >= warmup) checksum += byte;
    }
    uint64_t ns = failed ? 0 : bench_elapsed(start);
    if (close(request[1])) failed = 1;
    if (!failed) {
        unsigned char extra;
        if (transfer(reply[0], &extra, 1, 0) != 1) failed = 1;
    }
    if (close(reply[0])) failed = 1;
    if (failed) kill(child, SIGKILL);
    if (bench_wait(child)) failed = 1;
    if (failed) return 1;
    bench_result("pipe", "round_trip", count, warmup, ns, resolution, checksum);
    return 0;
}
