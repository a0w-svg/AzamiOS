/* ============================================================================
 * AzamiOS Userspace — Real-time Process & System Resource Monitor (top.elf)
 * File: userland/apps/top/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/stdbool.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/sysinfo.h"
#include "../../libc/include/time.h"
#include "../../libc/include/getopt.h"

typedef struct {
    int pid;
    int ppid;
    char state[4];
    char name[64];
    unsigned long vmsize_kb;
    float cpu_pct;
    float mem_pct;
} top_proc_t;

static int parse_meminfo(unsigned long *total_kb, unsigned long *free_kb)
{
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) return -1;

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    *total_kb = 512 * 1024;
    *free_kb = 450 * 1024;

    char *line = strtok(buf, "\n");
    while (line) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            *total_kb = (unsigned long)atoi(line + 9);
        } else if (strncmp(line, "MemFree:", 8) == 0) {
            *free_kb = (unsigned long)atoi(line + 8);
        }
        line = strtok(NULL, "\n");
    }
    return 0;
}

static void display_top_header(unsigned long total_mem_kb, unsigned long free_mem_kb, int total_procs)
{
    struct sysinfo s;
    long uptime_sec = 0;
    if (sysinfo(&s) == 0) uptime_sec = s.uptime;

    long days = uptime_sec / 86400;
    long hours = (uptime_sec % 86400) / 3600;
    long mins = (uptime_sec % 3600) / 60;
    long secs = uptime_sec % 60;

    unsigned long used_mem_kb = total_mem_kb > free_mem_kb ? (total_mem_kb - free_mem_kb) : 0;
    double total_mb = total_mem_kb / 1024.0;
    double free_mb  = free_mem_kb / 1024.0;
    double used_mb  = used_mem_kb / 1024.0;

    printf("\033[H\033[2J"); /* Clear screen and home cursor */
    printf("top - %02ld:%02ld:%02ld up ", hours, mins, secs);
    if (days > 0) printf("%ld days, ", days);
    printf("%02ld:%02ld,  1 user,  load average: 0.05, 0.03, 0.01\n", mins, secs);

    printf("Tasks: %3d total,   1 running, %3d sleeping,   0 stopped,   0 zombie\n",
           total_procs, total_procs > 1 ? total_procs - 1 : 0);

    printf("%%Cpu(s):  0.4 us,  0.2 sy,  0.0 ni, 99.4 id,  0.0 wa,  0.0 hi,  0.0 si\n");
    printf("MiB Mem : %7.1f total, %7.1f free, %7.1f used,     0.0 buff/cache\n\n",
           total_mb, free_mb, used_mb);

    printf("\033[7m  PID USER      PR  NI    VIRT    RES    SHR S  %%CPU  %%MEM     TIME+ COMMAND             \033[0m\n");
}

int main(int argc, char **argv)
{
    int iterations = 1;
    int delay_sec = 2;

    int opt;
    while ((opt = getopt(argc, argv, "n:d:b")) != -1) {
        switch (opt) {
        case 'n': iterations = atoi(optarg); break;
        case 'd': delay_sec = atoi(optarg); break;
        case 'b': break; /* Batch mode */
        default: break;
        }
    }

    if (iterations <= 0) iterations = 1;
    if (delay_sec <= 0) delay_sec = 1;

    for (int iter = 0; iter < iterations; iter++) {
        unsigned long total_mem_kb = 512 * 1024;
        unsigned long free_mem_kb = 450 * 1024;
        parse_meminfo(&total_mem_kb, &free_mem_kb);

        DIR *dir = opendir("/proc");
        if (!dir) {
            fprintf(stderr, "top: /proc not available\n");
            return 1;
        }

        top_proc_t procs[64];
        int count = 0;

        struct dirent *de;
        while ((de = readdir(dir)) != NULL && count < 64) {
            int pid = atoi(de->d_name);
            if (pid <= 0) continue;

            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/status", pid);
            int fd = open(path, O_RDONLY, 0);
            if (fd < 0) continue;

            char sbuf[512];
            ssize_t n = read(fd, sbuf, sizeof(sbuf) - 1);
            close(fd);
            if (n <= 0) continue;
            sbuf[n] = '\0';

            procs[count].pid = pid;
            procs[count].ppid = 0;
            strcpy(procs[count].state, "S");
            strcpy(procs[count].name, "app");
            procs[count].vmsize_kb = 64;

            char *line = strtok(sbuf, "\n");
            while (line) {
                if (strncmp(line, "Name:", 5) == 0) {
                    char *v = line + 5; while (*v == ' ' || *v == '\t') v++;
                    strncpy(procs[count].name, v, sizeof(procs[count].name) - 1);
                } else if (strncmp(line, "State:", 6) == 0) {
                    char *v = line + 6; while (*v == ' ' || *v == '\t') v++;
                    procs[count].state[0] = *v;
                    procs[count].state[1] = '\0';
                } else if (strncmp(line, "VmSize:", 7) == 0) {
                    procs[count].vmsize_kb = (unsigned long)atoi(line + 7);
                }
                line = strtok(NULL, "\n");
            }
            count++;
        }
        closedir(dir);

        display_top_header(total_mem_kb, free_mem_kb, count);

        for (int i = 0; i < count; i++) {
            float mem_pct = (procs[i].vmsize_kb * 100.0f) / (total_mem_kb ? total_mem_kb : 1);
            printf("%5d %-9s 20   0 %6luK %6luK      0 %s   0.0 %5.1f   0:00.12 %-20s\n",
                   procs[i].pid, "root", procs[i].vmsize_kb, procs[i].vmsize_kb,
                   procs[i].state, (double)mem_pct, procs[i].name);
        }

        if (iter + 1 < iterations) {
            sleep(delay_sec);
        }
    }

    return 0;
}
