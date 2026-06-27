#include "./include/ac97.h"
#include "./include/pci.h"
#include "../arch/include/isr.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../klibc/include/string.h"
#include "../mem/include/mmp.h"
#include "../mem/include/paging.h"
#include "../mem/include/pmm.h"
#include <stdbool.h>

/* ── Intel AC'97 PCI identifiers ────────────────────────────────────────── */
#define AC97_VENDOR_ID  0x8086
#define AC97_DEVICE_ID  0x2415

/* ── Native Audio Mixer (NAM) registers — offsets from BAR0 ─────────────── */
#define NAM_RESET           0x00  /* Write any value to reset codec         */
#define NAM_MASTER_VOL      0x02  /* Master output volume (L/R attenuation) */
#define NAM_PCM_OUT_VOL     0x18  /* PCM output volume                     */
#define NAM_SAMPLE_RATE     0x2C  /* Front DAC sample rate                 */
#define NAM_EXT_AUDIO_ID    0x28  /* Extended Audio ID                     */
#define NAM_EXT_AUDIO_CTRL  0x2A  /* Extended Audio Status/Control         */

/* ── Native Audio Bus Master (NABM) registers — offsets from BAR1 ────── */
#define NABM_PCM_OUT_BDBAR  0x10  /* Buffer Descriptor List Base Address   */
#define NABM_PCM_OUT_CIV    0x14  /* Current Index Value (byte)            */
#define NABM_PCM_OUT_LVI    0x15  /* Last Valid Index (byte)               */
#define NABM_PCM_OUT_SR     0x16  /* Status Register (16-bit)              */
#define NABM_PCM_OUT_CR     0x1B  /* Control Register (byte)               */
#define NABM_GLOB_CNT       0x2C  /* Global Control (32-bit)               */
#define NABM_GLOB_STA       0x30  /* Global Status  (32-bit)               */

/* ── Control register bits ──────────────────────────────────────────────── */
#define CR_RPBM    (1 << 0)  /* Run/Pause Bus Master                       */
#define CR_RR      (1 << 1)  /* Reset Registers                            */
#define CR_LVBIE   (1 << 2)  /* Last Valid Buffer Interrupt Enable          */
#define CR_FEIE    (1 << 3)  /* FIFO Error Interrupt Enable                */
#define CR_IOCE    (1 << 4)  /* Interrupt On Completion Enable              */

/* ── Status register bits ───────────────────────────────────────────────── */
#define SR_DCH     (1 << 0)  /* DMA Controller Halted                      */
#define SR_CELV    (1 << 1)  /* Current Equals Last Valid                   */
#define SR_LVBCI   (1 << 2)  /* Last Valid Buffer Completion Interrupt      */
#define SR_BCIS    (1 << 3)  /* Buffer Completion Interrupt Status          */
#define SR_FIFOE   (1 << 4)  /* FIFO Error                                 */

/* ── Global Control bits ────────────────────────────────────────────────── */
#define GC_GIE     (1 << 0)  /* GPI Interrupt Enable                       */
#define GC_CR      (1 << 1)  /* Cold Reset                                 */
#define GC_WR      (1 << 2)  /* Warm Reset                                 */

/* ── BDL (Buffer Descriptor List) ───────────────────────────────────────── */
#define BDL_MAX_ENTRIES  32

/* BDL entry flags */
#define BDL_FLAG_IOC     (1 << 15)  /* Interrupt On Completion              */
#define BDL_FLAG_BUP     (1 << 14)  /* Buffer Underrun Policy               */

/*
    Buffer Descriptor List entry (8 bytes);
    Each entry points to a physical buffer of PCM sample data;
*/
typedef struct __attribute__((packed)) {
    uint32_t phys_addr;     /* Physical address of the sample buffer       */
    uint16_t num_samples;   /* Number of samples in the buffer (not bytes) */
    uint16_t flags;         /* Control flags (IOC, BUP)                    */
} ac97_bdl_entry_t;

/* ── Driver state ───────────────────────────────────────────────────────── */
static bool     ac97_present  = false;
static uint16_t nam_base      = 0;    /* BAR0: mixer register I/O base     */
static uint16_t nabm_base     = 0;    /* BAR1: bus master I/O base         */
static uint8_t  ac97_irq      = 0;    /* IRQ line from PCI config space    */

/* BDL must be aligned and in physically contiguous low memory */
static ac97_bdl_entry_t *bdl  = NULL;
static uint32_t          bdl_phys = 0;

/* DMA audio buffer — single contiguous page for sample data */
#define AC97_DMA_BUF_SAMPLES  0x8000   /* 32768 samples = 64 KB @ 16-bit   */
static int16_t *dma_buffer     = NULL;
static uint32_t dma_buffer_phys = 0;

