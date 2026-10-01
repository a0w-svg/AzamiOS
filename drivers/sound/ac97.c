/* Intel ICH AC'97: stereo signed 16-bit PCM through a 32-entry DMA ring. */
#define DEBUG 1
#include "../../include/azami/debug.h"
#include "../../include/azami/defs.h"
#include "ac97.h"
#include "sound.h"
#include "../../hal/device.h"
#include "../base/pci_bus.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"
#include "../../kernel/uaccess.h"

#define AC97_BDL_ENTRIES 32
#define AC97_BUFFER_SIZE 4096
#define AC97_FRAME_SIZE 4

static u32 g_nam_bar, g_nabm_bar;
static ac97_bdl_entry_t *g_bdl;
static u8 *g_audio_buffers[AC97_BDL_ENTRIES];
static spinlock_t g_ac97_lock = SPINLOCK_INIT;
static u8 g_ac97_lvi;
static bool g_ac97_started, g_ac97_ready, g_ac97_vra;
static u32 g_ac97_rate = 48000;
static u32 g_ac97_volume = 0x6464;
static sound_device_t g_ac97_sound_dev;

static void ac97_outb(u32 bar, u16 off, u8 v) { outb(bar + off, v); }
static void ac97_outw(u32 bar, u16 off, u16 v) { outw(bar + off, v); }
static void ac97_outd(u32 bar, u16 off, u32 v) { outl(bar + off, v); }
static u8 ac97_inb(u32 bar, u16 off) { return inb(bar + off); }
static u16 ac97_inw(u32 bar, u16 off) { return inw(bar + off); }
static u32 ac97_ind(u32 bar, u16 off) { return inl(bar + off); }

/* Serialize codec transactions with the controller as well as other CPUs.
 * POST-port reads give bounded delays without depending on scheduler startup. */
static int ac97_codec_acquire(void)
{
    if (!(ac97_ind(g_nabm_bar, AC97_GLOB_STA) & AC97_GLOB_READY))
        return -ENODEV;
    for (unsigned i = 0; i < 1000; i++) {
        if (!(ac97_inb(g_nabm_bar, AC97_CAS) & 1)) return 0;
        inb(0x80);
    }
    return -ETIMEDOUT;
}

static int ac97_codec_write(u16 reg, u16 value)
{
    int ret = ac97_codec_acquire();
    if (ret) return ret;
    ac97_outw(g_nam_bar, reg, value);
    return 0;
}

static int ac97_codec_read(u16 reg, u16 *value)
{
    int ret = ac97_codec_acquire();
    if (ret) return ret;
    *value = ac97_inw(g_nam_bar, reg);
    if (ac97_ind(g_nabm_bar, AC97_GLOB_STA) & AC97_GLOB_RCS) {
        ac97_outd(g_nabm_bar, AC97_GLOB_STA, AC97_GLOB_RCS);
        return -EIO;
    }
    return 0;
}

/* Called with the driver lock held, or before publishing the device. */
static int ac97_reset_pcm(void)
{
    ac97_outb(g_nabm_bar, AC97_PO_CR, 0);
    unsigned i;
    for (i = 0; i < 1000; i++) {
        if (ac97_inw(g_nabm_bar, AC97_PO_SR) & AC97_SR_DCH) break;
        inb(0x80);
    }
    if (i == 1000) return -ETIMEDOUT;
    ac97_outb(g_nabm_bar, AC97_PO_CR, AC97_CR_RESET);
    for (i = 0; i < 1000; i++) {
        if (!(ac97_inb(g_nabm_bar, AC97_PO_CR) & AC97_CR_RESET)) break;
        inb(0x80);
    }
    if (i == 1000) return -ETIMEDOUT;
    g_ac97_started = false;
    g_ac97_lvi = 0;
    ac97_outw(g_nabm_bar, AC97_PO_SR, AC97_SR_ACK);
    if (g_bdl)
        ac97_outd(g_nabm_bar, AC97_PO_BDBAR,
                  (u32)VIRT_TO_PHYS((virt_addr_t)g_bdl));
    return 0;
}

static unsigned ac97_free_slots(u8 civ)
{
    /* Never overwrite CIV, even when the engine has just exhausted LVI.
     * Publishing a new LVI resumes at the prefetched *next* descriptor. */
    if (!g_ac97_started) return AC97_BDL_ENTRIES;
    return (civ - g_ac97_lvi - 1) & (AC97_BDL_ENTRIES - 1);
}

