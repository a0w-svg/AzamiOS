#include "common.h"
#include <cpuid.h>
#include <signal.h>
#include <sys/utsname.h>

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void json_string(const char *value)
{
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 32 || *p > 126) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void environment(void)
{
    struct utsname info;
    if (uname(&info)) bench_die("uname");
    char brand[49] = {0};
    unsigned int maximum = __get_cpuid_max(0x80000000, NULL);
    if (maximum >= 0x80000004) {
        for (unsigned int i = 0; i < 3; i++) {
            unsigned int a, b, c, d;
            __cpuid(0x80000002 + i, a, b, c, d);
            memcpy(brand + i * 16, &a, 4); memcpy(brand + i * 16 + 4, &b, 4);
            memcpy(brand + i * 16 + 8, &c, 4); memcpy(brand + i * 16 + 12, &d, 4);
        }
    }
    printf("AZBENCH {\"type\":\"environment\",\"sysname\":"); json_string(info.sysname);
    printf(",\"release\":"); json_string(info.release);
    printf(",\"machine\":"); json_string(info.machine);
    printf(",\"cpu_brand\":"); json_string(brand);
    printf(",\"clock\":\"CLOCK_MONOTONIC\",\"timer_resolution_ns\":%" PRIu64 "}\n",
           bench_resolution());
}

static int run_child(const char *directory, const char *name, uint64_t count,
                      uint64_t timeout_seconds, uint64_t id, int warmup)
{
    char path[PATH_MAX], number[32];
    if (snprintf(path, sizeof(path), "%s/bench_%s", directory, name) >= (int)sizeof(path))
        return 1;
    snprintf(number, sizeof(number), "%" PRIu64, count);
    printf("AZBENCH {\"type\":\"begin\",\"id\":%" PRIu64 ",\"benchmark\":\"%s\","
           "\"warmup\":%s}\n", id, name, warmup ? "true" : "false");
    fflush(NULL);
    uint64_t start = bench_now();
    pid_t child = fork();
    if (child < 0) bench_die("supervisor fork");
    if (!child) {
        if (setpgid(0, 0)) { perror("child setpgid"); _exit(126); }
        char *args[] = { path, number, NULL };
        char *env[] = { "PATH=/bin:/usr/bin", "HOME=/root", NULL };
        execve(path, args, env);
        perror(path);
        _exit(127);
    }
    if (setpgid(child, child) && errno != EACCES && errno != ESRCH) {
        kill(child, SIGKILL);
        (void)bench_wait(child);
        bench_die("parent setpgid");
    }
    int status = 0, timed_out = 0, wait_failed = 0;
    for (;;) {
        pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) break;
        if (result < 0 && errno != EINTR) { wait_failed = 1; break; }
        if (stop_requested) { wait_failed = 1; break; }
        uint64_t now = bench_now();
        if (now < start || now - start >= timeout_seconds * 1000000000) {
            timed_out = 1;
            break;
        }
        struct timespec delay = { .tv_nsec = 10000000 };
        if (nanosleep(&delay, NULL) && errno != EINTR) { wait_failed = 1; break; }
    }
    if (timed_out || wait_failed) {
        kill(-child, SIGKILL);
        kill(child, SIGKILL);
        pid_t result;
        do { result = waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
        if (result != child) wait_failed = 1;
    }
    /* Also stop any surviving descendants of a failed payload. PID 1 adopts
     * them in the guest, so reap those that are already dead without blocking. */
    int failed = timed_out || wait_failed || !WIFEXITED(status) || WEXITSTATUS(status);
    if (failed) {
        kill(-child, SIGKILL);
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
    printf("AZBENCH {\"type\":\"end\",\"id\":%" PRIu64 ",\"child_pid\":%ld,\"status\":%d,"
           "\"timed_out\":%s,\"passed\":%s}\n", id,
           (long)child, status, timed_out ? "true" : "false", failed ? "false" : "true");
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 1 && argc != 3) {
        fprintf(stderr, "usage: %s [config-file binary-directory]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction action = { .sa_handler = request_stop };
    if (sigemptyset(&action.sa_mask) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGINT, &action, NULL)) bench_die("supervisor sigaction");
    const char *config = argc == 3 ? argv[1] : "/etc/azbench.conf";
    const char *directory = argc == 3 ? argv[2] : "/bench";
    FILE *file = fopen(config, "r");
    if (!file) bench_die("benchmark config");
    /* repetitions, warm-up rounds, syscall calls, pipe round trips, mmap MiB,
     * fork iterations, per-child timeout seconds. No shell is involved. */
    uint64_t values[7], maximum[] = {1000, 100, 1000000000, 100000000, 65536, 1000000, 86400};
    for (size_t i = 0; i < 7; i++) {
        char text[32];
        if (fscanf(file, "%31s", text) != 1) {
            fprintf(stderr, "incomplete benchmark config\n"); fclose(file); return 2;
        }
        values[i] = bench_number(text, maximum[i]);
    }
    char extra[2];
    if (fscanf(file, "%1s", extra) != EOF || ferror(file)) {
        fprintf(stderr, "trailing data in benchmark config\n"); fclose(file); return 2;
    }
    if (fclose(file)) bench_die("config fclose");
    environment();
    const char *names[] = {"syscall", "pipe", "mmap", "fork"};
    int failed = 0;
    uint64_t id = 0;
    for (uint64_t round = 0; round < values[0] + values[1] && !failed; round++) {
        for (size_t i = 0; i < 4; i++) {
            if (stop_requested) { failed = 1; break; }
            failed = run_child(directory, names[i], values[i + 2], values[6],
                               id++, round < values[1]);
            if (failed) break;
        }
    }
    printf("AZBENCH {\"type\":\"suite_end\",\"cases\":%" PRIu64 ",\"passed\":%s}\n",
           id, failed ? "false" : "true");
    return failed ? 1 : 0;
}
