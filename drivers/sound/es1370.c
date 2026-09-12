/* ============================================================================
 * AzamiOS — Ensoniq AudioPCI ES1370 / ES1371 Audio Driver
 * File: drivers/sound/es1370.c
 *
 * Implements audio playback for the Ensoniq AudioPCI ES1370 / ES1371 sound card
 * (supported in QEMU via -device ES1370). Exposes /dev/dsp2 and /dev/audio2.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "es1370.h"
#include "../base/pci_bus.h"
#include "../../fs/vfs.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"

extern int devfs_register_device(const char *name, file_operations_t *fops, void *private_data);

#define ES_CONTROL     0x00
#define ES_STATUS      0x04
#define ES_MEM_PAGE    0x0C
#define ES_CODEC       0x10
#define ES_SERIAL      0x20
#define ES_DAC2_SAMP   0x28

#define ES_DAC2_EN     (1U << 5)
#define ES_BREQ        (1U << 4)
#define ES_CDC_EN      (1U << 1)

#define ES_MEM_DAC2_ADDR 0x38
#define ES_MEM_DAC2_SIZE 0x3C

#define ES_BUF_SIZE    4096

static u16        g_es_io_base = 0;
static u8         g_es_irq = 0;
static bool       g_es_ready = false;
static spinlock_t g_es_lock = SPINLOCK_INIT;

static u8        *g_es_dma_buffer = NULL;
static phys_addr_t g_es_dma_phys = 0;

static void es1370_codec_write(u16 base, u8 reg, u8 val)
{
    /* Wait for CODEC ready */
    for (int i = 0; i < 10000; i++) {
        if (!(inl(base + ES_STATUS) & (1 << 8))) break;
        cpu_pause();
    }
    outw(base + ES_CODEC, ((u16)reg << 8) | val);
}

static void es1370_codec_init(u16 base)
{
    /* AK4531 Codec registers: un-mute Master, Line, and Voice DAC */
    es1370_codec_write(base, 0x00, 0x00); /* Master L */
    es1370_codec_write(base, 0x01, 0x00); /* Master R */
    es1370_codec_write(base, 0x02, 0x00); /* Voice DAC L */
    es1370_codec_write(base, 0x03, 0x00); /* Voice DAC R */
    es1370_codec_write(base, 0x04, 0x00); /* FM DAC L */
    es1370_codec_write(base, 0x05, 0x00); /* FM DAC R */
}

s64 es1370_write_pcm(const u8 *data, u64 len)
{
    if (!g_es_ready || !data || len == 0) return 0;

    irqflags_t flags = spinlock_lock_irqsave(&g_es_lock);

    size_t chunk = (len > ES_BUF_SIZE) ? ES_BUF_SIZE : (size_t)len;
    memcpy(g_es_dma_buffer, data, chunk);

    /* Program sample count (16-bit stereo = 4 bytes per frame) */
    u32 sample_count = (u32)(chunk / 4);
    if (sample_count > 0) sample_count--;
    outl(g_es_io_base + ES_DAC2_SAMP, sample_count);

    /* Start DAC2 */
    u32 ctrl = inl(g_es_io_base + ES_CONTROL);
    outl(g_es_io_base + ES_CONTROL, ctrl | ES_DAC2_EN);

    spinlock_unlock_irqrestore(&g_es_lock, flags);
    return (s64)chunk;
}

static s64 dev_es1370_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return es1370_write_pcm((const u8 *)buf, len);
}

static file_operations_t g_es1370_fops = {
    .read    = NULL,
    .write   = dev_es1370_write,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
};

static int es1370_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *info = to_pci_info(dm);
    if (!info) return -ENODEV;

    u32 raw_bar = info->bar[0];
    if (!(raw_bar & 1)) return -ENODEV;

    u16 io_base = (u16)(raw_bar & ~0x3);
    if (!io_base) return -ENODEV;

    g_es_io_base = io_base;
    g_es_irq = info->interrupt_line;

    pci_enable_bus_mastering(dm->hal);

    /* Allocate DMA buffer */
    g_es_dma_phys = pmm_alloc_page();
    if (!g_es_dma_phys) return -ENOMEM;
    g_es_dma_buffer = (u8 *)PHYS_TO_VIRT(g_es_dma_phys);
    memset(g_es_dma_buffer, 0, ES_BUF_SIZE);

    /* Set up DAC2 physical buffer address and size in ES memory window */
    outb(io_base + ES_MEM_PAGE, 0x0C);
    outl(io_base + ES_MEM_DAC2_ADDR, (u32)g_es_dma_phys);
    outl(io_base + ES_MEM_DAC2_SIZE, (u32)(ES_BUF_SIZE / 4 - 1));

    /* Initialize AK4531 Codec */
    es1370_codec_init(io_base);

    /* Enable CDC and BREQ in Control register */
    outl(io_base + ES_CONTROL, ES_CDC_EN | ES_BREQ);

    g_es_ready = true;

    devfs_register_device("dsp2", &g_es1370_fops, NULL);
    devfs_register_device("audio2", &g_es1370_fops, NULL);
    dm_set_drvdata(dm, &g_es_ready);

    pr_debug("[ES1370] Ensoniq AudioPCI ready at I/O 0x%04X, IRQ %u (/dev/dsp2, /dev/audio2)\n",
             g_es_io_base, g_es_irq);

    return 0;
}

static const pci_device_id_t es1370_pci_ids[] = {
    { PCI_DEVICE(0x1274, 0x5000) },   /* Ensoniq ES1370 AudioPCI */
    { PCI_DEVICE(0x1274, 0x1371) },   /* Ensoniq ES1371 / CT5880 */
    { 0 }
};

static pci_driver_t es1370_pci_driver = {
    .drv      = { .name = "es1370" },
    .id_table = es1370_pci_ids,
    .probe    = es1370_probe,
    .remove   = NULL,
};

void es1370_init(void)
{
    pci_driver_register(&es1370_pci_driver);
}
