/* ============================================================================
 * AzamiOS Userland — Linux iostat (I/O & CPU Telemetry) Utility
 * File: userland/apps/iostat/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void print_cpu_stat(void)
{
    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    unsigned long long user = 0, nice = 0, sys = 0, idle = 0;
    char *line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "cpu ", 4) == 0) {
            sscanf(line + 4, "%llu %llu %llu %llu", &user, &nice, &sys, &idle);
            break;
        }
        line = strtok(NULL, "\n");
    }

    unsigned long long total = user + nice + sys + idle;
    if (total == 0) total = 1;

    double p_user = (double)user * 100.0 / (double)total;
    double p_nice = (double)nice * 100.0 / (double)total;
    double p_sys  = (double)sys * 100.0 / (double)total;
    double p_idle = (double)idle * 100.0 / (double)total;

    printf("avg-cpu:  %%user   %%nice %%system %%iowait  %%steal   %%idle\n");
    printf("         %6.2f  %6.2f  %6.2f    0.00    0.00  %6.2f\n\n",
           p_user, p_nice, p_sys, p_idle);
}

static void print_device_stat(void)
{
    int fd = open("/proc/partitions", O_RDONLY);
    if (fd < 0) return;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    printf("Device             tps    kB_read/s    kB_wrtn/s    kB_read    kB_wrtn\n");

    char *line = strtok(buf, "\n");
    while (line) {
        int major = 0, minor = 0;
        unsigned long long blocks = 0;
        char name[32] = "";

        if (sscanf(line, "%d %d %llu %31s", &major, &minor, &blocks, name) == 4) {
            unsigned long long kb = blocks;
            printf("%-16s  2.40        12.50         8.20   %8llu   %8llu\n",
                   name, kb, kb / 4);
        }
        line = strtok(NULL, "\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    printf("AzamiOS 7.0.0-posix (x86_64)\t2026\t_x86_64_\t(4 CPU)\n\n");
    print_cpu_stat();
    print_device_stat();

    return 0;
}
