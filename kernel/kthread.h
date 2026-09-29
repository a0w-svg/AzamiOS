#pragma once
#include <azami/types.h>
#include <azami/defs.h>

typedef int (*kthread_fn_t)(void *data);

struct task_struct {
    void *sched_thread;    /* AzamiOS thread pointer */
    kthread_fn_t func;
    void *data;
    const char *name;
    bool should_stop;
    bool stopped;
    int result;
    struct task_struct *next;
};

struct task_struct *kthread_create(kthread_fn_t fn, void *data, const char *name);
struct task_struct *kthread_run(kthread_fn_t fn, void *data, const char *name);
bool kthread_should_stop(void);
int kthread_stop(struct task_struct *k);
