/* ============================================================================
 * AzamiOS — Intel High Definition Audio (HDA / Azalia) Driver Implementation
 * File: drivers/sound/hda.c
 *
 * Implements the Intel High Definition Audio Specification (Rev 1.0a) with:
 *   - Hardware controller reset and capability discovery (GCAP, GCTL)
 *   - Codec discovery via STATESTS
 *   - CORB (Command Output Ring Buffer) & RIRB (Response Input Ring Buffer)
 *   - Codec initialization (Power State D0, Pin Complex Output Enable,
 *     Headphone/Line-Out Amplifier EAPD, Unmute DAC converters)
 *   - Cyclic DMA Output Stream engine with 4KB page-aligned Buffer Descriptor
 *     List (BDL) running 48 kHz 16-bit stereo PCM.
 *
 * Exposes /dev/dsp1 to userspace for OSS/Linux audio playback.
 * ============================================================================ */

#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "hda.h"
#include "../../hal/pci.h"
#include "../../hal/device.h"
#include "../base/pci_bus.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/lib/string.h"
#include "../../fs/vfs.h"

/* HDA MMIO Register Offsets */
#define HDA_REG_GCAP         0x00
#define HDA_REG_VMIN         0x02
#define HDA_REG_VMAJ         0x03
#define HDA_REG_OUTPAY       0x04
#define HDA_REG_INPAY        0x06
#define HDA_REG_GCTL         0x08
#define HDA_REG_WAKEEN       0x0C
#define HDA_REG_STATESTS     0x0E
#define HDA_REG_GSTS         0x10
#define HDA_REG_INTCTL       0x20
#define HDA_REG_INTSTS       0x24
#define HDA_REG_CORBLBASE    0x40
#define HDA_REG_CORBUBASE    0x44
#define HDA_REG_CORBWP       0x48
#define HDA_REG_CORBRP       0x4A
#define HDA_REG_CORBCTL      0x4C
#define HDA_REG_CORBSTS      0x4D
#define HDA_REG_CORBSIZE     0x4E
#define HDA_REG_RIRBLBASE    0x50
#define HDA_REG_RIRBUBASE    0x54
#define HDA_REG_RIRBWP       0x58
#define HDA_REG_RINTCNT      0x5A
#define HDA_REG_RIRBCTL      0x5C
#define HDA_REG_RIRBSTS      0x5D
#define HDA_REG_RIRBSIZE     0x5E

/* Stream Descriptor Offsets (relative to stream base) */
#define HDA_SD_CTL           0x00
#define HDA_SD_STS           0x03
#define HDA_SD_LPIB          0x04
#define HDA_SD_CBL           0x08
#define HDA_SD_LVI           0x0C
#define HDA_SD_FIFOS         0x10
#define HDA_SD_FMT           0x12
#define HDA_SD_BDPL          0x18
#define HDA_SD_BDPU          0x1C

/* Standard HDA Verbs */
#define HDA_VERB_GET_PARAM          0xF00
#define HDA_VERB_SET_CONN_SELECT    0x701
#define HDA_VERB_SET_POWER_STATE    0x705
#define HDA_VERB_SET_STREAM_CHANNEL 0x706
#define HDA_VERB_SET_PIN_WIDGET_CTL 0x707
#define HDA_VERB_SET_AMP_GAIN_MUTE  0x300
#define HDA_VERB_SET_CONV_FMT       0x200
#define HDA_VERB_SET_EAPD_BTLENABLE 0x70C

/* Buffer Descriptor List Entry */
typedef struct __attribute__((packed)) {
    u64 addr;
    u32 len;
    u32 ioc;
} hda_bdl_entry_t;

static virt_addr_t g_hda_mmio = 0;
static spinlock_t  g_hda_lock = SPINLOCK_INIT;
static bool        g_hda_ready = false;

/* CORB / RIRB Ring Buffers */
static u32        *g_corb = NULL;
static phys_addr_t g_corb_phys = 0;
static u64        *g_rirb = NULL;
static phys_addr_t g_rirb_phys = 0;
static u16         g_corb_wp = 0;
static u16         g_rirb_rp = 0;

/* DMA Output Stream Buffers */
#define HDA_NUM_BDL_ENTRIES 2
#define HDA_BDL_BUF_SIZE    32768
#define HDA_TOTAL_BUF_SIZE  (HDA_NUM_BDL_ENTRIES * HDA_BDL_BUF_SIZE)

static hda_bdl_entry_t *g_bdl = NULL;
static phys_addr_t      g_bdl_phys = 0;
static u8              *g_dma_buffer = NULL;
static phys_addr_t      g_dma_buffer_phys = 0;
static u32              g_out_stream_offset = 0;
static u32              g_dma_write_pos = 0;

