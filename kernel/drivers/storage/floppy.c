/**
 * kernel/drivers/storage/floppy.c — AzamiOS Floppy Disk Controller Driver
 *
 * EDUCATIONAL RELIABILITY EXPLANATIONS:
 * 1. IRQ Timeout Enforcement:
 *    Waiting endlessly for an IRQ (`while(!floppy_irq_fired);`) causes a system hang
 *    if the virtual disk or physical drive fails to signal completion. A bounding timeout
 *    counter (`timeout = 5000000`) guarantees kernel thread recovery.
 */

#include "./include/floppy.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../arch/include/isr.h"
#include "include/dma.h"

#include "../../filesystem/include/vfs.h"
#include "../../klibc/include/string.h"

static block_device_t floppy_dev;

// signal variable if controller has finished operation (secured by volatile!)
static volatile int floppy_irq_fired = 0;

/*
    IRQ 6 handler (38 in IDT)
    Controller FDC invoke it every time when it finished read,write,search
*/
void floppy_irq_handler(registers_t *r){
    UNUSED(r);
    floppy_irq_fired = 1;
}

/*
    Puts in sleep (blocks executing) until it receives interrupt from FDD
 */
 static void floppy_wait_irq(){
    int timeout = 5000000;
    while(!floppy_irq_fired && --timeout > 0);
    floppy_irq_fired = 0;
 }

 /*
 Secure sending byte to FDC
 According to specyficatio, we need to wait for bit 7 (0x80) in MSR will be ready
 */
static void floppy_write_cmd(uint8_t cmd){
    for(int i = 0; i < 500; i++){
        if(inb(FDC_MSR) & 0x80){
            outb(FDC_FIFO, cmd);
            return;
        }
    }
}

/* Secure reading byre from FDC (answers to commands) */
static uint8_t floppy_read_data(){
    for(int i = 0; i < 500; i++){
        if(inb(FDC_MSR) & 0x80){
            return inb(FDC_FIFO);
        }
    }
    return 0;
}

/*
    Use DMA driver to prepare channel 2
*/
static void floppy_dma_init(uint8_t *buffer, uint32_t length, int is_write){
    uint32_t addr = (uint32_t)buffer;

    // divide address in elements for DMA
    uint8_t page = (addr >> 16) & 0xFF;
    uint8_t offset_low = addr & 0xFF;
    uint8_t offset_high = (addr >> 8) & 0xFF;

    // DMA requires values (size - 1)
    uint16_t count = length - 1;
    uint8_t count_low = count & 0xFF;
    uint8_t count_high = (count >> 8) & 0xFF;

    dma_mask_channel(2);
    dma_reset_flipflop(0);

    dma_set_address(2, offset_low, offset_high);
    dma_set_ex_page_register(2, page);

    dma_reset_flipflop(0);
    dma_set_count(2, count_low, count_high);

    if(is_write){
        dma_set_write(2);
    }
    else{
        dma_set_read(2);
    }

    dma_unmask_channel(2); // ready to transfer;
}

void floppy_register_vfs(void);

void floppy_init(){
    kprintf("Initiation of FDC (Floppy) Driver...\n");
    
    // registration handler IRQ6
    register_interrupt_handler(38, floppy_irq_handler);
    // reset controller
    outb(FDC_DOR, 0x00);
    outb(FDC_DOR, 0x0C);

    floppy_wait_irq();

    for(int i = 0; i < 4; i++){
        floppy_write_cmd(FDC_CMD_SENSE_INT);
        floppy_read_data();
        floppy_read_data();
    }

    // set speed to 500 Kbps (standard 1.44MB)
    outb(FDC_CCR, 0x00);
    floppy_register_vfs();
}

/*
    read physical sektor from floppy
*/
void floppy_read_sector(uint8_t head, uint8_t track, uint8_t sector, uint8_t *buffer){
    floppy_dma_init(buffer, 512, 0);
    
    outb(FDC_DOR, 0x1C);
    
    floppy_write_cmd(FDC_CMD_READ_SECTOR);
    floppy_write_cmd((head << 2) | 0);
    floppy_write_cmd(track);
    floppy_write_cmd(head);
    floppy_write_cmd(sector);
    floppy_write_cmd(2);
    floppy_write_cmd(18);
    floppy_write_cmd(0x1B);
    floppy_write_cmd(0xFF);

    floppy_wait_irq();

    for(int i = 0; i < 7; i++){
        floppy_read_data();
    }
    outb(FDC_DOR, 0x0C);
}

static uint32_t floppy_block_read(block_device_t *dev, uint32_t lba, uint32_t count, void *buffer) {
    (void)dev;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t cur_lba = lba + i;
        uint8_t sector = (cur_lba % 18) + 1;
        uint8_t head = (cur_lba / 18) % 2;
        uint8_t track = cur_lba / 36;
        floppy_read_sector(head, track, sector, (uint8_t*)buffer + (i * 512));
    }
    return count * 512;
}

void floppy_register_vfs(void) {
    memset(&floppy_dev, 0, sizeof(floppy_dev));
    memcpy(floppy_dev.name, "fd0", 4);
    floppy_dev.block_size = 512;
    floppy_dev.read = floppy_block_read;
    floppy_dev.write = 0;
    vfs_register_device(&floppy_dev);
    kprintf("FDC (Floppy) Driver has been initiated succesfully (/dev/fd0 registered).\n");
}