/* ── Low-level NAM/NABM helpers ─────────────────────────────────────────── */
static void nam_write16(uint16_t reg, uint16_t val) { outw(nam_base + reg, val); }
static uint16_t nam_read16(uint16_t reg)            { return inw(nam_base + reg); }

static void nabm_write8 (uint16_t reg, uint8_t  val) { outb(nabm_base + reg, val); }
static void nabm_write16(uint16_t reg, uint16_t val) { outw(nabm_base + reg, val); }
static void nabm_write32(uint16_t reg, uint32_t val) { outl(nabm_base + reg, val); }
static uint8_t __attribute__((unused)) nabm_read8 (uint16_t reg) { return inb(nabm_base + reg); }
static uint16_t nabm_read16(uint16_t reg) { return inw(nabm_base + reg); }
static uint32_t __attribute__((unused)) nabm_read32(uint16_t reg) { return inl(nabm_base + reg); }

/* ── IRQ handler ────────────────────────────────────────────────────────── */
static void ac97_irq_handler(registers_t *r)
{
    UNUSED(r);
    uint16_t sr = nabm_read16(NABM_PCM_OUT_SR);
    /* Acknowledge all status bits by writing them back */
    nabm_write16(NABM_PCM_OUT_SR, sr & (SR_LVBCI | SR_BCIS | SR_FIFOE));
}

/* ── Physical memory allocation helpers ─────────────────────────────────── */
/*
    Allocate contiguous physical pages and identity-map them;
    Returns the physical (= virtual, identity mapped) address;
*/
static uint32_t alloc_phys_pages(uint32_t num_pages)
{
    /* Allocate the first page to get a base address */
    void *first = pmm_alloc_block();
    if (!first) return 0;
    uint32_t base = (uint32_t)first;
    paging_map_page(base, base, 0, 1);

    /* Allocate remaining contiguous pages */
    for (uint32_t i = 1; i < num_pages; i++) {
        void *page = pmm_alloc_block();
        uint32_t addr = (uint32_t)page;
        paging_map_page(addr, addr, 0, 1);
    }
    return base;
}

/* ── Startup tone generation ────────────────────────────────────────────── */
/*
    Generate a 440 Hz sine-wave tone (signed 16-bit stereo, 48 kHz);
    Uses a polynomial approximation of sin() to avoid needing libm;
*/
static int16_t fast_sin16(uint32_t phase)
{
    /*
        phase is 0..65535 representing 0..2π;
        Use a parabolic approximation of sine;
    */
    int32_t x = (int32_t)(phase & 0xFFFF) - 32768; /* -32768..32767 */
    int32_t abs_x = x < 0 ? -x : x;
    /* Map to -32768..32767 with correct sign for sine */
    int32_t shifted = (int32_t)(phase & 0xFFFF);
    int32_t half = shifted < 32768 ? shifted : 65536 - shifted;
    /* Scale to -1..1 range in fixed point (Q15) */
    int32_t normalized = (half - 16384) * 2; /* -32768..32767 */
    /* Parabolic sine approximation: sin(x) ≈ 4x(1-|x|) / scale */
    int32_t abs_n = normalized < 0 ? -normalized : normalized;
    int32_t result = (normalized * (32768 - abs_n)) >> 14;
    UNUSED(abs_x);
    UNUSED(x);
    return (int16_t)result;
}

