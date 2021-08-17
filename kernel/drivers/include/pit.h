#ifndef PIT_H
#define PIT_H
#include <stdint.h>
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
uint32_t read_pit_count();
/*
    Initialize PIT
*/
void init_pit();
/*
    waits until ticks be zero.
*/
void pit_wait(int ticks);
#endif