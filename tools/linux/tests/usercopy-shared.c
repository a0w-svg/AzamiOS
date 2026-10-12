/* Shared-open-description read transactions, with four CPU-pinned readers.
 * Each result is one 64-byte atomic pipe write; no EOF dependency on teardown.
 * The default uses an ordinary regular file: the Linux host does not serialize
 * the offset of a directly created memfd, unlike ordinary regular-file opens.
 * USERCOPY_SHARED_MEMFD=1 selects additional Azami memfd concurrency stress;
 * that mode does not claim to be a matching Linux shared-position oracle. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CHILDREN = 4, RECORDS = 4096, PER_READ = 8 };
#define RECORD_MAGIC UINT64_C(0xa5b6000000000000)
#define OWNER_MASK (UINT64_C(3) << 32)
static unsigned passed, failed;

static void check(const char *name, bool good)
{
    printf("%s %s\n", good ? "PASS" : "FAIL", name);
    if (good) passed++; else failed++;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void pause_poll(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 };
    nanosleep(&ts, NULL);
}

static bool write_all(int fd, const void *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const char *)buf + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

static bool gate_wait(int fd)
{
    char byte;
    ssize_t n;
    do { n = read(fd, &byte, 1); } while (n < 0 && errno == EINTR);
    return n == 1;
}

/* Returns 1 for one complete transaction, 0 at EOF, -1 on any bad result. */
static int transfer(int data, int result, unsigned owner, int64_t deadline)
{
    uint64_t packet[PER_READ];
    ssize_t n;
    do { n = read(data, packet, sizeof(packet)); } while (n < 0 && errno == EINTR);
    if (!n) return 0;
    if (n != (ssize_t)sizeof(packet)) return -1;
    for (unsigned i = 0; i < PER_READ; i++)
        packet[i] |= (uint64_t)owner << 32;
    for (;;) {
        n = write(result, packet, sizeof(packet));
        if (n == (ssize_t)sizeof(packet)) return 1;
        if (n >= 0 || (errno != EAGAIN && errno != EINTR)) return -1;
        int64_t now = now_ns();
        if (now < 0 || now >= deadline) return -1;
        pause_poll();
    }
}

