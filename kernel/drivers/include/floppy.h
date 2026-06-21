#ifndef FLOPPY_H
#define FLOPPY_H
#include <stdint.h>

// Floppy Disk Drive Registers
#define FDC_DOR  0x3F2 // Digital Output Register (Drive and choose disk)
#define FDC_MSR  0x3F4 // Main Status Register (Ready to receive command)
#define FDC_FIFO 0x3F5 // Data FIFO (Sending and Receiving Data/Commands)
#define FDC_CCR  0x3F7 // Configuration Control Register (Transfer Speed)

// Controller Commands (with set bits MT, MFM, SK for 1.44MB floppies)
#define FDC_CMD_READ_SECTOR 0xE6
#define FDC_CMD_SENSE_INT   0x08
#define FDC_CMD_SPECIFY     0x03
#define FDC_CMD_RECALIBRATE 0x07

void init_floppy();
void floppy_read_sector(uint8_t head, uint8_t track, uint8_t sector, uint8_t *buffer);

#endif