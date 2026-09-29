#pragma once

#include <azami/types.h>
#include <azami/defs.h>

struct tasklet_struct {
    struct tasklet_struct *next;
    unsigned long state;  /* TASKLET_STATE_SCHED, TASKLET_STATE_RUN */
    _Atomic unsigned int count;  /* disable count; runs only when 0 */
    void (*func)(unsigned long data);
    unsigned long data;
};

#define TASKLET_STATE_SCHED  0  /* scheduled to run */
#define TASKLET_STATE_RUN    1  /* running on some CPU */

void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long), unsigned long data);
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_hi_schedule(struct tasklet_struct *t);
void tasklet_disable(struct tasklet_struct *t);
void tasklet_enable(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);

/* Process pending tasklets - called from softirq context */
void tasklet_action(void);

/* Bottom half control */
void local_bh_disable(void);
void local_bh_enable(void);
