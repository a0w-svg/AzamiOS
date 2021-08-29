#ifndef PC_SPEAKER_H
#define PC_SPEAKER_H
#include <stdint.h>
/*
    Play the sound using the PC speaker;
*/
void play_sound(uint32_t nfreq);
/*
    Stop sound;
*/
void no_sound();
/*
    Make a beep;
*/
void beep();
#endif