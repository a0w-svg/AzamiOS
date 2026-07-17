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

// acquired lock type spinlock (Test-and-Test-and-Set to prevent cache line bouncing)
static inline void spinlock_acquire(volatile int *lock){
    while(1){
        while(*lock != 0){
            asm volatile("pause");
        }
        int locked = 1;
        asm volatile(
            "xchg %0, %1"
            : "+r" (locked), "+m" (*lock)
            :: "memory"
        );
        if(locked == 0){
            break;
        }
    }
}

// release lock type spinlock
static inline void spinlock_release(volatile int *lock){
    asm volatile("" ::: "memory");
    *lock = 0;
}

// acquired lock type spinlock with IRQ saving
static inline void spinlock_acquire_irqsave(volatile int *lock, unsigned long *flags){
    asm volatile("pushf; pop %0; cli" : "=r" (*flags) : : "memory");
    while(1){
        while(*lock != 0){
            asm volatile("pause");
        }
        int locked = 1;
        asm volatile(
            "xchg %0, %1"
            : "+r" (locked), "+m" (*lock)
            :: "memory"
        );
        if(locked == 0){
            break;
        }
    }
}

// release lock type spinlock with IRQ restoring
static inline void spinlock_release_irqrestore(volatile int *lock, unsigned long flags){
    asm volatile("" ::: "memory");
    *lock = 0;
    if(flags & (1 << 9)){ // IF (Interrupt Flag) is bit 9 in EFLAGS/RFLAGS
        asm volatile("sti" ::: "memory");
    }
}


#endif