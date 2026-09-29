#pragma once
#include "../include/azami/types.h"
#include "time/timekeeping.h"

/* HZ = tick rate (100 Hz matches AzamiOS's TK_HZ) */
#define HZ 100

/* jiffies = the system tick counter */
extern volatile unsigned long jiffies;

/* Time conversion macros */
#define msecs_to_jiffies(m)   ((unsigned long)((u64)(m) * HZ / 1000))
#define jiffies_to_msecs(j)   ((unsigned int)((u64)(j) * 1000 / HZ))
#define usecs_to_jiffies(u)   ((unsigned long)((u64)(u) * HZ / 1000000))
#define jiffies_to_usecs(j)   ((unsigned int)((u64)(j) * 1000000 / HZ))
#define nsecs_to_jiffies(n)   ((unsigned long)((u64)(n) * HZ / 1000000000ULL))

/* Time comparison macros (handles wraparound) */
#define time_after(a, b)      ((long)((b) - (a)) < 0)
#define time_before(a, b)     time_after(b, a)
#define time_after_eq(a, b)   ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b)  time_after_eq(b, a)
#define time_in_range(a, b, c) (time_after_eq(a, b) && time_before_eq(a, c))

/* Delay helpers */
void mdelay(unsigned int msecs);
void udelay(unsigned int usecs);
void ndelay(unsigned int nsecs);
void msleep(unsigned int msecs);
void usleep_range(unsigned long min, unsigned long max);

/* Initialize jiffies from scheduler tick counter */
void jiffies_init(void);
/* Called from timer tick to increment jiffies */
void jiffies_tick(void);