/* ── MMIO Helpers ────────────────────────────────────────────────────────── */

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

static inline void hda_write16(u32 reg, u16 val)
{
    *(volatile u16 *)(g_hda_mmio + reg) = val;
}

static inline u8 hda_read8(u32 reg)
{
    return *(volatile u8 *)(g_hda_mmio + reg);
}

static inline void hda_write8(u32 reg, u8 val)
{
    *(volatile u8 *)(g_hda_mmio + reg) = val;
}

/* ── CORB / RIRB Command Engine ───────────────────────────────────────────── */

static int hda_send_verb(u8 codec_addr, u8 nid, u32 verb, u8 param, u64 *resp_out)
{
    if (!g_corb || !g_rirb) return -ENODEV;

    u32 cmd = ((u32)codec_addr << 28) | ((u32)nid << 20) | (verb << 8) | param;

    /* Write verb to CORB ring */
    g_corb_wp = (g_corb_wp + 1) % 256;
    g_corb[g_corb_wp] = cmd;
    hda_write16(HDA_REG_CORBWP, g_corb_wp);

    /* Wait for response in RIRB */
    int timeout = 50000;
    while (timeout-- > 0) {
        u16 rirb_wp = hda_read16(HDA_REG_RIRBWP) & 0xFF;
        if (rirb_wp != g_rirb_rp) {
            g_rirb_rp = (g_rirb_rp + 1) % 256;
            if (resp_out) *resp_out = g_rirb[g_rirb_rp];
            return 0;
        }
        cpu_pause();
    }
    return -ETIMEDOUT;
}

static int hda_setup_corb_rirb(void)
{
    /* Allocate 4KB page for CORB (256 entries x 4B = 1024B) */
    g_corb_phys = pmm_alloc_page();
    if (!g_corb_phys) return -ENOMEM;
    g_corb = (u32 *)PHYS_TO_VIRT(g_corb_phys);
    memset(g_corb, 0, 4096);

    /* Allocate 4KB page for RIRB (256 entries x 8B = 2048B) */
    g_rirb_phys = pmm_alloc_page();
    if (!g_rirb_phys) {
        pmm_free_page(g_corb_phys);
        return -ENOMEM;
    }
    g_rirb = (u64 *)PHYS_TO_VIRT(g_rirb_phys);
    memset(g_rirb, 0, 4096);

    /* 1. Stop CORB & RIRB DMA engines */
    hda_write8(HDA_REG_CORBCTL, 0);
    hda_write8(HDA_REG_RIRBCTL, 0);
    while (hda_read8(HDA_REG_CORBCTL) & 0x02) cpu_pause();
    while (hda_read8(HDA_REG_RIRBCTL) & 0x02) cpu_pause();

    /* 2. Program base addresses */
    hda_write32(HDA_REG_CORBLBASE, (u32)(g_corb_phys & 0xFFFFFFFF));
    hda_write32(HDA_REG_CORBUBASE, (u32)(g_corb_phys >> 32));
    hda_write32(HDA_REG_RIRBLBASE, (u32)(g_rirb_phys & 0xFFFFFFFF));
    hda_write32(HDA_REG_RIRBUBASE, (u32)(g_rirb_phys >> 32));

    /* 3. Configure ring sizes (size capability bit 2 = 256 entries) */
    hda_write8(HDA_REG_CORBSIZE, 0x02);
    hda_write8(HDA_REG_RIRBSIZE, 0x02);

    /* 4. Reset CORB Read Pointer */
    hda_write16(HDA_REG_CORBRP, 0x8000); /* Reset bit */
    while (!(hda_read16(HDA_REG_CORBRP) & 0x8000)) cpu_pause();
    hda_write16(HDA_REG_CORBRP, 0x0000); /* Clear reset bit */
    while (hda_read16(HDA_REG_CORBRP) & 0x8000) cpu_pause();

    /* 5. Reset Write Pointers */
    hda_write16(HDA_REG_CORBWP, 0);
    hda_write16(HDA_REG_RIRBWP, 0x8000); /* Reset RIRB write pointer */
    g_corb_wp = 0;
    g_rirb_rp = 0;

    /* 6. Interrupt count threshold = 1 */
    hda_write16(HDA_REG_RINTCNT, 1);

    /* 7. Start CORB & RIRB DMA engines */
    hda_write8(HDA_REG_CORBCTL, 0x02);
    hda_write8(HDA_REG_RIRBCTL, 0x02);
    int timeout = 10000;
    while (!(hda_read8(HDA_REG_CORBCTL) & 0x02) && --timeout > 0) cpu_pause();

    return 0;
}

/* ── Codec Initialization ─────────────────────────────────────────────────── */

