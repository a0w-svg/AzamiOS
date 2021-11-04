#ifndef LPT_H
#define LPT_H
#include <stdint.h>

/*
    ports
    LPT1 - 0x378 or occasionally 0x3BC (IRQ7)
    LPT2 - 0x278 (IRQ 6)
    LPT3 - 0x3BC (IRQ 5)
*/
/*
    Data Register address = Base_Addr + 0
    Status Register address = Base_Addr + 1

    Control Register Address = Base_Adrr + 2
    Bit 0 - STROBE
    Bit 1 - AUTO_LF
    Bit 2 - INITIALISE
    Bit 3 - SELECT
    Bit 4 - IRQACK
    Bit 5 - BIDI
    Bit 6 - unused
    Bit 7 - unused
*/

/*
    Sends a byte to the printer
*/
void lpt_write_byte(uint8_t lpt_data);

#endif