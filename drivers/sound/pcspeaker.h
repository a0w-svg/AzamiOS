/* ============================================================================
 * AzamiOS — PC Speaker (PIT Channel 2) Sound Driver Header
 * File: drivers/sound/pcspeaker.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/* ── IOCTL Commands for /dev/speaker and /dev/beep ───────────────────────── */
#define SPEAKER_IOC_PLAY_TONE   0x5301
#define SPEAKER_IOC_STOP_TONE   0x5302
#define SPEAKER_IOC_BEEP        0x5303
#define SPEAKER_IOC_PLAY_NOTE   0x5304

/** pcspeaker_init() — Initialize PC speaker driver and register /dev/speaker. */
void pcspeaker_init(void);

/** pcspeaker_play(freq_hz) — Start playing continuous square wave at freq_hz. */
void pcspeaker_play(u32 freq_hz);

/** pcspeaker_stop() — Silence PC speaker. */
void pcspeaker_stop(void);

/** pcspeaker_beep(freq_hz, duration_ms) — Beep synchronously or asynchronously. */
void pcspeaker_beep(u32 freq_hz, u32 duration_ms);

/** pcspeaker_note_to_freq(note_idx) — Convert MIDI note 0..127 to frequency in Hz. */
u32 pcspeaker_note_to_freq(u8 note_idx);

/** pcspeaker_play_note(note_idx, duration_ms) — Play musical note for duration. */
void pcspeaker_play_note(u8 note_idx, u32 duration_ms);