static void ac97_play_startup_tone(void)
{
    if (!ac97_present || !dma_buffer || !bdl) return;

    uint32_t sample_rate  = 48000;
    uint32_t tone_freq    = 440;
    uint32_t duration_ms  = 250;
    uint32_t total_frames = (sample_rate * duration_ms) / 1000; /* mono frames */
    uint32_t total_samples = total_frames * 2; /* stereo: L + R per frame */

    if (total_samples > AC97_DMA_BUF_SAMPLES)
        total_samples = AC97_DMA_BUF_SAMPLES;

    /* Generate tone into DMA buffer */
    uint32_t phase = 0;
    uint32_t phase_inc = (tone_freq * 65536) / sample_rate;
    for (uint32_t i = 0; i < total_samples; i += 2) {
        int16_t sample = fast_sin16(phase & 0xFFFF);
        /* Apply a simple envelope: fade in/out over first/last 512 frames */
        uint32_t frame = i / 2;
        if (frame < 512) {
            sample = (int16_t)((int32_t)sample * frame / 512);
        } else if (frame > total_frames - 512) {
            uint32_t remaining = total_frames - frame;
            sample = (int16_t)((int32_t)sample * remaining / 512);
        }
        /* Scale down to ~50% amplitude to avoid harshness */
        sample = sample / 2;
        dma_buffer[i]     = sample; /* Left  */
        dma_buffer[i + 1] = sample; /* Right */
        phase += phase_inc;
    }

    /* Set up BDL: single entry pointing to entire buffer */
    uint32_t buf_samples = total_samples / 2; /* AC'97 BDL counts in sample frames (stereo pairs) */
    /* Split into chunks of at most 0xFFFE samples per BDL entry */
    uint32_t entries = 0;
    uint32_t remaining = buf_samples;
    uint32_t offset = 0;
    while (remaining > 0 && entries < BDL_MAX_ENTRIES) {
        uint16_t chunk = remaining > 0xFFFE ? 0xFFFE : (uint16_t)remaining;
        bdl[entries].phys_addr   = dma_buffer_phys + offset * 4; /* 4 bytes per stereo frame */
        bdl[entries].num_samples = chunk;
        bdl[entries].flags       = (remaining <= chunk) ? (BDL_FLAG_IOC | BDL_FLAG_BUP) : BDL_FLAG_IOC;
        offset    += chunk;
        remaining -= chunk;
        entries++;
    }

    /* Try setting sample rate (requires Variable Rate Audio support) */
    uint16_t ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) { /* VRA bit */
        uint16_t ext_ctrl = nam_read16(NAM_EXT_AUDIO_CTRL);
        nam_write16(NAM_EXT_AUDIO_CTRL, ext_ctrl | 0x0001); /* Enable VRA */
        nam_write16(NAM_SAMPLE_RATE, (uint16_t)sample_rate);
    }

    /* Reset PCM-out DMA engine */
    nabm_write8(NABM_PCM_OUT_CR, CR_RR);
    /* Wait for reset to complete */
    for (volatile int i = 0; i < 10000; i++);
    nabm_write8(NABM_PCM_OUT_CR, 0);

    /* Load BDL base address */
    nabm_write32(NABM_PCM_OUT_BDBAR, bdl_phys);

    /* Set Last Valid Index */
    nabm_write8(NABM_PCM_OUT_LVI, (uint8_t)(entries - 1));

    /* Clear status bits */
    nabm_write16(NABM_PCM_OUT_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);

    /* Start playback with interrupts enabled */
    nabm_write8(NABM_PCM_OUT_CR, CR_RPBM | CR_LVBIE | CR_IOCE);

    kprintf("ac97: playing 440 Hz startup tone (%d ms)\n", duration_ms);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/*
    Initialize the AC'97 sound driver;
*/
void ac97_init(void)
{
    uint8_t bus, slot, func;
    kprintf("\nac97: probing PCI bus for Intel AC'97 Audio...\n");

    if (!pci_find_device(AC97_VENDOR_ID, AC97_DEVICE_ID, &bus, &slot, &func)) {
        kprintf("ac97: device not found (add -device ac97 to QEMU)\n");
        return;
    }

    kprintf("ac97: found at PCI %d:%d.%d\n", bus, slot, func);

    /* Read BAR0 (NAM) and BAR1 (NABM) I/O base addresses */
    nam_base  = (uint16_t)(pci_config_read32(bus, slot, func, 0x10) & 0xFFFC);
    nabm_base = (uint16_t)(pci_config_read32(bus, slot, func, 0x14) & 0xFFFC);
    kprintf("ac97: NAM I/O base = 0x%x, NABM I/O base = 0x%x\n", nam_base, nabm_base);

    /* Read IRQ line from PCI config */
    ac97_irq = (uint8_t)(pci_config_read32(bus, slot, func, 0x3C) & 0xFF);
    kprintf("ac97: IRQ line = %d\n", ac97_irq);

    /* Enable PCI Bus Mastering (set bit 2 of PCI Command register) */
    uint16_t pci_cmd = pci_config_read16(bus, slot, func, 0x04);
    pci_cmd |= (1 << 2) | (1 << 0); /* Bus Master + I/O Space */
    pci_config_write16(bus, slot, func, 0x04, pci_cmd);

    /* Register IRQ handler */
    if (ac97_irq >= 32) {
        /* IRQ number already includes PIC offset, use directly */
        register_interrupt_handler(ac97_irq, ac97_irq_handler);
    } else {
        /* Raw IRQ number — add PIC offset (IRQ0 = 32) */
        register_interrupt_handler(ac97_irq + 32, ac97_irq_handler);
    }

    /* ── Cold Reset the AC'97 controller ──────────────────────────────── */
    nabm_write32(NABM_GLOB_CNT, GC_CR);
    /* Wait for codec ready */
    /* Busy-loop delay (~50ms) — cannot use pit_wait because init_pit may not be called yet */
    for (volatile int _d = 0; _d < 500000; _d++);

    /* Enable PCM output via Global Control */
    nabm_write32(NABM_GLOB_CNT, GC_GIE | GC_CR);

    /* ── Reset the codec via NAM ──────────────────────────────────────── */
    nam_write16(NAM_RESET, 0x0001);
    /* Busy-loop delay (~20ms) */
    for (volatile int _d = 0; _d < 200000; _d++);

    /* ── Set volumes to maximum (0 attenuation) ───────────────────────── */
    nam_write16(NAM_MASTER_VOL, 0x0000);   /* Master: L=0, R=0 (max) */
    nam_write16(NAM_PCM_OUT_VOL, 0x0808);  /* PCM: moderate volume   */

    /* ── Allocate BDL (32 entries × 8 bytes = 256 bytes, needs 1 page) ─ */
    bdl_phys = alloc_phys_pages(1);
    if (!bdl_phys) {
        kprintf("ac97: failed to allocate BDL memory\n");
        return;
    }
    bdl = (ac97_bdl_entry_t *)bdl_phys;
    memset(bdl, 0, BDL_MAX_ENTRIES * sizeof(ac97_bdl_entry_t));

    /* ── Allocate DMA sample buffer (AC97_DMA_BUF_SAMPLES × 2 bytes) ── */
    uint32_t buf_bytes = AC97_DMA_BUF_SAMPLES * sizeof(int16_t);
    uint32_t buf_pages = (buf_bytes + 4095) / 4096;
    dma_buffer_phys = alloc_phys_pages(buf_pages);
    if (!dma_buffer_phys) {
        kprintf("ac97: failed to allocate DMA buffer\n");
        return;
    }
    dma_buffer = (int16_t *)dma_buffer_phys;
    memset(dma_buffer, 0, buf_bytes);

    ac97_present = true;
    kprintf("ac97: driver initialized successfully\n");

    /* Play startup tone */
    ac97_play_startup_tone();
}

