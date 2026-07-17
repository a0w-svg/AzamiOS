/**
 * speaker.c  –  AzamiOS Ring-3 PC Speaker Audio Driver Implementation
 */
#include "speaker.h"
#include "port.h"


#define PIT_CHANNEL2 0x42
#define PIT_CMD_REG  0x43

void play_sound(uint32_t nfreq) {
    if (nfreq == 0) return;
    uint32_t div_freq = 1193180 / nfreq; 
    sys_outb(PIT_CMD_REG, 0xB6);
    sys_outb(PIT_CHANNEL2, (uint8_t)(div_freq));
    sys_outb(PIT_CHANNEL2, (uint8_t)(div_freq >> 8));
    
    uint8_t temp = sys_inb(0x61);
    if (temp != (temp | 3)) {
        sys_outb(0x61, temp | 3);
    }
}

void no_sound(void) {
    uint8_t temp = sys_inb(0x61) & 0xFC;
    sys_outb(0x61, temp);
}

static void delay_loop(int ms) {
    volatile int count = ms * 10000;
    while (count > 0) { count--; }
}

void beep(void) {
    play_sound(1000);
    delay_loop(100);
    no_sound();
}
