/* ============================================================================
 * AzamiOS Userspace — Execution Scheduling (sched.h)
 * File: userland/libc/include/sched.h
 * ============================================================================ */
#pragma once

#include "sys/types.h"
#include "time.h"

struct sched_param {
    int sched_priority;
};

#define SCHED_OTHER 0
#define SCHED_FIFO  1
#define SCHED_RR    2

int sched_yield(void);
int sched_get_priority_min(int policy);
int sched_get_priority_max(int policy);
int sched_getparam(pid_t pid, struct sched_param *param);
int sched_setparam(pid_t pid, const struct sched_param *param);
int sched_getscheduler(pid_t pid);
int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param);
