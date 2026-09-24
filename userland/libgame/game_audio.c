/* ============================================================================
 * AzamiOS Game Framework — Audio Implementation
 * File: userland/libgame/game_audio.c
 * ============================================================================ */

#include "include/game/game_audio.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

/* sin approximation: Bhaskara I's formula (good enough for audio) */
static int16_t audio_sin_approx(int phase, int period, int amplitude)
{
    if (period <= 0) return 0;
    /* Normalize phase to [0, period) */
    phase = phase % period;
    if (phase < 0) phase += period;

    /* Map to [0, 32768) fixed point angle */
    int half = period / 2;
    int in_second_half = (phase >= half) ? 1 : 0;
    if (in_second_half) phase -= half;

    /* Quadratic approximation of sin in [0, π] */
    /* sin(x) ≈ 16x(π-x) / (5π² - 4x(π-x)) */
    int x = phase;
    int px = half - phase;
    long num = 4L * x * px;
    long den = half * (long)half;
    int value = (int)((num * amplitude) / den);

    return in_second_half ? (int16_t)(-value) : (int16_t)value;
}

/* ── Initialization ───────────────────────────────────────────────────────── */

void audio_init(audio_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->master_volume = 80;

    /* Initialize all channels as free */
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
        ctx->channels[i].sfx_id = -1;

    /* Open audio device */
    ctx->dsp_fd = open("/dev/dsp", O_WRONLY, 0);
    ctx->initialized = (ctx->dsp_fd >= 0);
}

void audio_shutdown(audio_ctx_t *ctx)
{
    audio_stop_all(ctx);

    /* Free all SFX buffers */
    for (int i = 0; i < ctx->sfx_count; i++) {
        if (ctx->sfx[i].active && ctx->sfx[i].samples) {
            free(ctx->sfx[i].samples);
            ctx->sfx[i].samples = NULL;
            ctx->sfx[i].active = false;
        }
    }

    if (ctx->dsp_fd >= 0) {
        close(ctx->dsp_fd);
        ctx->dsp_fd = -1;
    }
    ctx->initialized = false;
}

/* ── SFX Loading ──────────────────────────────────────────────────────────── */

int audio_load_sfx(audio_ctx_t *ctx, const int16_t *pcm, int sample_count, int sample_rate)
{
    if (sample_count <= 0 || !pcm) return -1;

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < AUDIO_MAX_SFX; i++) {
        if (!ctx->sfx[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    ctx->sfx[slot].samples = (int16_t *)malloc((size_t)sample_count * sizeof(int16_t));
    if (!ctx->sfx[slot].samples) return -1;

    memcpy(ctx->sfx[slot].samples, pcm, (size_t)sample_count * sizeof(int16_t));
    ctx->sfx[slot].sample_count = sample_count;
    ctx->sfx[slot].sample_rate = sample_rate;
    ctx->sfx[slot].active = true;

    if (slot >= ctx->sfx_count) ctx->sfx_count = slot + 1;
    return slot;
}

int audio_generate_tone(audio_ctx_t *ctx, int freq_hz, int duration_ms,
                         wave_type_t wave, int volume)
{
    if (freq_hz <= 0 || duration_ms <= 0 || volume <= 0) return -1;

    int sample_count = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *buf = (int16_t *)malloc((size_t)sample_count * sizeof(int16_t));
    if (!buf) return -1;

    int period = AUDIO_SAMPLE_RATE / freq_hz;
    if (period < 1) period = 1;
    int half_period = period / 2;
    int amp = (volume * 327) / 100;  /* scale 0-100 to 0-32700 */

    /* Simple RNG for noise */
    unsigned int noise_state = 0xDEADBEEFu;

    for (int i = 0; i < sample_count; i++) {
        int16_t sample = 0;

        switch (wave) {
        case WAVE_SQUARE:
            sample = ((i % period) < half_period) ? (int16_t)amp : (int16_t)(-amp);
            break;

        case WAVE_SINE:
            sample = audio_sin_approx(i, period, amp);
            break;

        case WAVE_NOISE:
            noise_state ^= noise_state << 13;
            noise_state ^= noise_state >> 17;
            noise_state ^= noise_state << 5;
            sample = (int16_t)((int)(noise_state & 0xFFFF) - 32768);
            sample = (int16_t)((sample * amp) / 32767);
            break;

        case WAVE_TRIANGLE: {
            int pos = i % period;
            if (pos < half_period) {
                sample = (int16_t)(-amp + (2 * amp * pos) / half_period);
            } else {
                sample = (int16_t)(amp - (2 * amp * (pos - half_period)) / half_period);
            }
            break;
        }

        case WAVE_SAWTOOTH: {
            int pos = i % period;
            sample = (int16_t)(-amp + (2 * amp * pos) / period);
            break;
        }
        }

        /* Envelope: quick attack, sustain, fade out last 20% */
        int env = 255;
        int attack_samples = AUDIO_SAMPLE_RATE / 200; /* 5ms attack */
        int release_start = sample_count - (sample_count / 5);

        if (i < attack_samples) {
            env = (i * 255) / attack_samples;
        } else if (i > release_start) {
            env = ((sample_count - i) * 255) / (sample_count - release_start);
        }

        buf[i] = (int16_t)((sample * env) / 255);
    }

    int sfx_id = audio_load_sfx(ctx, buf, sample_count, AUDIO_SAMPLE_RATE);
    free(buf);
    return sfx_id;
}

void audio_free_sfx(audio_ctx_t *ctx, int sfx_id)
{
    if (sfx_id < 0 || sfx_id >= AUDIO_MAX_SFX) return;
    if (!ctx->sfx[sfx_id].active) return;

    /* Stop any channels playing this SFX */
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (ctx->channels[i].sfx_id == sfx_id)
            ctx->channels[i].sfx_id = -1;
    }

    free(ctx->sfx[sfx_id].samples);
    ctx->sfx[sfx_id].samples = NULL;
    ctx->sfx[sfx_id].active = false;
}

/* ── Playback ─────────────────────────────────────────────────────────────── */

int audio_play_sfx(audio_ctx_t *ctx, int sfx_id, int volume, bool loop)
{
    if (sfx_id < 0 || sfx_id >= AUDIO_MAX_SFX) return -1;
    if (!ctx->sfx[sfx_id].active) return -1;

    /* Find a free channel */
    int ch = -1;
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (ctx->channels[i].sfx_id < 0) { ch = i; break; }
    }
    if (ch < 0) {
        /* Steal the oldest non-looping channel */
        for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
            if (!ctx->channels[i].looping) { ch = i; break; }
        }
        if (ch < 0) ch = 0;  /* last resort */
    }

    ctx->channels[ch].sfx_id = sfx_id;
    ctx->channels[ch].position = 0;
    ctx->channels[ch].volume = (volume > 255) ? 255 : (volume < 0) ? 0 : volume;
    ctx->channels[ch].looping = loop;
    return ch;
}

