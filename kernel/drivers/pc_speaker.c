#include "./include/pc_speaker.h"
#include "../klibc/include/port.h"
#include "./include/pit.h"
#define PIT_CHANNEL2 0x42
#define PIT_CMD_REG  0x43

/*
    Play the sound using built in speaker;
*/
void play_sound(uint32_t nfreq)
{
    uint32_t div_freq;
    uint8_t temp;
    // set the PIT to the specified frequency;
    div_freq = 1193180 / nfreq; 
    outb(PIT_CMD_REG, 0xB6);
    outb(PIT_CHANNEL2, (uint8_t)(div_freq));
    outb(PIT_CHANNEL2, (uint8_t)(div_freq >> 8));
    // play the sound
    temp = inb(0x61);
    if(temp != (temp | 3))
        outb(0x61, temp | 3);
}

/*
    Stop sound;
*/
void no_sound()
{
    uint8_t temp = inb(0x61) & 0xFC;
    outb(0x61, temp);
}

/*
    Make a beep;
*/
void beep()
{
    play_sound(1000);
    pit_wait(10);
    no_sound();
}