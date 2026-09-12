/* ============================================================================
 * AzamiOS — PC Speaker (PIT Channel 2) Sound Driver
 * File: drivers/sound/pcspeaker.c
 * ============================================================================ */

#include "pcspeaker.h"
#include "../../fs/vfs.h"
#include "../../include/azami/defs.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/uaccess.h"
#include "../../kernel/lib/string.h"
#include "../../drivers/char/console.h"

#define PIT_BASE_FREQ 1193180

static const u16 g_note_freqs[128] = {
    8, 9, 9, 10, 10, 11, 12, 12, 13, 14, 15, 15,          /* 0..11: C-1..B-1 */
    16, 17, 18, 19, 21, 22, 23, 25, 26, 28, 29, 31,       /* 12..23: C0..B0 */
    33, 35, 37, 39, 41, 44, 46, 49, 52, 55, 58, 62,       /* 24..35: C1..B1 */
    65, 69, 73, 78, 82, 87, 93, 98, 104, 110, 117, 123,   /* 36..47: C2..B2 */
    131, 139, 147, 156, 165, 175, 185, 196, 208, 220, 233, 247, /* 48..59: C3..B3 */
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494, /* 60..71: C4..B4 (60=Mid C, 69=A440) */
    523, 554, 587, 622, 659, 698, 740, 784, 831, 880, 932, 988, /* 72..83: C5..B5 */
    1047, 1109, 1175, 1245, 1319, 1397, 1480, 1568, 1661, 1760, 1865, 1976, /* 84..95: C6..B6 */
    2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729, 3951, /* 96..107: C7..B7 */
    4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902, /* 108..119: C8..B8 */
    8372, 8870, 9397, 9956, 10548, 11175, 11840, 12544   /* 120..127: C9..G9 */
};

u32 pcspeaker_note_to_freq(u8 note_idx)
{
    if (note_idx >= 128) return 0;
    return g_note_freqs[note_idx];
}

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

void pcspeaker_play_note(u8 note_idx, u32 duration_ms)
{
    u32 freq = pcspeaker_note_to_freq(note_idx);
    if (freq > 0) {
        pcspeaker_beep(freq, duration_ms);
    }
}

/* ── Character device operations for /dev/speaker ────────────────────────── */

static s64 speaker_read(file_t *filp, void *buf, size_t len, u64 *offset)
{
    (void)filp;
    if (!buf || len == 0) return 0;
    if (*offset > 0) return 0;

    const char *status = "PC Speaker Synthesizer (PIT Channel 2) active\n";
    size_t slen = strlen(status);
    if (len > slen) len = slen;
    memcpy(buf, status, len);
    *offset += len;
    return (s64)len;
}

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
    case SPEAKER_IOC_PLAY_TONE: /* 0x5301 */
        pcspeaker_play((u32)arg);
        return 0;
    case SPEAKER_IOC_STOP_TONE: /* 0x5302 */
        pcspeaker_stop();
        return 0;
    case SPEAKER_IOC_BEEP:      /* 0x5303 */
        pcspeaker_beep((u32)(arg & 0xFFFF), (u32)(arg >> 16));
        return 0;
    case SPEAKER_IOC_PLAY_NOTE: /* 0x5304: arg[7:0] = note, arg[31:8] = duration_ms */ {
        u8 note = (u8)(arg & 0xFF);
        u32 dur = (u32)(arg >> 8);
        if (dur == 0) {
            pcspeaker_play(pcspeaker_note_to_freq(note));
        } else {
            pcspeaker_play_note(note, dur);
        }
        return 0;
    }
    default:
        return -(s64)EINVAL;
    }
}

static file_operations_t speaker_fops = {
    .read  = speaker_read,
    .write = speaker_write,
    .ioctl = speaker_ioctl,
};

#include "../base/platform.h"

static int pcspeaker_platform_probe(platform_device_t *pdev)
{
    (void)pdev;
    pr_debug("[PCSPKR] PC speaker platform device active (/dev/speaker, /dev/beep)\n");
    return 0;
}

static platform_driver_t g_pcspkr_platform_driver = {
    .drv = {
        .name = "pcspkr",
    },
    .probe  = pcspeaker_platform_probe,
    .remove = NULL,
};

void pcspeaker_init(void)
{
    pcspeaker_stop();
    devfs_register_device("speaker", &speaker_fops, NULL);
    devfs_register_device("beep", &speaker_fops, NULL);
    platform_driver_register(&g_pcspkr_platform_driver);
}
