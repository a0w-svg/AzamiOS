#include "./include/dma.h"
#include "../klibc/include/port.h"
/*
    Secondary ISA DMAC IO ports;
*/
enum DMA0_IO 
{
    DMA0_STATUS_REG = 0x08, // Status register (read)
    DMA0_CMD_REG = 0x08, // Command Register (write)
    DMA0_REQUEST_REG = 0x09, // Request Register (write)
    DMA0_CHAN_MASK_REG = 0x0A, // Single Mask Register (write)
    DMA0_MODE_REG = 0x0B,   // Mode Register (write)
    DMA0_CLEARBYTE_FLIPFLOP_REG = 0x0C, // Clear Byte Poiter Flip-Flop (write)
    DMA0_TEMP_REG = 0x0D, // Intermediate Register (Read)
    DMA0_PRIMARY_CLEAR_REG = 0x0D, // Primary Clear (write)
    DMA0_CLEAR_MASK_REG = 0xE, // Clear Mask Register (write)
    DMA0_MASK_REG = 0x0F // Write Mask Register (write)
};
/*
    Primary ISA DMAC IO Ports;
*/
enum DMA1_IO
{
    DMA1_STATUS_REG = 0xD0, // Status register (read)
    DMA1_CMD_REG = 0xD0, // Command Register (write)
    DMA1_REQUEST_REG = 0xD2, // Request Register (write)
    DMA1_CHAN_MASK_REG = 0xD4, // Single Mask Register (write)
    DMA1_MODE_REG = 0xD6,   // Mode Register (write)
    DMA1_CLEARBYTE_FLIPFLOP_REG = 0xD8, // Clear Byte Poiter Flip-Flop (write)
    DMA1_INTER_REG = 0xDA, // Intermediate Register (Read)
    DMA1_PRIMARY_CLEAR_REG = 0x0D, // Primary Clear (write)
    DMA1_UNMASK_ALL_REG = 0xDC, // Clear Mask Register (write)
    DMA1_MASK_REG = 0xDE // Write Mask Register (write)
};

/*
    Secondary ISA DMAC Channel Ports;
*/
enum DMA0_CHANNEL_IO
{
    DMA0_CHAN0_ADDR_REG = 0, // Channel 0 Address Registry;
    DMA0_CHAN0_COUNT_REG = 1, // Channel 0 Counter Registry;
    DMA0_CHAN1_ADDR_REG = 2, // Channel 1 Address Registry;
    DMA0_CHAN1_COUNT_REG = 3, // Channel 1 Counter Registry;
    DMA0_CHAN2_ADDR_REG = 4, // Channel 2 Address Registry;
    DMA0_CHAN2_COUNT_REG = 5, // Channel 2 Counter Registry;
    DMA0_CHAN3_ADDR_REG = 6, // Channel 3 Address Registry;
    DMA0_CHAN3_COUNT_REG = 7, // Channel 3 Counter Registry;
};

/*
    Primary ISA DMAC Channel Ports;
*/
enum DMA1_CHANNEL_IO
{
    DMA1_CHAN4_ADDR_REG = 0xC0, // Channel 4 Address Registry;
    DMA1_CHAN4_COUNT_REG = 0xC2, // Channel 4 Counter Registry;
    DMA1_CHAN5_ADDR_REG = 0xC4, // Channel 5 Address Registry;
    DMA1_CHAN5_COUNT_REG = 0xC6, // Channel 5 Counter Registry;
    DMA1_CHAN6_ADDR_REG = 0xC8, // Channel 6 Address Registry;
    DMA1_CHAN6_COUNT_REG = 0xCA, // Channel 6 Counter Registry;
    DMA1_CHAN7_ADDR_REG = 0xCC, // Channel 7 Address Registry;
    DMA1_CHAN7_COUNT_REG = 0xCE // Channel 7 Counter Registry;
};

