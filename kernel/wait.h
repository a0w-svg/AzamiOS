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

/* wait_event - sleep until condition is true */
#define wait_event(wq, condition) \
    do { while (!(condition)) { __wait_event_sleep(&(wq)); } } while(0)

#define wait_event_interruptible(wq, condition) \
    ({ int __ret = 0; while (!(condition)) { __wait_event_sleep(&(wq)); } __ret; })

#define wait_event_timeout(wq, condition, timeout) \
    ({ unsigned long __ret = (timeout); \
       unsigned long __end = jiffies + __ret; \
       while (!(condition) && time_before(jiffies, __end)) { __wait_event_sleep(&(wq)); } \
       (condition) ? (__ret ?: 1) : 0; })

/* Waitqueue init */
void wait_queue_init(void);