void audio_stop_channel(audio_ctx_t *ctx, int channel)
{
    if (channel < 0 || channel >= AUDIO_MAX_CHANNELS) return;
    ctx->channels[channel].sfx_id = -1;
}

void audio_stop_all(audio_ctx_t *ctx)
{
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++)
        ctx->channels[i].sfx_id = -1;
}

void audio_set_volume(audio_ctx_t *ctx, int volume_0_100)
{
    if (volume_0_100 < 0) volume_0_100 = 0;
    if (volume_0_100 > 100) volume_0_100 = 100;
    ctx->master_volume = volume_0_100;
}

/* ── Mixing ───────────────────────────────────────────────────────────────── */

void audio_mix_and_flush(audio_ctx_t *ctx)
{
    if (!ctx->initialized || ctx->dsp_fd < 0) return;

    /* Check if any channels are active */
    bool any_active = false;
    for (int i = 0; i < AUDIO_MAX_CHANNELS; i++) {
        if (ctx->channels[i].sfx_id >= 0) { any_active = true; break; }
    }
    if (!any_active) return;

    /* Clear mix buffer */
    memset(ctx->mix_buf, 0, sizeof(ctx->mix_buf));

    /* Mix all active channels */
    int32_t accum[AUDIO_BUFFER_SIZE];
    memset(accum, 0, sizeof(accum));

    for (int ch = 0; ch < AUDIO_MAX_CHANNELS; ch++) {
        audio_channel_t *c = &ctx->channels[ch];
        if (c->sfx_id < 0) continue;

        audio_sfx_t *sfx = &ctx->sfx[c->sfx_id];
        if (!sfx->active || !sfx->samples) {
            c->sfx_id = -1;
            continue;
        }

        int vol = (c->volume * ctx->master_volume) / 100;

        for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
            if (c->position >= sfx->sample_count) {
                if (c->looping) {
                    c->position = 0;
                } else {
                    c->sfx_id = -1;
                    break;
                }
            }
            accum[i] += ((int32_t)sfx->samples[c->position] * vol) / 255;
            c->position++;
        }
    }

    /* Clamp and write to output buffer */
    for (int i = 0; i < AUDIO_BUFFER_SIZE; i++) {
        int32_t s = accum[i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        ctx->mix_buf[i] = (int16_t)s;
    }

    /* Write to /dev/dsp */
    write(ctx->dsp_fd, ctx->mix_buf, sizeof(ctx->mix_buf));
}