static s64 ac97_write_pcm(const u8 *data, u64 len)
{
    if (!len) return 0;
    if (len < AC97_FRAME_SIZE) return -EINVAL;
    len &= ~(u64)(AC97_FRAME_SIZE - 1);
    irqflags_t irqf = spinlock_lock_irqsave(&g_ac97_lock);
    if (!g_ac97_ready) {
        spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
        return -ENODEV;
    }
    u8 civ = ac97_inb(g_nabm_bar, AC97_PO_CIV) & 31;
    unsigned free_slots = ac97_free_slots(civ);
    u8 slot = g_ac97_started ? ((g_ac97_lvi + 1) & 31) : civ;
    u64 written = 0;
    while (free_slots-- && written < len) {
        u64 chunk = MIN(len - written, (u64)AC97_BUFFER_SIZE);
        __builtin_memcpy(g_audio_buffers[slot], data + written, chunk);
        /* DMA addresses are fixed at allocation, not recalculated per write.
         * No IOC: users submit synchronously and inspect CIV for capacity;
         * there is no completion worker needing a per-buffer interrupt. */
        g_bdl[slot].samples = chunk / 2;
        g_bdl[slot].flags = 0;
        g_ac97_lvi = slot;
        slot = (slot + 1) & 31;
        written += chunk;
    }
    if (written) {
        /* Publish the whole batch only after all PCM and descriptors are
         * visible to DMA. An old CIV snapshot only underestimates capacity. */
        wmb();
        ac97_outb(g_nabm_bar, AC97_PO_LVI, g_ac97_lvi);
        if (!g_ac97_started) {
            ac97_outb(g_nabm_bar, AC97_PO_CR, AC97_CR_RUN);
            g_ac97_started = true;
        }
    }
    spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
    return written ? (s64)written : -EAGAIN;
}

static int ac97_set_rate(s32 requested)
{
    if (requested < 0) return -EINVAL;
    if (!requested) return 0; /* OSS query */
    u32 rate = requested;
    if (rate < 8000) rate = 8000;
    if (rate > 48000 || !g_ac97_vra) rate = 48000;
    if (rate == g_ac97_rate) return 0;
    if (g_ac97_started &&
        !(ac97_inw(g_nabm_bar, AC97_PO_SR) & AC97_SR_DCH)) return -EBUSY;
    int ret = ac97_codec_write(AC97_NAMBAR_PCM_FRONT_RATE, rate);
    u16 actual;
    if (!ret) ret = ac97_codec_read(AC97_NAMBAR_PCM_FRONT_RATE, &actual);
    if (ret) return ret;
    if (actual < 8000 || actual > 48000) return -EIO;
    g_ac97_rate = actual;
    return 0;
}

static int ac97_set_volume(u32 value)
{
    u32 left = MIN(value & 0xff, 100U);
    u32 right = MIN((value >> 8) & 0xff, 100U);
    u16 raw = ((31 - left * 31 / 100) << 8) | (31 - right * 31 / 100);
    if (!left && !right) raw |= 0x8000; /* mute, rather than just attenuate */
    int ret = ac97_codec_write(AC97_NAMBAR_MASTER_VOL, raw);
    /* PCM output remains at unity: avoid attenuating every sample twice. */
    if (!ret) g_ac97_volume = left | (right << 8);
    return ret;
}

