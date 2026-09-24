/* ============================================================================
 * AzamiOS Game Framework — Audio Subsystem
 * File: userland/libgame/include/game/game_audio.h
 *
 * Sound effects and music via /dev/dsp:
 *  • Procedural tone synthesis (square, sine approx, noise)
 *  • SFX slot system (up to AUDIO_MAX_SFX loaded effects)
 *  • Software mixing of up to AUDIO_MAX_CHANNELS concurrent sounds
 *  • Per-channel and master volume control
 *  • Fire-and-forget playback API
 *
 * Tuning defines:
 *   AUDIO_MAX_SFX       — max loaded sound effects (default 32)
 *   AUDIO_MAX_CHANNELS  — max concurrent playback (default 8)
 *   AUDIO_SAMPLE_RATE   — output sample rate (default 44100)
 *   AUDIO_BUFFER_SIZE   — mixing buffer size in samples (default 2048)
 * ============================================================================ */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef AUDIO_MAX_SFX
#define AUDIO_MAX_SFX      32
#endif
#ifndef AUDIO_MAX_CHANNELS
#define AUDIO_MAX_CHANNELS 8
#endif
#ifndef AUDIO_SAMPLE_RATE
#define AUDIO_SAMPLE_RATE  44100
#endif
#ifndef AUDIO_BUFFER_SIZE
#define AUDIO_BUFFER_SIZE  2048
#endif

/* ── Sound Effect ─────────────────────────────────────────────────────────── */

typedef struct {
    int16_t *samples;       /* PCM sample data (owned, malloc'd) */
    int      sample_count;
    int      sample_rate;
    bool     active;        /* slot is in use */
} audio_sfx_t;

/* ── Playback Channel ─────────────────────────────────────────────────────── */

typedef struct {
    int      sfx_id;        /* index into sfx array, -1 = free */
    int      position;      /* current sample position */
    int      volume;        /* 0..255 */
    bool     looping;
} audio_channel_t;

/* ── Audio Context ────────────────────────────────────────────────────────── */

typedef struct {
    int              dsp_fd;     /* /dev/dsp file descriptor */
    audio_sfx_t      sfx[AUDIO_MAX_SFX];
    int              sfx_count;
    audio_channel_t  channels[AUDIO_MAX_CHANNELS];
    int              master_volume; /* 0..100 */
    bool             initialized;

    /* Mixing buffer */
    int16_t          mix_buf[AUDIO_BUFFER_SIZE];
} audio_ctx_t;

/* ── Tone Waveform Types ──────────────────────────────────────────────────── */

typedef enum {
    WAVE_SQUARE,
    WAVE_SINE,      /* approximated sine */
    WAVE_NOISE,
    WAVE_TRIANGLE,
    WAVE_SAWTOOTH
} wave_type_t;

/* ── API (implemented in game_audio.c) ────────────────────────────────────── */

void audio_init(audio_ctx_t *ctx);
void audio_shutdown(audio_ctx_t *ctx);

/* Load a PCM buffer as a sound effect. Returns sfx_id or -1. */
int  audio_load_sfx(audio_ctx_t *ctx, const int16_t *pcm, int sample_count, int sample_rate);

/* Generate a procedural tone and load it. Returns sfx_id. */
int  audio_generate_tone(audio_ctx_t *ctx, int freq_hz, int duration_ms,
                          wave_type_t wave, int volume);

/* Free a loaded SFX slot. */
void audio_free_sfx(audio_ctx_t *ctx, int sfx_id);

/* Play a loaded SFX on the next free channel. Returns channel index or -1. */
int  audio_play_sfx(audio_ctx_t *ctx, int sfx_id, int volume, bool loop);

/* Stop a specific channel. */
void audio_stop_channel(audio_ctx_t *ctx, int channel);

/* Stop all channels. */
void audio_stop_all(audio_ctx_t *ctx);

/* Set master volume (0..100). */
void audio_set_volume(audio_ctx_t *ctx, int volume_0_100);

/* Mix and flush active channels to /dev/dsp. Call each frame or on a timer. */
void audio_mix_and_flush(audio_ctx_t *ctx);
