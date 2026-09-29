#include "kthread.h"
#include <kernel/sched/sched.h>
#include <kernel/mm/kmalloc.h>
#include <kernel/lib/string.h>
#include <arch/x86_64/cpu/spinlock.h>

static spinlock_t kthread_lock = SPINLOCK_INIT;
static struct task_struct *kthread_list = NULL;

static void kthread_wrapper(void *arg) {
    struct task_struct *task = (struct task_struct *)arg;
    task->result = task->func(task->data);
    task->stopped = true;
    sched_exit_thread();
}

struct task_struct *kthread_create(kthread_fn_t fn, void *data, const char *name) {
    struct task_struct *task = kzalloc(sizeof(struct task_struct));
    if (!task) return NULL;
    
    task->func = fn;
    task->data = data;
    task->name = name;
    task->should_stop = false;
    task->stopped = false;
    
    task->sched_thread = thread_create_ex(sched_kernel_process(), (uintptr_t)kthread_wrapper, (uintptr_t)task, true, false);
    
    unsigned long flags;
    flags = spinlock_lock_irqsave(&kthread_lock);
    task->next = kthread_list;
    kthread_list = task;
    spinlock_unlock_irqrestore(&kthread_lock, flags);
    
    return task;
}

struct task_struct *kthread_run(kthread_fn_t fn, void *data, const char *name) {
    struct task_struct *task = kzalloc(sizeof(struct task_struct));
    if (!task) return NULL;
    
    task->func = fn;
    task->data = data;
    task->name = name;
    task->should_stop = false;
    task->stopped = false;
    
    task->sched_thread = thread_create(sched_kernel_process(), (uintptr_t)kthread_wrapper, (uintptr_t)task, true);
    
    unsigned long flags;
    flags = spinlock_lock_irqsave(&kthread_lock);
    task->next = kthread_list;
    kthread_list = task;
    spinlock_unlock_irqrestore(&kthread_lock, flags);
    
    return task;
}

bool kthread_should_stop(void) {
    thread_t *curr = sched_current_thread();
    unsigned long flags;
    bool should_stop = false;
    
    flags = spinlock_lock_irqsave(&kthread_lock);
    struct task_struct *task = kthread_list;
    while (task) {
        if (task->sched_thread == curr) {
            should_stop = task->should_stop;
            break;
        }
        task = task->next;
    }
    spinlock_unlock_irqrestore(&kthread_lock, flags);
    return should_stop;
}

int kthread_stop(struct task_struct *k) {
    if (!k) return -1;
    
    unsigned long flags;
    flags = spinlock_lock_irqsave(&kthread_lock);
    k->should_stop = true;
    spinlock_unlock_irqrestore(&kthread_lock, flags);
    
    while (!k->stopped) {
        sched_sleep(1);
    }
    
    flags = spinlock_lock_irqsave(&kthread_lock);
    struct task_struct **curr = &kthread_list;
    while (*curr) {
        if (*curr == k) {
            *curr = k->next;
            break;
        }
        curr = &(*curr)->next;
    }
    spinlock_unlock_irqrestore(&kthread_lock, flags);
    
    int res = k->result;
    kfree(k);
    return res;
}