static s64 ac97_ioctl(u64 cmd, void *arg)
{
    s32 value = 0;
    bool input = false, output = true;
    size_t out_size = sizeof(value);
    ac97_audio_buf_info_t space;
    const void *out = &value;
    switch (cmd) {
    case SOUND_PCM_WRITE_RATE:
    case SOUND_PCM_WRITE_VOLUME:
        output = false;
        input = true;
        break;
    case AC97_DSP_SPEED:
    case AC97_DSP_STEREO:
    case AC97_DSP_SETFMT:
    case AC97_DSP_CHANNELS:
        input = true;
        break;
    case SOUND_PCM_READ_RATE:
    case SOUND_PCM_READ_VOLUME:
    case AC97_DSP_GETBLKSIZE:
    case AC97_DSP_GETFMTS:
    case AC97_PCM_READ_BITS:
    case AC97_PCM_READ_CHANNELS:
        break;
    case AC97_DSP_GETOSPACE:
        out = &space;
        out_size = sizeof(space);
        break;
    case AC97_DSP_RESET:
        output = false;
        break;
    default:
        return -ENOTTY;
    }
    /* Faultable user copies must run outside the IRQ-disabled driver lock. */
    if ((input || output) &&
        (!arg || (uintptr_t)arg > TASK_SIZE_MAX - out_size)) return -EFAULT;
    if (input && copy_from_user(&value, arg, sizeof(value))) return -EFAULT;
    irqflags_t irqf = spinlock_lock_irqsave(&g_ac97_lock);
    int ret = 0;
    if (!g_ac97_ready) {
        ret = -ENODEV;
        goto done;
    }
    switch (cmd) {
    case AC97_DSP_RESET:
        ret = ac97_reset_pcm();
        /* A failed reset cannot safely accept new DMA work. */
        if (ret) g_ac97_ready = false;
        break;
    case SOUND_PCM_WRITE_VOLUME:
        ret = ac97_set_volume((u32)value);
        break;
    case SOUND_PCM_READ_VOLUME:
        value = g_ac97_volume;
        break;
    case SOUND_PCM_WRITE_RATE:
    case AC97_DSP_SPEED:
        ret = ac97_set_rate(value);
        value = g_ac97_rate;
        break;
    case SOUND_PCM_READ_RATE:
        value = g_ac97_rate;
        break;
    case AC97_DSP_STEREO:
        value = 1;
        break;
    case AC97_DSP_CHANNELS:
    case AC97_PCM_READ_CHANNELS:
        value = 2;
        break;
    case AC97_DSP_SETFMT:
    case AC97_DSP_GETFMTS:
        value = AC97_AFMT_S16_LE;
        break;
    case AC97_PCM_READ_BITS:
        value = 16;
        break;
    case AC97_DSP_GETBLKSIZE:
        value = AC97_BUFFER_SIZE;
        break;
    case AC97_DSP_GETOSPACE:
        space.fragments = ac97_free_slots(ac97_inb(g_nabm_bar, AC97_PO_CIV) & 31);
        space.fragstotal = AC97_BDL_ENTRIES;
        space.fragsize = AC97_BUFFER_SIZE;
        space.bytes = space.fragments * space.fragsize;
        break;
    }
done:
    spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
    if (!ret && output && copy_to_user(arg, out, out_size)) return -EFAULT;
    return ret;
}

static sound_ops_t g_ac97_ops = {
    .write_pcm = ac97_write_pcm,
    .ioctl = ac97_ioctl
};

static void ac97_free_dma(void)
{
    for (unsigned i = 0; i < AC97_BDL_ENTRIES; i++) {
        if (g_audio_buffers[i]) {
            pmm_free_pages(VIRT_TO_PHYS((virt_addr_t)g_audio_buffers[i]), 1);
            g_audio_buffers[i] = NULL;
        }
    }
    if (g_bdl) {
        pmm_free_pages(VIRT_TO_PHYS((virt_addr_t)g_bdl), 1);
        g_bdl = NULL;
    }
}