static void hda_init_codec(u8 codec_addr)
{
    /* Power up Audio Function Group (AFG node 0x01) to D0 */
    hda_send_verb(codec_addr, 0x01, HDA_VERB_SET_POWER_STATE, 0x00, NULL);

    /* Configure common DAC converters (nodes 0x02, 0x03):
     * - Unmute and set 0dB gain (0xB000 = out, mute=0, gain=0dB)
     * - Assign to Stream 1, Channel 0 (0x10)
     * - Set format 48 kHz, 16-bit stereo (0x0011) */
    for (u8 dac = 2; dac <= 3; dac++) {
        hda_send_verb(codec_addr, dac, HDA_VERB_SET_POWER_STATE, 0x00, NULL);
        hda_send_verb(codec_addr, dac, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB0, NULL);
        hda_send_verb(codec_addr, dac, HDA_VERB_SET_STREAM_CHANNEL, 0x10, NULL);
        hda_send_verb(codec_addr, dac, HDA_VERB_SET_CONV_FMT, 0x11, NULL);
    }

    /* Configure common Pin Complexes (Line Out = 0x14, HP = 0x15, Front HP = 0x1B):
     * - Set Pin Control: Output Enable (0x40) + Headphone Amp (0x80) = 0xC0
     * - Unmute Output Amplifier
     * - Turn on EAPD (External Amplifier) */
    u8 pin_nodes[] = { 0x14, 0x15, 0x16, 0x17, 0x1B, 0x1F };
    for (size_t i = 0; i < sizeof(pin_nodes); i++) {
        u8 pin = pin_nodes[i];
        hda_send_verb(codec_addr, pin, HDA_VERB_SET_POWER_STATE, 0x00, NULL);
        hda_send_verb(codec_addr, pin, HDA_VERB_SET_PIN_WIDGET_CTL, 0xC0, NULL);
        hda_send_verb(codec_addr, pin, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB0, NULL);
        hda_send_verb(codec_addr, pin, HDA_VERB_SET_EAPD_BTLENABLE, 0x02, NULL);
    }
}

/* ── Output Stream DMA Engine ─────────────────────────────────────────────── */

static int hda_setup_output_stream(u32 out_stream_off)
{
    /* Allocate 1 page for Buffer Descriptor List (BDL) */
    g_bdl_phys = pmm_alloc_page();
    if (!g_bdl_phys) return -ENOMEM;
    g_bdl = (hda_bdl_entry_t *)PHYS_TO_VIRT(g_bdl_phys);
    memset(g_bdl, 0, 4096);

    /* Allocate physically contiguous pages for audio ring buffer */
    size_t pages = (HDA_TOTAL_BUF_SIZE + 4095) / 4096;
    g_dma_buffer_phys = pmm_alloc_pages(pages);
    if (!g_dma_buffer_phys) {
        pmm_free_page(g_bdl_phys);
        return -ENOMEM;
    }
    g_dma_buffer = (u8 *)PHYS_TO_VIRT(g_dma_buffer_phys);
    memset(g_dma_buffer, 0, HDA_TOTAL_BUF_SIZE);

    /* Program 2 BDL entries for cyclic double-buffering */
    for (int i = 0; i < HDA_NUM_BDL_ENTRIES; i++) {
        g_bdl[i].addr = g_dma_buffer_phys + (i * HDA_BDL_BUF_SIZE);
        g_bdl[i].len  = HDA_BDL_BUF_SIZE;
        g_bdl[i].ioc  = 1; /* Interrupt On Completion */
    }

    u32 base = out_stream_off;

    /* 1. Reset stream engine */
    hda_write8(base + HDA_SD_CTL, 0);
    hda_write8(base + HDA_SD_CTL, 0x01); /* SRST bit */
    while (!(hda_read8(base + HDA_SD_CTL) & 0x01)) cpu_pause();
    hda_write8(base + HDA_SD_CTL, 0);
    while (hda_read8(base + HDA_SD_CTL) & 0x01) cpu_pause();

    /* 2. Program BDL Base Address */
    hda_write32(base + HDA_SD_BDPL, (u32)(g_bdl_phys & 0xFFFFFFFF));
    hda_write32(base + HDA_SD_BDPU, (u32)(g_bdl_phys >> 32));

    /* 3. Program Cyclic Buffer Length & Last Valid Index */
    hda_write32(base + HDA_SD_CBL, HDA_TOTAL_BUF_SIZE);
    hda_write16(base + HDA_SD_LVI, HDA_NUM_BDL_ENTRIES - 1);

    /* 4. Stream Format: 48 kHz, 16-bit, 2 channels = 0x0011 */
    hda_write16(base + HDA_SD_FMT, 0x0011);

    /* 5. Set Stream ID = 1 (bits 23:20) and start DMA run (bit 1) */
    u32 ctl = (1U << 20) | (1U << 1); /* Stream ID 1 | RUN */
    hda_write32(base + HDA_SD_CTL, ctl);

    g_dma_write_pos = 0;
    return 0;
}

