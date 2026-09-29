#include "jiffies.h"
#include "sched/sched.h"
#include "timer_list.h"
#include "../include/azami/defs.h"

volatile unsigned long jiffies = 0;

void jiffies_init(void)
{
    jiffies = 0;
}

void jiffies_tick(void)
{
    jiffies++;
    run_timer_list();
}

void ndelay(unsigned int nsecs)
{
    u64 start = ktime_get_ns();
    while (ktime_get_ns() - start < nsecs) {
        cpu_pause();
    }
}

void udelay(unsigned int usecs)
{
    ndelay(usecs * 1000);
}

void mdelay(unsigned int msecs)
{
    udelay(msecs * 1000);
}

void msleep(unsigned int msecs)
{
    sched_sleep(msecs_to_jiffies(msecs));
}

void usleep_range(unsigned long min, unsigned long max)
{
    (void)max;
    if (min >= 1000) {
        msleep(min / 1000);
    } else {
        udelay(min);
    }
}
