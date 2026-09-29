#include "completion.h"
#include <kernel/sched/sched.h>
#include <kernel/time/timekeeping.h>

void complete(struct completion *c) {
    unsigned long flags;
    flags = spinlock_lock_irqsave(&c->lock);
    c->done++;
    spinlock_unlock_irqrestore(&c->lock, flags);
}

void complete_all(struct completion *c) {
    unsigned long flags;
    flags = spinlock_lock_irqsave(&c->lock);
    c->done = (unsigned int)-1;
    spinlock_unlock_irqrestore(&c->lock, flags);
}

void wait_for_completion(struct completion *c) {
    while (1) {
        unsigned long flags;
        flags = spinlock_lock_irqsave(&c->lock);
        if (c->done) {
            if (c->done != (unsigned int)-1)
                c->done--;
            spinlock_unlock_irqrestore(&c->lock, flags);
            return;
        }
        spinlock_unlock_irqrestore(&c->lock, flags);
        sched_sleep(1);
    }
}

unsigned long wait_for_completion_timeout(struct completion *c, unsigned long timeout_ms) {
    u64 start_ns = ktime_get_ns();
    u64 timeout_ns = timeout_ms * 1000000ULL;
    
    while (1) {
        unsigned long flags;
        flags = spinlock_lock_irqsave(&c->lock);
        if (c->done) {
            if (c->done != (unsigned int)-1)
                c->done--;
            spinlock_unlock_irqrestore(&c->lock, flags);
            
            u64 elapsed = ktime_get_ns() - start_ns;
            if (elapsed >= timeout_ns) return 1;
            return (unsigned long)((timeout_ns - elapsed) / 1000000ULL);
        }
        spinlock_unlock_irqrestore(&c->lock, flags);
        
        if (ktime_get_ns() - start_ns >= timeout_ns)
            return 0; /* timeout */
            
        sched_sleep(1);
    }
}

bool completion_done(struct completion *c) {
    unsigned long flags;
    bool done;
    flags = spinlock_lock_irqsave(&c->lock);
    done = (c->done > 0);
    spinlock_unlock_irqrestore(&c->lock, flags);
    return done;
}

void reinit_completion(struct completion *c) {
    c->done = 0;
}
