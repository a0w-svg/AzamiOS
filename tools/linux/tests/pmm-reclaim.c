/* Stock-musl probe for Azami's read-only deferred PMM reclaim counters.
 * Run with azami.pmm_selftest=1: the boot test leaves a real OOM notification
 * pending so this checks scheduled execution, not just a direct test sweep. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const names[] = {
    "worker_started", "pending_zones", "pressure_batches", "sweeps",
    "worker_cycles", "worker_sweeps", "reclaimed_pages", "callbacks",
    "lock_busy", "rpc_busy", "rpc_timeouts", "time_unavailable",
    "offline_cpus", "start_failures"
};
enum { FIELDS = sizeof(names) / sizeof(names[0]) };
static unsigned passed, failed;

static void check(const char *name, bool ok)
{
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) passed++; else failed++;
}

static bool snapshot(unsigned long long values[FIELDS])
{
    char buf[4096];
    int fd = open("/proc/pmm_reclaim", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t used = 0;
    for (;;) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return false; }
        if (!n) break;
        used += (size_t)n;
        if (used == sizeof(buf) - 1) { close(fd); return false; }
    }
    close(fd);
    buf[used] = 0;
    unsigned seen = 0;
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char key[64], extra;
        unsigned long long value;
        if (sscanf(line, "%63s %llu %c", key, &value, &extra) != 2) return false;
        unsigned field;
        for (field = 0; field < FIELDS && strcmp(key, names[field]); field++) {}
        if (field == FIELDS || (seen & (1U << field))) return false;
        seen |= 1U << field;
        values[field] = value;
    }
    return seen == (1U << FIELDS) - 1 && values[0] <= 1 && values[1] <= 3;
}

static long long now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/proc/pmm_reclaim", O_RDONLY | O_CLOEXEC);
    check("reclaim counters readable", fd >= 0);
    if (fd >= 0) close(fd);
    fd = open("/proc/pmm_reclaim", O_WRONLY | O_CLOEXEC);
    bool readonly = fd < 0 && (errno == EACCES || errno == EROFS || errno == EPERM);
    if (fd >= 0) {
        readonly = write(fd, "1", 1) == -1 &&
                   (errno == EACCES || errno == EROFS || errno == EPERM);
        close(fd);
    }
    check("reclaim counters reject writes", readonly);
    DIR *dir = opendir("/proc");
    bool listed = false;
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)))
            if (!strcmp(entry->d_name, "pmm_reclaim")) listed = true;
        closedir(dir);
    }
    check("reclaim counters listed", listed);
    unsigned long long values[FIELDS] = {0};
    bool valid = snapshot(values);
    long long start = now_ns();
    if (valid && start >= 0) {
        /* Bounded even if the clock stops progressing. */
        for (unsigned attempts = 0; attempts < 250; attempts++) {
            if (values[4] >= 2 && values[5] >= 1) break;
            long long now = now_ns();
            if (now < start || now - start >= 5000000000LL) break;
            struct timespec wait = {.tv_nsec = 20000000};
            if (nanosleep(&wait, NULL) && errno != EINTR) break;
            if (!snapshot(values)) { valid = false; break; }
        }
    }
    check("complete counter schema", valid);
    check("worker started without allocation failure", valid && values[0] == 1 && values[13] == 0);
    check("scheduled worker executed twice", valid && values[4] >= 2);
    check("scheduled worker consumed pressure", valid && values[5] >= 1 && values[2] >= 2 &&
          values[3] >= 2 && values[11] == 0);
    unsigned long long quiet_cycles = 0, quiet_sweeps = 0;
    start = now_ns();
    if (valid && start >= 0) {
        for (unsigned attempts = 0; attempts < 250; attempts++) {
            if (!quiet_cycles && !values[1] && values[4] >= 10) {
                quiet_cycles = values[4];
                quiet_sweeps = values[5];
            }
            if (quiet_cycles && values[4] >= quiet_cycles + 2) break;
            long long now = now_ns();
            if (now < start || now - start >= 5000000000LL) break;
            struct timespec wait = {.tv_nsec = 20000000};
            if (nanosleep(&wait, NULL) && errno != EINTR) break;
            if (!snapshot(values)) { valid = false; break; }
        }
    }
    check("pressure epoch stops without another failure", valid && quiet_cycles >= 10 &&
          values[4] >= quiet_cycles + 2 && values[5] == quiet_sweeps &&
          values[5] >= 1 && values[5] <= 8 && values[1] == 0);
    if (valid) {
        printf("PMMWORKER {");
        for (unsigned i = 0; i < FIELDS; i++)
            printf("%s\"%s\":%llu", i ? "," : "", names[i], values[i]);
        printf(",\"quiescent_cycles\":%llu,\"quiescent_sweeps\":%llu}\n",
               quiet_cycles, quiet_sweeps);
    }
    printf("== PMM worker probe complete: %u failed ==\n", failed);
    printf("== probe complete: %u passed, %u failed ==\n", passed, failed);
    /* PID1 remains alive until the VM runner observes the verdict. */
    for (;;) pause();
}
