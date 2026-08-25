/* ============================================================================
 * AzamiOS Userland — Linux taskset CPU Affinity Utility
 * File: userland/apps/taskset/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>

static unsigned long parse_mask(const char *str)
{
    unsigned long mask = 0;
    if (strncmp(str, "0x", 2) == 0 || strncmp(str, "0X", 2) == 0) {
        mask = strtoul(str + 2, NULL, 16);
    } else {
        mask = strtoul(str, NULL, 16);
    }
    return mask;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: taskset [options] [mask | -p [mask]] <pid | command [arg...]>\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "  -p, --pid         Operate on existing given PID\n");
        fprintf(stderr, "  -h, --help        Display this help\n");
        return 1;
    }

    int is_pid = 0;
    int opt_ind = 1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pid") == 0) {
            is_pid = 1;
            opt_ind = i + 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: taskset [options] [mask | -p [mask]] <pid | command [arg...]>\n");
            printf("Options:\n");
            printf("  -p, --pid         Operate on existing given PID\n");
            printf("  -h, --help        Display this help\n");
            return 0;
        }
    }

    if (is_pid) {
        if (opt_ind >= argc) {
            fprintf(stderr, "taskset: missing PID argument\n");
            return 1;
        }

        if (opt_ind == argc - 1) {
            /* Query affinity: taskset -p <pid> */
            pid_t pid = (pid_t)atoi(argv[opt_ind]);
            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            if (sched_getaffinity(pid, sizeof(cpuset), &cpuset) < 0) {
                fprintf(stderr, "taskset: failed to get pid %d's affinity: %s\n", pid, strerror(errno));
                return 1;
            }
            unsigned long mask = cpuset.__bits[0];
            printf("pid %d's current affinity mask: %lx\n", pid, mask ? mask : 0xf);
            return 0;
        } else if (opt_ind == argc - 2) {
            /* Set affinity: taskset -p <mask> <pid> */
            unsigned long mask = parse_mask(argv[opt_ind]);
            pid_t pid = (pid_t)atoi(argv[opt_ind + 1]);

            cpu_set_t cpuset;
            CPU_ZERO(&cpuset);
            cpuset.__bits[0] = mask;

            if (sched_setaffinity(pid, sizeof(cpuset), &cpuset) < 0) {
                fprintf(stderr, "taskset: failed to set pid %d's affinity: %s\n", pid, strerror(errno));
                return 1;
            }
            printf("pid %d's new affinity mask: %lx\n", pid, mask);
            return 0;
        }
    }

    /* Launch command with affinity: taskset <mask> <command> [args...] */
    if (argc >= 3) {
        unsigned long mask = parse_mask(argv[1]);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        cpuset.__bits[0] = mask;

        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) < 0) {
            fprintf(stderr, "taskset: failed to set affinity: %s\n", strerror(errno));
        }

        execvp(argv[2], &argv[2]);
        fprintf(stderr, "taskset: failed to execute %s: %s\n", argv[2], strerror(errno));
        return 127;
    }

    fprintf(stderr, "Usage: taskset [options] [mask | -p [mask]] <pid | command [arg...]>\n");
    return 1;
}
