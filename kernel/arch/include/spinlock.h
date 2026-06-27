#ifndef SPINLOCK_H
#define SPINLOCK_H

#include <stdint.h>

// atomic value incrementation (e.g core counter)
static inline void atomic_inc(volatile uint32_t *val){
    asm volatile(
        "lock incl %0"
        : "+m" (*val)
        :: "memory"
    );
}

// acquired lock type spinlock
static inline void spinlock_acquire(volatile int *lock){
    int locked = 1;
    while(1){
        asm volatile(
            "xchg %0, %1"
            : "+r" (locked), "+m" (*lock)
            :: "memory"
        );
        if(locked == 0){
            break;
        }
        asm volatile("pause");
    }
}

// release lock type spinlock
static inline void spinlock_release(volatile int *lock){
    asm volatile("" ::: "memory");
    *lock = 0;
}


#endif