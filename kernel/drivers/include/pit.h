#ifndef PIT_H
#define PIT_H

#include <stdint.h>

#define PIT_TIMER_FREQ_HZ 1000

/* Global atomic tick counters */
extern volatile uint64_t g_system_ticks;
extern uint32_t pit_ticks; /* Legacy 32-bit tick counter */

/*
    Set PIT frequency;
*/
void set_pit_phase(uint32_t hz);
/*
    Set pit count;
*/
void set_pit_count(uint32_t count);
/*
    Reads current pit count.
*/
uint32_t read_pit_count(void);
/*
    Initialize PIT
*/
void init_pit(void);
/*
    waits until ticks elapse.
*/
void pit_wait(int ticks);
/*
    Get 64-bit atomic uptime ticks.
*/
uint64_t timer_get_ticks(void);
/*
    Get 32-bit atomic uptime ticks.
*/
uint32_t timer_get_ticks_32(void);

#endif