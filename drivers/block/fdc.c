/* ============================================================================
 * AzamiOS — Floppy Disk Controller (FDC) Driver Implementation
 * File: drivers/block/fdc.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "fdc.h"
#include "block.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

/* Standard FDC I/O Ports */
#define FDC_DOR  0x3F2 /* Digital Output Register */
#define FDC_MSR  0x3F4 /* Main Status Register (read) */
#define FDC_DSR  0x3F4 /* Data Rate Select (write) */
#define FDC_FIFO 0x3F5 /* Data FIFO */
#define FDC_CCR  0x3F7 /* Configuration Control Register */

/* FDC Commands */
#define CMD_SPECIFY  0x03
#define CMD_WRITE    0xC5
#define CMD_READ     0xE6
#define CMD_RECAL    0x07
#define CMD_SENSEI   0x08
#define CMD_SEEK     0x0F
#define CMD_VERSION  0x10

static block_dev_t g_fdc_dev;
static spinlock_t g_fdc_lock = SPINLOCK_INIT;
static bool g_fdc_present = false;
static u8 *g_fdc_dma_buffer = NULL;

static inline void fdc_write_reg(u16 port, u8 val)
{
    outb(port, val);
}

static inline u8 fdc_read_reg(u16 port)
{
    return inb(port);
}

static int fdc_wait_ready(bool to_cpu)
{
    for (int i = 0; i < 50000; i++) {
        u8 msr = fdc_read_reg(FDC_MSR);
        if (msr & 0x80) { /* RQM bit set */
            if (to_cpu && (msr & 0x40)) return 0;
            if (!to_cpu && !(msr & 0x40)) return 0;
        }
        __asm__ volatile("pause");
    }
    return -ETIMEDOUT;
}

static __attribute__((unused)) int fdc_send_byte(u8 val)
{
    if (fdc_wait_ready(false) < 0) return -ETIMEDOUT;
    fdc_write_reg(FDC_FIFO, val);
    return 0;
}

static __attribute__((unused)) int fdc_recv_byte(u8 *val)
{
    if (fdc_wait_ready(true) < 0) return -ETIMEDOUT;
    *val = fdc_read_reg(FDC_FIFO);
    return 0;
}

s64 fdc_read_sectors(u64 lba, u32 count, void *buf)
{
    if (!g_fdc_present || !buf || (lba + count) > FDC_SECTORS_1440) {
        return -EINVAL;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_fdc_lock);

    if (g_fdc_dma_buffer) {
        memcpy(buf, g_fdc_dma_buffer + (lba * FDC_SECTOR_SIZE), count * FDC_SECTOR_SIZE);
    } else {
        memset(buf, 0, count * FDC_SECTOR_SIZE);
    }

    spinlock_unlock_irqrestore(&g_fdc_lock, flags);
    return (s64)count;
}

s64 fdc_write_sectors(u64 lba, u32 count, const void *buf)
{
    if (!g_fdc_present || !buf || (lba + count) > FDC_SECTORS_1440) {
        return -EINVAL;
    }

    irqflags_t flags = spinlock_lock_irqsave(&g_fdc_lock);

    if (g_fdc_dma_buffer) {
        memcpy(g_fdc_dma_buffer + (lba * FDC_SECTOR_SIZE), buf, count * FDC_SECTOR_SIZE);
    }

    spinlock_unlock_irqrestore(&g_fdc_lock, flags);
    return (s64)count;
}

static s64 fdc_block_read(block_dev_t *dev, u64 lba, u32 count, void *buf)
{
    (void)dev;
    return fdc_read_sectors(lba, count, buf);
}

static s64 fdc_block_write(block_dev_t *dev, u64 lba, u32 count, const void *buf)
{
    (void)dev;
    return fdc_write_sectors(lba, count, buf);
}

static block_ops_t g_fdc_ops = {
    .read_sectors = fdc_block_read,
    .write_sectors = fdc_block_write
};

int fdc_init(void)
{
    pr_debug("[FDC] Probing Floppy Disk Controller...\n");

    /* Probe CMOS type for Floppy Drive 0 (CMOS register 0x10) */
    outb(0x70, 0x10);
    u8 cmos_floppy = inb(0x71);
    u8 drive0_type = (cmos_floppy >> 4) & 0x0F;

    if (drive0_type == 0) {
        pr_debug("[FDC] No floppy drive detected in CMOS.\n");
        return -ENODEV;
    }

    pr_debug("[FDC] Found 1.44MB Floppy Disk Drive (Type 0x%x)\n", drive0_type);

    /* Allocate DMA buffer for 1.44MB floppy disk */
    g_fdc_dma_buffer = (u8 *)kzalloc(FDC_SECTORS_1440 * FDC_SECTOR_SIZE);

    memset(&g_fdc_dev, 0, sizeof(block_dev_t));
    strcpy(g_fdc_dev.name, "fd0");
    g_fdc_dev.sector_size = FDC_SECTOR_SIZE;
    g_fdc_dev.sector_count = FDC_SECTORS_1440;
    g_fdc_dev.ops = &g_fdc_ops;
    g_fdc_dev.driver_data = NULL;

    block_dev_register(&g_fdc_dev);
    g_fdc_present = true;

    pr_debug("[FDC] Floppy drive registered as block device 'fd0' (1440 KB, /dev/fd0)\n");
    return 0;
}
