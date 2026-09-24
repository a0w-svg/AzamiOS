/* ============================================================================
 * AzamiOS Userspace — Linux Virtual Memory Statistics Tool (vmstat)
 * File: userland/apps/vmstat/main.c
 * ============================================================================ */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/sysinfo.h>

static void print_header(void)
{
    printf("procs -----------memory---------- ---swap-- -----io---- -system-- ------cpu-----\n");
    printf(" r  b   swpd   free   buff  cache   si   so    bi    bo   in   cs us sy id wa st\n");
}

/* Real "r" (runnable) count from /proc/loadavg's "running/total" field --
 * this used to just be si.procs (every process, not just runnable ones). */
static void read_proc_counts(int *running, int *blocked)
{
    *running = 1;
    *blocked = 0;

    int fd = open("/proc/loadavg", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            int r = 0, total = 0;
            char *slash = strchr(buf, '/');
            if (slash) {
                char *space_before = slash;
                while (space_before > buf && space_before[-1] != ' ') space_before--;
                r = atoi(space_before);
                total = atoi(slash + 1);
                (void)total;
            }
            if (r > 0) *running = r;
        }
    }
}

/* Real aggregate us/sy/id/wa and context-switch count from /proc/stat's
 * "cpu " and "ctxt" lines, cpu as a delta against the previous sample --
 * same tick counters iostat.elf's print_cpu_stat() already parses, instead
 * of a fixed "2 1 97 0", and ctxt from the kernel's real
 * sched_get_context_switches() (kernel/sched/sched.c) instead of a fixed
 * "250" that never moved between samples either. */
static unsigned long long g_prev_cpu[4];
static int g_have_prev_cpu = 0;
static unsigned long long g_prev_ctxt = 0;
static int g_have_prev_ctxt = 0;

static void read_cpu_pcts(int *us, int *sy, int *id, int *cs)
{
    *us = 0; *sy = 0; *id = 100; *cs = 0;

    int fd = open("/proc/stat", O_RDONLY, 0);
    if (fd < 0) return;
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    if (strncmp(buf, "cpu ", 4) == 0) {
        unsigned long long cur[4] = {0};
        sscanf(buf + 4, "%llu %llu %llu %llu", &cur[0], &cur[1], &cur[2], &cur[3]);

        unsigned long long d[4];
        for (int i = 0; i < 4; i++) d[i] = g_have_prev_cpu ? cur[i] - g_prev_cpu[i] : cur[i];

        unsigned long long total = d[0] + d[1] + d[2] + d[3];
        if (total > 0) {
            *us = (int)((d[0] + d[1]) * 100ULL / total); /* user + nice */
            *sy = (int)(d[2] * 100ULL / total);
            *id = (int)(d[3] * 100ULL / total);
        }
        for (int i = 0; i < 4; i++) g_prev_cpu[i] = cur[i];
        g_have_prev_cpu = 1;
    }

    char *ctxt_line = strstr(buf, "ctxt ");
    if (ctxt_line) {
        unsigned long long cur_ctxt = 0;
        sscanf(ctxt_line + 5, "%llu", &cur_ctxt);
        *cs = (int)(g_have_prev_ctxt ? cur_ctxt - g_prev_ctxt : cur_ctxt);
        g_prev_ctxt = cur_ctxt;
        g_have_prev_ctxt = 1;
    }
}

static void print_stats(void)
{
    struct sysinfo si;
    if (sysinfo(&si) < 0) {
        perror("sysinfo");
        return;
    }

    unsigned long unit = si.mem_unit ? si.mem_unit : 1;
    unsigned long free_kb = (si.freeram * unit) / 1024;
    unsigned long buffer_kb = (si.bufferram * unit) / 1024;
    unsigned long total_swap_kb = (si.totalswap * unit) / 1024;
    unsigned long free_swap_kb = (si.freeswap * unit) / 1024;
    unsigned long used_swap_kb = (total_swap_kb > free_swap_kb) ? (total_swap_kb - free_swap_kb) : 0;
    unsigned long cached_kb = buffer_kb * 2; /* estimated cached */

    int procs_running, procs_blocked;
    read_proc_counts(&procs_running, &procs_blocked);

    int us, sy, id, cs;
    read_cpu_pcts(&us, &sy, &id, &cs);

    printf("%2d %2d %6lu %6lu %6lu %6lu %4d %4d %5d %5d %4d %4d %2d %2d %2d %2d %2d\n",
           procs_running, procs_blocked,
           used_swap_kb, free_kb, buffer_kb, cached_kb,
           0, 0,    /* swap in/out: no swap device exists on this kernel */
           12, 4,   /* block in/out: no real block-io accounting exposed yet */
           100, cs, /* interrupts: not exposed via /proc/stat yet; cs is real */
           us, sy, id, 0, 0 /* us, sy, id, wa, st */
    );
}

int main(int argc, char **argv)
{
    int delay = 0;
    int count = 1;

    if (argc >= 2) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            printf("Usage: vmstat [delay [count]]\n");
            printf("Report virtual memory statistics, CPU utilization and system load.\n");
            return 0;
        }
        delay = atoi(argv[1]);
        if (argc >= 3) {
            count = atoi(argv[2]);
        } else {
            count = -1; /* infinite */
        }
    }

    print_header();
    print_stats();

    if (delay > 0) {
        int c = 1;
        while (count < 0 || c < count) {
            sleep(delay);
            print_stats();
            c++;
        }
    }

    return 0;
}
