/* ============================================================================
 * AzamiOS — PC Speaker (PIT Channel 2) Sound Driver
 * File: drivers/sound/pcspeaker.c
 * ============================================================================ */

#include "pcspeaker.h"
#include "../../fs/vfs.h"
#include "../../include/azami/defs.h"
#include "../../kernel/sched/sched.h"
#include "../../drivers/char/console.h"

#define PIT_BASE_FREQ 1193180

void pcspeaker_play(u32 freq_hz)
{
    if (freq_hz == 0 || freq_hz > 20000) {
        pcspeaker_stop();
        return;
    }

    u32 div = PIT_BASE_FREQ / freq_hz;
    if (div > 65535) div = 65535;
    if (div == 0) div = 1;

    outb(0x43, 0xB6); /* Channel 2, LSB/MSB, square wave mode */
    outb(0x42, (u8)(div & 0xFF));
    outb(0x42, (u8)((div >> 8) & 0xFF));

    u8 tmp = inb(0x61);
    if ((tmp & 3) != 3) {
        outb(0x61, tmp | 3);
    }
}

void pcspeaker_stop(void)
{
    u8 tmp = inb(0x61);
    outb(0x61, tmp & 0xFC);
}

void pcspeaker_beep(u32 freq_hz, u32 duration_ms)
{
    if (freq_hz == 0) freq_hz = 750;
    if (duration_ms == 0) duration_ms = 100;

    pcspeaker_play(freq_hz);
    u64 ticks = (duration_ms + 9) / 10;
    if (ticks == 0) ticks = 1;
    sched_sleep(ticks);
    pcspeaker_stop();
}

/* ── Character device operations for /dev/speaker ────────────────────────── */

static s64 speaker_write(file_t *filp, const void *buf, size_t len, u64 *offset)
{
    (void)filp; (void)offset;
    if (!buf || len == 0) return 0;

    const char *str = (const char *)buf;
    u32 freq = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] >= '0' && str[i] <= '9') {
            freq = freq * 10 + (str[i] - '0');
        } else if (str[i] == '\n' || str[i] == ' ') {
            break;
        }
    }

    if (freq == 0 && len >= 4) {
        /* Binary format: 32-bit frequency */
        freq = *(const u32 *)buf;
    }

    if (freq > 0) {
        pcspeaker_play(freq);
    } else {
        pcspeaker_stop();
    }
    return (s64)len;
}

static s64 speaker_ioctl(file_t *filp, u32 cmd, u64 arg)
{
    (void)filp;
    switch (cmd) {
    case 0x5301: /* PLAY_TONE */
        pcspeaker_play((u32)arg);
        return 0;
    case 0x5302: /* STOP_TONE */
        pcspeaker_stop();
        return 0;
    case 0x5303: /* BEEP */
        pcspeaker_beep((u32)(arg & 0xFFFF), (u32)(arg >> 16));
        return 0;
    default:
        return -(s64)EINVAL;
    }
}

static file_operations_t speaker_fops = {
    .read = NULL,
    .write = speaker_write,
    .ioctl = speaker_ioctl,
};

void pcspeaker_init(void)
{
    pcspeaker_stop();
    devfs_register_device("speaker", &speaker_fops, NULL);
    devfs_register_device("beep", &speaker_fops, NULL);
}
