/**
 * include/speaker.h  –  AzamiOS Ring-3 PC Speaker Audio Driver Header
 */
#ifndef _USER_SPEAKER_H
#define _USER_SPEAKER_H

#include <stdint.h>

void play_sound(uint32_t nfreq);
void no_sound(void);
void beep(void);

#endif /* _USER_SPEAKER_H */
