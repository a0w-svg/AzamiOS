/* ============================================================================
 * AzamiOS — Sound Blaster 16 Audio Driver
 * File: drivers/sound/sb16.c
 * ============================================================================ */

#define DEBUG 1
#include <azami/debug.h>
#include "sb16.h"
#include "sound.h"
#include "../../arch/x86_64/cpu/pic.h"
#include "../../kernel/mm/kmalloc.h"
#include "../../kernel/mm/pmm.h"
#include "../../arch/x86_64/mm/vmm.h"
#include "../../arch/x86_64/cpu/spinlock.h"

#define SB_PORT_MIXER_ADDR   (SB16_DEFAULT_BASE + 0x04)
#define SB_PORT_MIXER_DATA   (SB16_DEFAULT_BASE + 0x05)
#define SB_PORT_RESET        (SB16_DEFAULT_BASE + 0x06)
#define SB_PORT_READ         (SB16_DEFAULT_BASE + 0x0A)
#define SB_PORT_WRITE        (SB16_DEFAULT_BASE + 0x0C)
#define SB_PORT_STATUS       (SB16_DEFAULT_BASE + 0x0C)
#define SB_PORT_DATA_AVAIL   (SB16_DEFAULT_BASE + 0x0E)
#define SB_PORT_16_ACK       (SB16_DEFAULT_BASE + 0x0F)

static spinlock_t g_sb16_lock = SPINLOCK_INIT;
static bool g_sb16_present = false;
static u32 g_sb16_samplerate = 44100;

static u8 sb16_dsp_read(void)
{
    for (int i = 0; i < 10000; i++) {
        if (inb(SB_PORT_DATA_AVAIL) & 0x80)
            return inb(SB_PORT_READ);
    }
    return 0;
}

static void sb16_dsp_write(u8 val)
{
    for (int i = 0; i < 10000; i++) {
        if ((inb(SB_PORT_WRITE) & 0x80) == 0) {
            outb(SB_PORT_WRITE, val);
            return;
        }
    }
}

static bool sb16_reset_dsp(void)
{
    outb(SB_PORT_RESET, 1);
    for (volatile int i = 0; i < 1000; i++) { __asm__ volatile("pause"); }
    outb(SB_PORT_RESET, 0);

    for (int i = 0; i < 1000; i++) {
        if ((inb(SB_PORT_DATA_AVAIL) & 0x80) && inb(SB_PORT_READ) == 0xAA) {
            return true;
        }
    }
    return false;
}

static s64 sb16_write_pcm(const u8 *data, u64 len)
{
    if (!g_sb16_present || !data || len == 0) return 0;

    spinlock_lock(&g_sb16_lock);

    /* Turn speaker on */
    sb16_dsp_write(0xD1);

    /* Set output sample rate */
    sb16_dsp_write(0x41);
    sb16_dsp_write((u8)((g_sb16_samplerate >> 8) & 0xFF));
    sb16_dsp_write((u8)(g_sb16_samplerate & 0xFF));

    spinlock_unlock(&g_sb16_lock);
    return (s64)len;
}

static s64 sb16_ioctl(u64 cmd, void *arg)
{
    (void)cmd;
    (void)arg;
    return 0;
}

static sound_ops_t g_sb16_ops = {
    .write_pcm = sb16_write_pcm,
    .ioctl = sb16_ioctl,
};

static sound_device_t g_sb16_dev = {
    .name = "sb16",
    .ops = &g_sb16_ops,
};

void sb16_init(void)
{
    if (!sb16_reset_dsp()) {
        pr_debug("[SB16] Sound Blaster 16 not detected at port 0x%x.\n", SB16_DEFAULT_BASE);
        return;
    }

    /* Query DSP version */
    sb16_dsp_write(0xE1);
    u8 major = sb16_dsp_read();
    u8 minor = sb16_dsp_read();

    if (major < 4) {
        pr_debug("[SB16] Found legacy Sound Blaster v%u.%u (SB16 requires v4+).\n", major, minor);
        return;
    }

    g_sb16_present = true;
    sound_register_device(&g_sb16_dev);
    pr_debug("[SB16] Sound Blaster 16 DSP v%u.%u initialized on port 0x%x\n",
             major, minor, SB16_DEFAULT_BASE);
}
