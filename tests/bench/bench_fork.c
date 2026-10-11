#include "common.h"

static void spawn(void)
{
    pid_t child = fork();
    if (child < 0) bench_die("fork");
    if (!child) _exit(0);
    if (bench_wait(child)) exit(1);
}

int main(int argc, char **argv)
{
    uint64_t count = bench_count(argc, argv, 1000, 1000000);
    uint64_t warmup = count < 10 ? count : 10;
    uint64_t resolution = bench_resolution();
    for (uint64_t i = 0; i < warmup; i++) spawn();
    uint64_t start = bench_now();
    for (uint64_t i = 0; i < count; i++) spawn();
    uint64_t ns = bench_elapsed(start);
    bench_result("fork", "fork_waitpid", count, warmup, ns, resolution, count);
    return 0;
}
