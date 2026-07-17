#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "process.h"

void scheduler_init(void);
void scheduler_add(process_t *proc);
void scheduler_schedule(void);
process_t *scheduler_get_current(void);
struct process *scheduler_create_idle_task(uint32_t core_id);

#endif