/*
    Set master playback volume;
*/
void ac97_set_volume(uint8_t vol)
{
    if (!ac97_present) return;
    /* AC'97 volume register: bits 5:0 = attenuation (0=max, 63=mute) */
    uint16_t reg = ((uint16_t)(vol & 0x3F) << 8) | (vol & 0x3F);
    nam_write16(NAM_MASTER_VOL, reg);
}

/*
    Play signed 16-bit stereo PCM samples;
*/
void ac97_play(const int16_t *samples, uint32_t num_samples, uint32_t sample_rate)
{
    if (!ac97_present || !samples || num_samples == 0) return;

    /* Clamp to buffer size */
    if (num_samples > AC97_DMA_BUF_SAMPLES)
        num_samples = AC97_DMA_BUF_SAMPLES;

    /* Copy samples into DMA buffer */
    memcpy(dma_buffer, samples, num_samples * sizeof(int16_t));

    /* Try setting sample rate if VRA supported */
    uint16_t ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        uint16_t ext_ctrl = nam_read16(NAM_EXT_AUDIO_CTRL);
        nam_write16(NAM_EXT_AUDIO_CTRL, ext_ctrl | 0x0001);
        nam_write16(NAM_SAMPLE_RATE, (uint16_t)sample_rate);
    }

    /* Stop any current playback */
    nabm_write8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile int i = 0; i < 10000; i++);
    nabm_write8(NABM_PCM_OUT_CR, 0);

    /* Set up BDL entries */
    uint32_t stereo_frames = num_samples / 2;
    uint32_t entries = 0;
    uint32_t remaining = stereo_frames;
    uint32_t offset = 0;
    while (remaining > 0 && entries < BDL_MAX_ENTRIES) {
        uint16_t chunk = remaining > 0xFFFE ? 0xFFFE : (uint16_t)remaining;
        bdl[entries].phys_addr   = dma_buffer_phys + offset * 4;
        bdl[entries].num_samples = chunk;
        bdl[entries].flags       = (remaining <= chunk) ? (BDL_FLAG_IOC | BDL_FLAG_BUP) : BDL_FLAG_IOC;
        offset    += chunk;
        remaining -= chunk;
        entries++;
    }

    /* Load BDL and start playback */
    nabm_write32(NABM_PCM_OUT_BDBAR, bdl_phys);
    nabm_write8(NABM_PCM_OUT_LVI, (uint8_t)(entries - 1));
    nabm_write16(NABM_PCM_OUT_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
    nabm_write8(NABM_PCM_OUT_CR, CR_RPBM | CR_LVBIE | CR_IOCE);
}

/*
    Stop any ongoing DMA playback;
*/
void ac97_stop(void)
{
    if (!ac97_present) return;
    /* Clear Run bit and reset DMA engine */
    nabm_write8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile int i = 0; i < 10000; i++);
    nabm_write8(NABM_PCM_OUT_CR, 0);
    /* Clear status */
    nabm_write16(NABM_PCM_OUT_SR, SR_LVBCI | SR_BCIS | SR_FIFOE);
}
