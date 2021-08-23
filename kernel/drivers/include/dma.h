#ifndef DMA_H
#define DMA_H
#include <stdint.h>

/*
    Set the DMA Extended Page register;
*/
void dma_set_ex_page_register(uint8_t reg, uint8_t value);
/*
    Set the mode of a channel;
*/
void dma_set_mode(uint8_t channel, uint8_t mode);
/*
    Prepares channel for read;
*/
void dma_set_read(uint8_t channel);
/*
    prepares channel for write;
*/
void dma_set_write(uint8_t channel);
/*
    Reset flipflop
*/
void dma_reset_flipflop(int dma);
/*
    Resets the controller to defaults;
*/
void dma_reset(int dma);
/*
    Sets the address of a channel;
*/
void dma_set_address(uint8_t channel, uint8_t low, uint8_t high);
/*
    Sets the counter of a channel
*/
void dma_set_count(uint8_t channel, uint8_t low, uint8_t high);
/*
    Set the mask of a channel;
*/
void dma_mask_channel(uint8_t channel);
/*
    Unset the mask of a channel;
*/
void dma_unmask_channel(uint8_t channel);
/*
    Unmask all dma channels;
*/
void dma_unmask_all(int dma);
#endif