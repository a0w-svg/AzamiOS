#include "workqueue.h"
#include <kernel/kthread.h>
#include <kernel/mm/kmalloc.h>
#include <arch/x86_64/cpu/spinlock.h>
#include <kernel/sched/sched.h>
#include <kernel/time/timekeeping.h>
#include <kernel/lib/string.h>

struct workqueue_struct {
    const char *name;
    spinlock_t lock;
    struct work_struct *head;
    struct work_struct *tail;
    struct delayed_work *delayed_head;
    struct task_struct *worker_thread;
    bool destroying;
};

static struct workqueue_struct *system_wq;

static void queue_work_locked(struct workqueue_struct *wq, struct work_struct *work) {
    work->next = NULL;
    if (wq->tail) {
        wq->tail->next = work;
    } else {
        wq->head = work;
    }
    wq->tail = work;
}

static int worker_thread_func(void *data) {
    struct workqueue_struct *wq = data;
    
    while (!kthread_should_stop()) {
        unsigned long flags;
        struct work_struct *work = NULL;
        
        flags = spinlock_lock_irqsave(&wq->lock);
        
        /* Process delayed works */
        u64 now = ktime_get_ns();
        struct delayed_work *prev = NULL;
        struct delayed_work *dw = wq->delayed_head;
        while (dw) {
            if (now >= dw->trigger_time) {
                struct delayed_work *expired = dw;
                if (prev) prev->timer_next = dw->timer_next;
                else wq->delayed_head = dw->timer_next;
                dw = dw->timer_next;
                
                queue_work_locked(wq, &expired->work);
            } else {
                prev = dw;
                dw = dw->timer_next;
            }
        }
        
        /* Pop normal work */
        if (wq->head) {
            work = wq->head;
            wq->head = work->next;
            if (!wq->head) wq->tail = NULL;
        }
        
        spinlock_unlock_irqrestore(&wq->lock, flags);
        
        if (work) {
            work->func(work);
        } else {
            if (wq->destroying) break;
            sched_sleep(1); /* Wait for condition */
        }
    }
    return 0;
}

struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags, int max_active) {
    struct workqueue_struct *wq = kzalloc(sizeof(*wq));
    if (!wq) return NULL;
    wq->name = name;
    wq->lock = (spinlock_t)SPINLOCK_INIT;
    wq->worker_thread = kthread_run(worker_thread_func, wq, name);
    return wq;
}

void destroy_workqueue(struct workqueue_struct *wq) {
    if (!wq) return;
    wq->destroying = true;
    if (wq->worker_thread) {
        kthread_stop(wq->worker_thread);
    }
    kfree(wq);
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
    unsigned long flags;
    flags = spinlock_lock_irqsave(&wq->lock);
    queue_work_locked(wq, work);
    spinlock_unlock_irqrestore(&wq->lock, flags);
    return true;
}

bool queue_delayed_work(struct workqueue_struct *wq, struct delayed_work *dwork, u64 delay_ns) {
    unsigned long flags;
    flags = spinlock_lock_irqsave(&wq->lock);
    
    dwork->delay_ns = delay_ns;
    dwork->trigger_time = ktime_get_ns() + delay_ns;
    
    dwork->timer_next = wq->delayed_head;
    wq->delayed_head = dwork;
    
    spinlock_unlock_irqrestore(&wq->lock, flags);
    return true;
}

bool schedule_work(struct work_struct *work) {
    return queue_work(system_wq, work);
}

bool schedule_delayed_work(struct delayed_work *dwork, u64 delay_ns) {
    return queue_delayed_work(system_wq, dwork, delay_ns);
}

bool cancel_work_sync(struct work_struct *work) {
    flush_workqueue(system_wq);
    return true;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork) {
    flush_workqueue(system_wq);
    return true;
}

void flush_workqueue(struct workqueue_struct *wq) {
    while (1) {
        unsigned long flags;
        flags = spinlock_lock_irqsave(&wq->lock);
        bool empty = (wq->head == NULL && wq->delayed_head == NULL);
        spinlock_unlock_irqrestore(&wq->lock, flags);
        if (empty) break;
        sched_sleep(1);
    }
}

void flush_scheduled_work(void) {
    flush_workqueue(system_wq);
}

void workqueue_init(void) {
    system_wq = alloc_workqueue("events", 0, 1);
}
