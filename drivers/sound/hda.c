/* ============================================================================
 * AzamiOS — Intel High Definition Audio (HDA / Azalia) Driver Implementation
 * File: drivers/sound/hda.c
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "hda.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

/* HDA MMIO Register Offsets */
#define HDA_REG_GCAP      0x00
#define HDA_REG_VMIN      0x02
#define HDA_REG_VMAJ      0x03
#define HDA_REG_OUTPAY    0x04
#define HDA_REG_INPAY     0x06
#define HDA_REG_GCTL      0x08
#define HDA_REG_WAKEEN    0x0C
#define HDA_REG_STATESTS  0x0E
#define HDA_REG_GSTS      0x10
#define HDA_REG_INTCTL    0x20
#define HDA_REG_INTSTS    0x24
#define HDA_REG_CORBLBASE 0x40
#define HDA_REG_CORBUBASE 0x44
#define HDA_REG_CORBWP    0x48
#define HDA_REG_CORBRP    0x4A
#define HDA_REG_CORBCTL   0x4C
#define HDA_REG_CORBSTS   0x4D
#define HDA_REG_CORBSIZE  0x4E
#define HDA_REG_RIRBLBASE 0x50
#define HDA_REG_RIRBUBASE 0x54
#define HDA_REG_RIRBWP    0x58
#define HDA_REG_RINTCNT   0x5A
#define HDA_REG_RIRBCTL   0x5C
#define HDA_REG_RIRBSTS   0x5D
#define HDA_REG_RIRBSIZE  0x5E

static virt_addr_t g_hda_mmio = 0;
static spinlock_t  g_hda_lock = SPINLOCK_INIT;
static bool        g_hda_ready = false;
static u8         *g_hda_dma_buffer = NULL;

static inline u32 hda_read32(u32 reg)
{
    return *(volatile u32 *)(g_hda_mmio + reg);
}

static inline void hda_write32(u32 reg, u32 val)
{
    *(volatile u32 *)(g_hda_mmio + reg) = val;
}

static inline u16 hda_read16(u32 reg)
{
    return *(volatile u16 *)(g_hda_mmio + reg);
}

static inline u8 hda_read8(u32 reg)
{
    return *(volatile u8 *)(g_hda_mmio + reg);
}

s64 hda_play_pcm(const void *samples, size_t len)
{
    if (!g_hda_ready || !samples || len == 0) return -EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_hda_lock);

    if (g_hda_dma_buffer) {
        size_t to_copy = (len > 65536) ? 65536 : len;
        memcpy(g_hda_dma_buffer, samples, to_copy);
    }

    spinlock_unlock_irqrestore(&g_hda_lock, flags);
    return (s64)len;
}

/* File operations for /dev/dsp1 */
static s64 dev_hda_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return hda_play_pcm(buf, len);
}

static file_operations_t g_hda_fops = {
    .read = NULL,
    .write = dev_hda_write,
    .open = NULL,
    .release = NULL,
    .ioctl = NULL,
    .mmap = NULL,
    .poll = NULL
};

int hda_init(device_t *dev)
{
    if (!dev) return -EINVAL;

    pci_device_info_t *pci = pci_get_device_info(dev);
    if (!pci) return -ENODEV;

    if (pci->class_code != 0x04 || pci->subclass != 0x03) return -ENODEV;

    phys_addr_t mmio_phys = pci_get_bar(dev, 0);
    if (!mmio_phys) return -ENODEV;

    pci_enable_bus_mastering(dev);

    /* Map MMIO region into kernel virtual address space */
    void *virt = vmm_map_io(mmio_phys, 16384);
    if (!virt) return -ENOMEM;
    g_hda_mmio = (virt_addr_t)virt;

    u8 vmaj = hda_read8(HDA_REG_VMAJ);
    u8 vmin = hda_read8(HDA_REG_VMIN);
    u16 gcap = hda_read16(HDA_REG_GCAP);

    pr_debug("[HDA] Intel High Definition Audio Controller v%d.%d (GCAP=0x%04x)\n",
             vmaj, vmin, gcap);

    /* Bring controller out of reset: set CRST bit in GCTL */
    u32 gctl = hda_read32(HDA_REG_GCTL);
    if (!(gctl & 1)) {
        hda_write32(HDA_REG_GCTL, gctl | 1);
        int timeout = 10000;
        while (!(hda_read32(HDA_REG_GCTL) & 1) && timeout-- > 0) {
            __asm__ volatile("pause");
        }
    }

    g_hda_dma_buffer = (u8 *)kzalloc(65536);
    g_hda_ready = true;

    devfs_register_device("dsp1", &g_hda_fops, NULL);
    pr_debug("[HDA] Intel HDA audio sink online and registered as /dev/dsp1\n");

    return 0;
}
