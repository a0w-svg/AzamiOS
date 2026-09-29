#pragma once
#include <azami/types.h>
#include <azami/defs.h>

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
    work_func_t func;
    struct work_struct *next;  /* queue linkage */
    unsigned long flags;
};

struct delayed_work {
    struct work_struct work;
    u64 delay_ns;       /* delay in nanoseconds */
    u64 trigger_time;   /* absolute time to execute */
    struct delayed_work *timer_next;  /* timer list linkage */
};

struct workqueue_struct;

/* Init macros */
#define INIT_WORK(w, f) do { (w)->func = (f); (w)->next = NULL; (w)->flags = 0; } while(0)
#define INIT_DELAYED_WORK(dw, f) do { INIT_WORK(&(dw)->work, (f)); (dw)->delay_ns = 0; (dw)->trigger_time = 0; (dw)->timer_next = NULL; } while(0)
#define DECLARE_WORK(n, f) struct work_struct n = { .func = (f), .next = NULL, .flags = 0 }

/* System workqueue */
bool schedule_work(struct work_struct *work);
bool schedule_delayed_work(struct delayed_work *dwork, u64 delay_ns);
bool cancel_work_sync(struct work_struct *work);
bool cancel_delayed_work_sync(struct delayed_work *dwork);
void flush_scheduled_work(void);

/* Custom workqueues */
struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags, int max_active);
void destroy_workqueue(struct workqueue_struct *wq);
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, u64 delay_ns);
void flush_workqueue(struct workqueue_struct *wq);

/* Flags */
#define WQ_UNBOUND    (1 << 0)
#define WQ_HIGHPRI    (1 << 1)
#define WQ_MEM_RECLAIM (1 << 2)

/* Global init */
void workqueue_init(void);
