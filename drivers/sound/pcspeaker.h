/* ============================================================================
 * AzamiOS — PC Speaker (PIT Channel 2) Sound Driver Header
 * File: drivers/sound/pcspeaker.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** pcspeaker_init() — Initialize PC speaker driver and register /dev/speaker in devfs. */
void pcspeaker_init(void);

/** pcspeaker_play(freq_hz) — Start playing continuous square wave at freq_hz. */
void pcspeaker_play(u32 freq_hz);

/** pcspeaker_stop() — Silence PC speaker. */
void pcspeaker_stop(void);

/** pcspeaker_beep(freq_hz, duration_ms) — Beep synchronously or asynchronously. */
void pcspeaker_beep(u32 freq_hz, u32 duration_ms);