/*
    ISA DMAC Extended Page Address Registers;
*/
enum DMA0_EX_PAGE_REG
{
    DMA_PAGE_EXTRA0 = 0x80, // Channel 0 (orginal PC) / Extra / Diagnostic port;
    DMA_PAGE_CHAN2_ADDRBYTE2 = 0x81, // Channel 1 (orginal PC) / Channel 2 (AT);
    DMA_PAGE_CHAN3_ADDRBYTE2 = 0x82, // Channel 2 (orginal PC) / Channel 3 (AT);
    DMA_PAGE_CHAN1_ADDRBYTE2 = 0x83, // Channel 3 (orginal PC) / Channel 1 (AT);
    DMA_PAGE_EXTRA1 = 0x84, // Extra;
    DMA_PAGE_EXTRA2 = 0x85, // Extra;
    DMA_PAGE_EXTRA3 = 0x86, // Extra;
    DMA_PAGE_CHAN6_ADDRBYTE2 = 0x87, // Channel 6(AT);
    DMA_PAGE_CHAN7_ADDRBYTE2 = 0x88, // Channel 7(AT);
    DMA_PAGE_CHAN5_ADDRBYTE2 = 0x89, // Channel 5(AT);
    DMA_PAGE_EXTRA4 = 0x8C, // Extra;
    DMA_PAGE_EXTRA5 = 0x8D, // Extra;
    DMA_PAGE_EXTRA6 = 0x8E, // Extra
    DMA_PAGE_DRAM_REFRESH = 0x8F // no longer used in new PCs;
};

/*
    DMA mode bit mask 
*/

enum DMA_MODE_REG_MASK
{
    DMA_MODE_MASK_SEL = 3, // bits 0-1: SEL0, SEL1 - Channel Select;
    DMA_MODE_MASK_TRA = 0xC, // bits 2-3: TRA0, TRA1 - Transport type;
    DMA_MODE_SELF_TEST = 0, // 00 // Controller self test;
    DMA_MODE_READ_TRANSFER = 4, // Read Transfer;
    DMA_MODE_WRITE_TRANSFER = 8, // Write Transfer;
    // bit 4: AUTO - Automatic reinitialize after transfer completes (Device must support);
    DMA_MODE_MASK_AUTO = 0x10,
    DMA_MODE_MASK_IDEC = 0x20, // bit 5: IDEC
    DMA_MODE_MASK = 0xC0, // bits 6-7 MOD0, MOD1 - Mode
    DMA_MODE_TRANSFER_ON_DEMAND = 0, // Transfer on Demand;
    DMA_MODE_TRANSFER_SINGLE = 0x40, // Single DMA Transfer;
    DMA_MODE_TRANSFER_BLOCK = 0x80, // Block DMA Transfer;
    DMA_MODE_TRANSFER_CASCADE = 0xC0 // Cascade Mode;
};

/*
    DMA command reg bit mask
*/
enum DMA_CMD_REG_MASK
{
    DMA_CMD_MASK_MEMTOMEM = 1, // bit 0: MMT - Memory to Memory Transfer;
    DMA_CMD_CHAN0ADDRHOLD = 2, // bit 1: ADHE - Channel 0 Address Hold;
    DMA_CMD_CONTROLER_ENABLE = 4, // bit 2: COND - Controler Enable;
    DMA_CMD_TIMING = 8, // bit 3: COMP - Timing;
    DMA_CMD_PRORITY = 0x10, // bit 4: PRIO - Prority;
    DMA_CMD_WRITESEL = 0x20, // bit 5: EXTW - Write selection;
    DMA_CMD_DREQ = 0x40, // bit 6: DROP - DMA Request (DREQ);
    DMA_CMD_DACK = 0x80 // bit 7: DACKP - DMA Acknowledge (DACK);
};

/*
    Set the mode of a channel;
*/
void dma_set_mode(uint8_t channel, uint8_t mode)
{
    int dma = (channel < 4) ? 0 : 1;
    int dma_channel = (dma == 0) ? channel : channel - 4;
    dma_mask_channel(channel);
    uint16_t port_chan = (channel < 4) ? (DMA0_MODE_REG) : DMA1_MODE_REG;
    outb(port_chan, dma_channel | (mode));
    dma_unmask_all(dma);
}

/*
    Prepares channel for read;
*/
void dma_set_read(uint8_t channel)
{
    dma_set_mode(channel, DMA_MODE_READ_TRANSFER | DMA_MODE_TRANSFER_SINGLE | DMA_MODE_MASK_AUTO);
}

/*
    prepares channel for write;
*/
void dma_set_write(uint8_t channel)
{
    dma_set_mode(channel, DMA_MODE_WRITE_TRANSFER | DMA_MODE_TRANSFER_SINGLE | DMA_MODE_MASK_AUTO);
}