static void child_run(int data, int result, unsigned owner, int cpu,
                      int start, int resume, int64_t deadline)
{
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    int status = 0;
    if (sched_setaffinity(0, sizeof(affinity), &affinity) || !gate_wait(start)) {
        status = 1;
    } else if (transfer(data, result, owner, deadline) != 1 || !gate_wait(resume)) {
        status = 2;
    } else {
        for (;;) {
            int rc = transfer(data, result, owner, deadline);
            if (!rc) break;
            if (rc < 0) { status = 3; break; }
            int64_t now = now_ns();
            if (now < 0 || now >= deadline) { status = 4; break; }
        }
    }
    /* Explicit close precedes exit; deferred process destruction cannot retain
     * this child's result writer while the supervisor counts the records. */
    close(result);
    close(data);
    close(start);
    close(resume);
    _exit(status);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    int cpus[CHILDREN];
    unsigned selected = 0;
    if (!sched_getaffinity(0, sizeof(allowed), &allowed)) {
        for (int i = 0; i < CPU_SETSIZE && selected < CHILDREN; i++)
            if (CPU_ISSET(i, &allowed)) cpus[selected++] = i;
    }
    int data = -1, results[2] = {-1, -1}, start[2] = {-1, -1}, resume[2] = {-1, -1};
    pid_t children[CHILDREN] = {0};
    bool reaped[CHILDREN] = {false};
    bool setup = selected == CHILDREN;
    int64_t begin = now_ns();
    int64_t deadline = begin < 0 ? 0 : begin + INT64_C(10000000000);
    setup = setup && begin >= 0;
    uint64_t records[RECORDS];
    for (unsigned i = 0; i < RECORDS; i++) records[i] = RECORD_MAGIC | i;
    bool use_memfd = getenv("USERCOPY_SHARED_MEMFD") != NULL;
    char path[] = "/tmp/azami-usercopy-shared-XXXXXX";
    bool temporary_file = false;
    if (setup) {
        if (use_memfd) data = memfd_create("usercopy-shared", MFD_CLOEXEC);
        else {
            data = mkstemp(path);
            temporary_file = data >= 0;
        }
        setup = data >= 0 && write_all(data, records, sizeof(records)) &&
                lseek(data, 0, SEEK_SET) == 0 &&
                pipe2(results, O_NONBLOCK | O_CLOEXEC) == 0 &&
                pipe2(start, O_CLOEXEC) == 0 && pipe2(resume, O_CLOEXEC) == 0;
    }
    unsigned spawned = 0;
    if (setup) {
        for (unsigned i = 0; i < CHILDREN; i++) {
            pid_t pid = fork();
            if (pid < 0) { setup = false; break; }
            if (!pid) {
                close(results[0]);
                close(start[1]);
                close(resume[1]);
                child_run(data, results[1], i, cpus[i], start[0], resume[0], deadline);
            }
            children[i] = pid;
            spawned++;
        }
    }
    if (results[1] >= 0) { close(results[1]); results[1] = -1; }
    if (start[0] >= 0) { close(start[0]); start[0] = -1; }
    if (resume[0] >= 0) { close(resume[0]); resume[0] = -1; }
    const char tokens[CHILDREN] = {1, 1, 1, 1};
    if (setup) setup = write_all(start[1], tokens, sizeof(tokens));
    if (start[1] >= 0) { close(start[1]); start[1] = -1; }

    bool seen[RECORDS] = {false};
    size_t received = 0, buffered = 0;
    uint64_t packet[PER_READ];
    unsigned owner_records[CHILDREN] = {0};
    unsigned owner_mask = 0, completed = 0;
    bool unique = true, exits = setup, released = false, timed_out = false;
    while (setup) {
        for (unsigned i = 0; i < spawned; i++) {
            if (reaped[i]) continue;
            int status = 0;
            pid_t rc = waitpid(children[i], &status, WNOHANG);
            if (rc == children[i]) {
                reaped[i] = true;
                completed++;
                if (!WIFEXITED(status) || WEXITSTATUS(status)) exits = false;
            } else if (rc < 0 && errno != EINTR) {
                exits = false;
                reaped[i] = true;
                completed++;
            }
        }
        ssize_t n = read(results[0], (char *)packet + buffered, sizeof(packet) - buffered);
        if (n > 0) {
            buffered += (size_t)n;
            if (buffered == sizeof(packet)) {
                for (unsigned i = 0; i < PER_READ; i++) {
                    unsigned owner = (unsigned)((packet[i] & OWNER_MASK) >> 32);
                    uint64_t raw = packet[i] & ~OWNER_MASK;
                    uint64_t index = raw & UINT64_C(0xffffffff);
                    received++;
                    owner_records[owner]++;
                    owner_mask |= 1U << owner;
                    if (index >= RECORDS || raw != (RECORD_MAGIC | index)) unique = false;
                    else if (seen[index]) unique = false;
                    else seen[index] = true;
                }
                buffered = 0;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            exits = false;
            break;
        } else if (completed == spawned && (n == 0 || (n < 0 && errno == EAGAIN))) {
            /* Count data, never wait for pipe EOF after process teardown. */
            break;
        } else {
            pause_poll();
        }
        if (!released && owner_mask == (1U << CHILDREN) - 1) {
            released = true;
            if (!write_all(resume[1], tokens, sizeof(tokens))) {
                exits = false;
                break;
            }
            close(resume[1]); resume[1] = -1;
        }
        int64_t now = now_ns();
        if (now < 0 || now >= deadline) { timed_out = true; exits = false; break; }
    }
    if (resume[1] >= 0) { close(resume[1]); resume[1] = -1; }
    /* Supervise failures without an unbounded waitpid on a broken guest. */
    for (unsigned i = 0; i < spawned; i++)
        if (!reaped[i]) kill(children[i], SIGKILL);
    int64_t cleanup_start = now_ns();
    int64_t cleanup_deadline = cleanup_start < 0 ? 0 : cleanup_start + INT64_C(2000000000);
    while (completed < spawned) {
        for (unsigned i = 0; i < spawned; i++) {
            if (reaped[i]) continue;
            int status;
            pid_t rc = waitpid(children[i], &status, WNOHANG);
            if (rc == children[i] || (rc < 0 && errno == ECHILD)) {
                reaped[i] = true; completed++;
            }
        }
        int64_t now = now_ns();
        if (now < 0 || now >= cleanup_deadline) break;
        pause_poll();
    }
    for (unsigned i = 0; i < RECORDS; i++) if (!seen[i]) unique = false;
    for (unsigned i = 0; i < CHILDREN; i++) if (!owner_records[i]) exits = false;
    if (data >= 0) close(data);
    if (temporary_file) unlink(path);
    if (results[0] >= 0) close(results[0]);
    printf("INFO shared backend=%s readers=%u records=%zu owner_records=%u,%u,%u,%u timeout=%u\n",
           use_memfd ? "memfd" : "regular", spawned, received, owner_records[0], owner_records[1], owner_records[2], owner_records[3], timed_out);
    check("shared description record count", setup && received == RECORDS && !buffered && !timed_out);
    check("shared description unique complete records", setup && unique);
    check("four pinned readers exit successfully", exits && spawned == CHILDREN && completed == CHILDREN && released);
    printf("== probe complete: %u passed, %u failed ==\n", passed, failed);
    if (getenv("USERCOPY_SHARED_EXIT")) return failed ? 1 : 0;
    for (;;) pause();
}
