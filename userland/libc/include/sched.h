/* ============================================================================
 * AzamiOS Userspace — Execution Scheduling (sched.h)
 * File: userland/libc/include/sched.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "time.h"
#include <string.h>

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

#define CPU_SETSIZE 1024
typedef struct {
    unsigned long __bits[CPU_SETSIZE / (8 * sizeof(unsigned long))];
} cpu_set_t;

#define CPU_ZERO(cpusetp) memset((cpusetp), 0, sizeof(cpu_set_t))
#define CPU_SET(cpu, cpusetp)   ((cpusetp)->__bits[(cpu) / (8 * sizeof(unsigned long))] |= (1UL << ((cpu) % (8 * sizeof(unsigned long)))))
#define CPU_CLR(cpu, cpusetp)   ((cpusetp)->__bits[(cpu) / (8 * sizeof(unsigned long))] &= ~(1UL << ((cpu) % (8 * sizeof(unsigned long)))))
#define CPU_ISSET(cpu, cpusetp) (!!((cpusetp)->__bits[(cpu) / (8 * sizeof(unsigned long))] & (1UL << ((cpu) % (8 * sizeof(unsigned long))))))

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
int sched_rr_get_interval(pid_t pid, struct timespec *tp);