/*
    Resets flipflop
*/
void dma_reset_flipflop(int dma)
{
    if(dma < 2) return;
    uint16_t port = (dma == 0) ? DMA0_CLEARBYTE_FLIPFLOP_REG : DMA1_CLEARBYTE_FLIPFLOP_REG;
    // it doesn't matter what is written to this register;
    outb(port, 0xFF);
}

/*
    Resets the controller to defaults;
*/
void dma_reset(int dma)
{
    // it doesn't matter what is written to this register;
    outb(DMA0_TEMP_REG, 0xFF);
}

/*
    Sets the address of a channel;
*/
void dma_set_address(uint8_t channel, uint8_t low, uint8_t high)
{
    if(channel > 8) return;
    uint16_t port = 0;
    switch(channel)
    {
        case 0:
            port = DMA0_CHAN0_ADDR_REG;
            break;
        case 1:
            port = DMA0_CHAN1_ADDR_REG;
            break;
        case 2:
            port = DMA0_CHAN2_ADDR_REG;
            break;
        case 3:
            port = DMA0_CHAN3_ADDR_REG;
            break;
        case 4:
            port = DMA1_CHAN4_ADDR_REG;
            break;
        case 5:
            port = DMA1_CHAN5_ADDR_REG;
            break;
        case 6:
            port = DMA1_CHAN6_ADDR_REG;
            break;
        case 7:
            port = DMA1_CHAN7_ADDR_REG;
            break;
    }
    outb(port, low);
    outb(port, high);
}

/*
    Sets the counter of a channel
*/
void dma_set_count(uint8_t channel, uint8_t low, uint8_t high)
{
    if(channel > 8) return;
    uint16_t port = 0;
    switch(channel)
    {
        case 0:
            port = DMA0_CHAN0_COUNT_REG;
            break;
        case 1:
            port = DMA0_CHAN1_COUNT_REG;
            break;
        case 2:
            port = DMA0_CHAN2_COUNT_REG;
            break;
        case 3:
            port = DMA0_CHAN3_COUNT_REG;
            break;
        case 4:
            port = DMA1_CHAN4_COUNT_REG;
            break;
        case 5:
            port = DMA1_CHAN5_COUNT_REG;
            break;
        case 6:
            port = DMA1_CHAN6_COUNT_REG;
            break;
        case 7:
            port = DMA1_CHAN7_COUNT_REG;
            break;
    }
    outb(port, low);
    outb(port, high);
}

/*
    Set the mask of a channel;
*/
void dma_mask_channel(uint8_t channel)
{
    if(channel <= 4)
        outb(DMA0_CHAN_MASK_REG, (1 << (channel - 1)));
    else
    {
        outb(DMA1_CHAN_MASK_REG, (1 << (channel - 5)));
    }
}

/*
    Unset the mask of a channel;
*/
void dma_unmask_channel(uint8_t channel)
{
    if(channel <= 4)
        outb(DMA0_CHAN_MASK_REG, channel);
    else
    {
        outb(DMA1_CHAN_MASK_REG, channel);
    }
}

/*
    Unmask all dma channels;
*/
void dma_unmask_all(int dma)
{
    // it doesn't matter what is written to this register;
    outb(DMA1_UNMASK_ALL_REG, 0xFF);
}

/*
    Set the DMA Extended Page register;
*/
void dma_set_ex_page_register(uint8_t reg, uint8_t value)
{
    if(reg > 14) return;
    uint16_t port = 0;
    switch(reg)
    {
        case 1:
            port = DMA_PAGE_CHAN1_ADDRBYTE2;
            break;
        case 2:
            port = DMA_PAGE_CHAN2_ADDRBYTE2;
            break;
        case 3:
            port = DMA_PAGE_CHAN3_ADDRBYTE2;
            break;
        case 4:
            return; // nothing should ever write to register 4;
            break;
        case 5:
            port = DMA_PAGE_CHAN5_ADDRBYTE2;
            break;
        case 6:
            port = DMA_PAGE_CHAN6_ADDRBYTE2;
            break;
        case 7:
            port = DMA_PAGE_CHAN7_ADDRBYTE2;
            break;
    }
    outb(port, value);
}