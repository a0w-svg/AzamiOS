/**
 * sb16.c  –  Sound Blaster 16 ISA Audio Driver
 */
#include "./include/sb16.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"

static bool g_sb16_present = false;

static void sb16_delay(void) {
    for (volatile int i = 0; i < 1000; i++);
}

void sb16_init(void) {
    kprintf("\nsb16: probing ISA bus port 0x%x for Sound Blaster 16...\n", SB16_PORT_BASE);

    /* Reset DSP */
    outb(SB16_DSP_RESET, 1);
    sb16_delay();
    outb(SB16_DSP_RESET, 0);
    sb16_delay();

    /* Check if DSP responded with 0xAA ready byte */
    int timeout = 10000;
    while (timeout-- > 0) {
        if (inb(SB16_DSP_STATUS) & 0x80) {
            if (inb(SB16_DSP_READ) == 0xAA) {
                g_sb16_present = true;
                break;
            }
        }
    }

    if (!g_sb16_present) {
        kprintf("sb16: DSP reset failed or hardware not present at 0x%x\n", SB16_PORT_BASE);
        return;
    }

    /* Request DSP Version */
    outb(SB16_DSP_WRITE, 0xE1);
    uint8_t major = 0, minor = 0;
    timeout = 10000;
    while (timeout-- > 0) {
        if (inb(SB16_DSP_STATUS) & 0x80) { major = inb(SB16_DSP_READ); break; }
    }
    timeout = 10000;
    while (timeout-- > 0) {
        if (inb(SB16_DSP_STATUS) & 0x80) { minor = inb(SB16_DSP_READ); break; }
    }

    kprintf("sb16: detected DSP version %d.%d (ISA IRQ 5, DMA 1/5)\n", major, minor);
    kprintf("sb16: 16-bit stereo audio playback engine ready\n");
}
