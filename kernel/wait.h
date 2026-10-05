#pragma once
#include "../include/azami/types.h"
#include "../arch/x86_64/cpu/spinlock.h"
#include "jiffies.h"

struct wait_queue_entry {
    void *private;                     /* the sleeping thread */
    struct wait_queue_entry *next;
};

struct wait_queue_head {
    spinlock_t lock;
    struct wait_queue_entry *first;
};

typedef struct wait_queue_head wait_queue_head_t;

#define DECLARE_WAIT_QUEUE_HEAD(name) \
    wait_queue_head_t name = { .lock = SPINLOCK_INIT, .first = NULL }

#define init_waitqueue_head(wqh) do { \
    spinlock_init(&(wqh)->lock); \
    (wqh)->first = NULL; \
} while(0)

void wake_up(wait_queue_head_t *wq);
void wake_up_all(wait_queue_head_t *wq);
void wake_up_interruptible(wait_queue_head_t *wq);
void wake_up_interruptible_all(wait_queue_head_t *wq);

void __wait_event_sleep(wait_queue_head_t *wq);
void __wait_event_sleep_timeout(wait_queue_head_t *wq, u64 deadline_ns);

/* wait_event - sleep until condition is true */
#define wait_event(wq, condition) \
    do { while (!(condition)) { __wait_event_sleep(&(wq)); } } while(0)

#define wait_event_interruptible(wq, condition) \
    ({ int __ret = 0; while (!(condition)) { __wait_event_sleep(&(wq)); } __ret; })

/* timeout is in milliseconds. The deadline uses the monotonic clock because
 * jiffies is not advanced by the scheduler tick in this kernel. */
#define wait_event_timeout(wq, condition, timeout) \
    ({ u64 __deadline = ktime_get_ns() + (u64)(timeout) * 1000000ULL; \
       while (!(condition) && ktime_get_ns() < __deadline) \
           __wait_event_sleep_timeout(&(wq), __deadline); \
       (condition) ? 1 : 0; })

/* Waitqueue init */
void wait_queue_init(void);
