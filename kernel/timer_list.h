#pragma once
#include "../include/azami/types.h"
#include "../include/azami/defs.h"
#include "jiffies.h"

struct timer_list {
    struct timer_list *next;
    unsigned long expires;       /* absolute jiffies value */
    void (*function)(struct timer_list *);
    unsigned int flags;
    bool pending;               /* timer is in the queue */
};

#define TIMER_FLAG_DEFERRABLE  (1 << 0)

/* Initialize a timer (Linux 4.15+ API) */
#define timer_setup(timer, callback, flags) \
    __timer_setup((timer), (callback), (flags))
void __timer_setup(struct timer_list *timer, void (*function)(struct timer_list *), unsigned int flags);

/* from_timer - get container struct from timer */
#define from_timer(var, callback_timer, timer_fieldname) \
    container_of(callback_timer, typeof(*var), timer_fieldname)

/* Modify/set expiry and activate the timer */
int mod_timer(struct timer_list *timer, unsigned long expires);

/* Add a timer (must set expires before calling) */
void add_timer(struct timer_list *timer);

/* Delete a timer */
int del_timer(struct timer_list *timer);

/* Delete and wait for handler completion */
int del_timer_sync(struct timer_list *timer);

/* Check if timer is pending */
int timer_pending(const struct timer_list *timer);

/* Process expired timers - called from jiffies_tick() */
void run_timer_list(void);

/* Init the timer subsystem */
void timer_list_init(void);
