#pragma once
#include <arch/x86_64/cpu/spinlock.h>
#include <azami/types.h>

struct completion {
    unsigned int done;
    spinlock_t lock;
};

#define DECLARE_COMPLETION(x) struct completion x = { .done = 0, .lock = SPINLOCK_INIT }
#define init_completion(x) do { (x)->done = 0; (x)->lock = (spinlock_t)SPINLOCK_INIT; } while(0)

void complete(struct completion *c);
void complete_all(struct completion *c);
void wait_for_completion(struct completion *c);
unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout_ms);
bool completion_done(struct completion *c);
void reinit_completion(struct completion *c);