s64 hda_play_pcm(const void *samples, size_t len)
{
    if (!g_hda_ready || !samples || len == 0) return -EINVAL;

    irqflags_t flags = spinlock_lock_irqsave(&g_hda_lock);

    const u8 *src = (const u8 *)samples;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = HDA_TOTAL_BUF_SIZE - g_dma_write_pos;
        if (chunk > remaining) chunk = remaining;

        memcpy(g_dma_buffer + g_dma_write_pos, src, chunk);
        g_dma_write_pos = (g_dma_write_pos + (u32)chunk) % HDA_TOTAL_BUF_SIZE;
        src += chunk;
        remaining -= chunk;
    }

    spinlock_unlock_irqrestore(&g_hda_lock, flags);
    return (s64)len;
}

/* ── File Operations for /dev/dsp1 ────────────────────────────────────────── */

static s64 dev_hda_write(struct file *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    return hda_play_pcm(buf, len);
}

static file_operations_t g_hda_fops = {
    .read    = NULL,
    .write   = dev_hda_write,
    .open    = NULL,
    .release = NULL,
    .ioctl   = NULL,
    .mmap    = NULL,
    .poll    = NULL
};

/* ── PCI Probe / Init ─────────────────────────────────────────────────────── */

static int hda_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    if (g_hda_ready) return -EBUSY;

    phys_addr_t mmio_phys = pci_get_bar(dm->hal, 0);
    if (!mmio_phys) return -ENODEV;

    pci_enable_bus_mastering(dm->hal);

    void *virt = vmm_map_io(mmio_phys, 16384);
    if (!virt) return -ENOMEM;
    g_hda_mmio = (virt_addr_t)virt;

    u8 vmaj = hda_read8(HDA_REG_VMAJ);
    u8 vmin = hda_read8(HDA_REG_VMIN);
    u16 gcap = hda_read16(HDA_REG_GCAP);
    u8 num_iss = (gcap >> 8) & 0x0F;

    pr_debug("[HDA] Intel HDA Controller v%d.%d (GCAP=0x%04x, %u in-streams)\n",
             vmaj, vmin, gcap, num_iss);

    /* Bring controller out of reset */
    u32 gctl = hda_read32(HDA_REG_GCTL);
    if (!(gctl & 1)) {
        hda_write32(HDA_REG_GCTL, gctl | 1);
        int timeout = 10000;
        while (!(hda_read32(HDA_REG_GCTL) & 1) && --timeout > 0) cpu_pause();
    }

    /* Wait for codecs to report state change status */
    for (volatile int i = 0; i < 500000; i++) cpu_pause();
    u16 statests = hda_read16(HDA_REG_STATESTS);

    /* Setup CORB / RIRB command rings */
    if (hda_setup_corb_rirb() < 0) {
        pr_debug("[HDA] Failed to setup CORB/RIRB ring buffers\n");
        return -EIO;
    }

    /* Initialize each detected codec */
    for (u8 c = 0; c < 15; c++) {
        if (statests & (1U << c)) {
            pr_debug("[HDA] Codec detected at SDI %u, configuring audio widgets...\n", c);
            hda_init_codec(c);
        }
    }

    /* Output Stream 1 begins after all Input Streams: 0x80 + 0x20 * num_iss */
    g_out_stream_offset = 0x80 + (0x20 * num_iss);
    if (hda_setup_output_stream(g_out_stream_offset) < 0) {
        pr_debug("[HDA] Failed to setup DMA Output Stream\n");
        return -EIO;
    }

    g_hda_ready = true;
    devfs_register_device("dsp1", &g_hda_fops, NULL);
    dm_set_drvdata(dm, &g_hda_ready);

    pr_debug("[HDA] Intel HD Audio online: 48kHz 16-bit stereo DMA active -> /dev/dsp1\n");
    return 0;
}

static void hda_remove(dm_device_t *dm)
{
    (void)dm;
    if (!g_hda_ready) return;
    hda_write32(HDA_REG_GCTL, hda_read32(HDA_REG_GCTL) & ~1u);
    g_hda_ready = false;
}

static const pci_device_id_t hda_pci_ids[] = {
    { PCI_DEVICE_CLASS(0x040300, 0xFFFF00) },
    { 0 }
};

static pci_driver_t hda_pci_driver = {
    .drv      = { .name = "hda" },
    .id_table = hda_pci_ids,
    .probe    = hda_probe,
    .remove   = hda_remove,
};

void hda_init(void)
{
    pci_driver_register(&hda_pci_driver);
}