static int ac97_probe(dm_device_t *dm, const pci_device_id_t *id)
{
    (void)id;
    pci_device_info_t *pci = to_pci_info(dm);
    if (!pci || !dm->hal) return -ENODEV;
    /* pci_get_bar() strips the type bits; validate the raw BARs instead. */
    u32 nam = pci->bar[0], nabm = pci->bar[1];
    /* This transport uses I/O BARs. Never truncate an MMIO BAR to a port. */
    if (!(nam & 1) || !(nabm & 1) || !(nam & ~3U) || !(nabm & ~3U) ||
        (nam & ~3U) > 0xff00 || (nabm & ~3U) > 0xffc0) return -ENODEV;
    irqflags_t irqf = spinlock_lock_irqsave(&g_ac97_lock);
    if (g_nam_bar || g_nabm_bar) {
        spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
        return -EBUSY;
    }
    g_nam_bar = nam & ~3U;
    g_nabm_bar = nabm & ~3U;
    spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
    u16 command = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command | 5);

    /* Select 16-bit stereo and bring the AC link out of cold reset. If
     * firmware already enabled the link, request a warm reset instead. */
    u32 control = ac97_ind(g_nabm_bar, AC97_GLOB_CNT);
    ac97_outd(g_nabm_bar, AC97_GLOB_CNT,
              AC97_GLOB_COLD | ((control & AC97_GLOB_COLD) ? AC97_GLOB_WARM : 0));
    int ret = -ETIMEDOUT;
    for (unsigned i = 0; i < 100000; i++) {
        if (!(ac97_ind(g_nabm_bar, AC97_GLOB_CNT) & AC97_GLOB_WARM) &&
            (ac97_ind(g_nabm_bar, AC97_GLOB_STA) & AC97_GLOB_READY)) {
            ret = 0;
            break;
        }
        inb(0x80);
    }
    if (ret) goto fail;
    ret = ac97_reset_pcm();
    if (ret) goto fail;
    u16 caps, ext;
    ret = ac97_codec_read(AC97_NAMBAR_EXT_AUDIO_ID, &caps);
    if (ret) goto fail;
    g_ac97_vra = false;
    if (caps & AC97_EXT_VRA) {
        ret = ac97_codec_read(AC97_NAMBAR_EXT_AUDIO_CTRL, &ext);
        if (ret) goto fail;
        ret = ac97_codec_write(AC97_NAMBAR_EXT_AUDIO_CTRL, ext | AC97_EXT_VRA);
        if (ret) goto fail;
        ret = ac97_codec_read(AC97_NAMBAR_EXT_AUDIO_CTRL, &ext);
        if (ret) goto fail;
        g_ac97_vra = !!(ext & AC97_EXT_VRA);
    }
    g_ac97_rate = 48000;
    if (g_ac97_vra) {
        ret = ac97_codec_write(AC97_NAMBAR_PCM_FRONT_RATE, 48000);
        if (ret) goto fail;
    }
    /* PCM gain code 8 is 0 dB; code 0 adds 12 dB on real codecs. */
    ret = ac97_codec_write(AC97_NAMBAR_PCM_OUT_VOL, 0x0808);
    if (ret) goto fail;
    ret = ac97_set_volume(0x6464);
    if (ret) goto fail;

    phys_addr_t bdl_phys = pmm_alloc_pages_32(1);
    if (!bdl_phys) { ret = -ENOMEM; goto fail; }
    g_bdl = (ac97_bdl_entry_t *)PHYS_TO_VIRT(bdl_phys);
    __builtin_memset(g_bdl, 0, AC97_BDL_ENTRIES * sizeof(*g_bdl));
    for (unsigned i = 0; i < AC97_BDL_ENTRIES; i++) {
        phys_addr_t buf_phys = pmm_alloc_pages_32(1);
        if (!buf_phys) { ret = -ENOMEM; goto fail; }
        g_audio_buffers[i] = (u8 *)PHYS_TO_VIRT(buf_phys);
        g_bdl[i].ptr = (u32)buf_phys;
    }
    ac97_outd(g_nabm_bar, AC97_PO_BDBAR, (u32)bdl_phys);
    g_ac97_ready = true;
    pr_debug("[AC97] Intel AC97 ready, %s rates, NAM 0x%x NABM 0x%x\n",
             g_ac97_vra ? "variable" : "48000 Hz fixed", g_nam_bar, g_nabm_bar);
    __builtin_memcpy(g_ac97_sound_dev.name, "Intel AC97", 11);
    g_ac97_sound_dev.ops = &g_ac97_ops;
    sound_register_device(&g_ac97_sound_dev);
    dm_set_drvdata(dm, &g_ac97_sound_dev);
    return 0;
fail:
    pr_debug("[AC97] Probe failed for %02x:%02x.%x: %d\n",
             pci->bus, pci->slot, pci->func, ret);
    /* Disable bus mastering before freeing even a partially built ring. */
    pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, command & ~4U);
    pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    ac97_free_dma();
    g_nam_bar = g_nabm_bar = 0;
    g_ac97_ready = false;
    return ret;
}

static void ac97_remove(dm_device_t *dm)
{
    irqflags_t irqf = spinlock_lock_irqsave(&g_ac97_lock);
    g_ac97_ready = false;
    ac97_outb(g_nabm_bar, AC97_PO_CR, 0);
    pci_device_info_t *pci = to_pci_info(dm);
    if (pci) {
        u16 cmd = pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
        pci_config_write16(pci->bus, pci->slot, pci->func, 0x04, cmd & ~4U);
        pci_config_read16(pci->bus, pci->slot, pci->func, 0x04);
    }
    ac97_free_dma();
    g_nam_bar = g_nabm_bar = 0;
    g_ac97_started = false;
    dm_set_drvdata(dm, NULL);
    spinlock_unlock_irqrestore(&g_ac97_lock, irqf);
}

static const pci_device_id_t ac97_pci_ids[] = {
    { PCI_DEVICE(0x8086, 0x2415) }, /* 82801AA / ICH */
    { PCI_DEVICE(0x8086, 0x2425) }, /* 82801AB / ICH0 */
    { PCI_DEVICE(0x8086, 0x2445) }, /* 82801BA / ICH2 */
    { PCI_DEVICE(0x8086, 0x2485) }, /* 82801CA / ICH3 */
    { PCI_DEVICE(0x8086, 0x7195) }, /* 82443MX / 440MX */
    { 0 }
};

static pci_driver_t ac97_pci_driver = {
    .drv = { .name = "ac97" },
    .id_table = ac97_pci_ids,
    .probe = ac97_probe,
    .remove = ac97_remove,
};

void ac97_init(void)
{
    pci_driver_register(&ac97_pci_driver);
}
