/* ============================================================================
 * AzamiOS — Audio Player & Synthesizer (v2.0)
 * File: userland/apps/audioplayer/main.c
 *
 * A small polyphonic synthesizer with a player UI in front of it.
 *
 * ── Talking to /dev/dsp ─────────────────────────────────────────────────────
 * The device is the AC'97 front DAC, which runs at its power-on default of
 * 48 kHz and takes 16-bit *stereo interleaved* frames — the driver never
 * reprograms the rate, so those are simply the terms.  Handing it mono at
 * 44.1 kHz, as this player used to, means every pair of samples is read as one
 * L/R frame: the music comes out an octave-and-a-bit sharp and smeared across
 * the two channels.  Everything here is generated at SAMPLE_RATE in CHANNELS
 * interleaved channels for that reason.
 *
 * Writes are non-blocking and return short — the driver takes what fits in its
 * DMA ring and reports how much.  A chunk is exactly one ring slot, and an
 * unaccepted chunk is retried rather than dropped, which is what paces the
 * synthesizer to real time without a clock.
 *
 * ── Features ────────────────────────────────────────────────────────────────
 *  • Phase-accurate square/triangle voices with attack-decay envelopes
 *  • Real 16-band spectrum measured from the audio (Goertzel), not faked
 *  • Seekable progress bar, draggable volume, scroll-wheel volume
 *  • Interactive tracklist, auto-advance, play/pause/stop
 *  • Catppuccin Mocha glassmorphic UI
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/sys/syscall.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W       580
#define WIN_H       430
#define MAP_ADDR    ((void *)0x6B000000)

/*
 * The AC'97 front DAC's power-on rate and frame format.  These are not a
 * preference — they are what the hardware is running at.
 */
#define SAMPLE_RATE  48000
#define CHANNELS     2

/* One write = one DMA ring slot (4096 bytes = 1024 stereo frames ≈ 21 ms). */
#define CHUNK_FRAMES 1024

/*
 * Each track is a 32-step phrase a couple of bars long — a few seconds on its
 * own.  A chiptune track is that phrase repeated, so one "play" is this many
 * passes, which is what makes a track roughly half a minute long and the
 * progress bar mean something.
 */
#define TRACK_LOOPS 8

#define NUM_TRACKS  4

typedef struct {
    const char *title;
    const char *artist;
    const char *duration;
    int bpm;
    int num_notes;
    const int *melody;
    const int *bass;
} track_t;

/* Musical frequencies (Hz) */
#define N_C3  130
#define N_D3  146
#define N_E3  164
#define N_F3  174
#define N_G3  196
#define N_A3  220
#define N_B3  246
#define N_C4  261
#define N_D4  293
#define N_E4  329
#define N_F4  349
#define N_G4  392
#define N_A4  440
#define N_B4  493
#define N_C5  523
#define N_D5  587
#define N_E5  659
#define N_G5  784
#define N_A5  880
#define N_REST 0

/* Track 0: Cyber Horizon (Synthwave) */
static const int t0_melody[] = {
    N_A4, N_C5, N_E5, N_A5, N_G5, N_E5, N_D5, N_E5,
    N_F4, N_A4, N_C5, N_F4, N_G4, N_B4, N_D5, N_G4,
    N_C4, N_E4, N_G4, N_C5, N_D5, N_C5, N_B4, N_G4,
    N_A4, N_E4, N_A4, N_B4, N_C5, N_B4, N_A4, N_E4
};
static const int t0_bass[] = {
    N_A3, N_A3, N_A3, N_A3, N_F3, N_F3, N_G3, N_G3,
    N_C3, N_C3, N_C3, N_C3, N_A3, N_A3, N_E3, N_E3
};

/* Track 1: Neon Sakura (Chiptune) */
static const int t1_melody[] = {
    N_E5, N_G5, N_A5, N_G5, N_E5, N_D5, N_E5, N_C5,
    N_D5, N_E5, N_D5, N_C5, N_A4, N_C5, N_D5, N_E5,
    N_G5, N_A5, N_G5, N_E5, N_G5, N_A5, N_C5, N_D5,
    N_E5, N_D5, N_C5, N_A4, N_C5, N_D5, N_C5, N_A4
};
static const int t1_bass[] = {
    N_A3, N_E3, N_A3, N_E3, N_F3, N_C3, N_F3, N_C3,
    N_C3, N_G3, N_C3, N_G3, N_A3, N_E3, N_A3, N_E3
};

/* Track 2: Azami Fanfare (Arcade Victory) */
static const int t2_melody[] = {
    N_C4, N_E4, N_G4, N_C5, N_REST, N_G4, N_C5, N_REST,
    N_E5, N_D5, N_C5, N_D5, N_E5, N_G5, N_E5, N_C5,
    N_A4, N_C5, N_E5, N_A5, N_G5, N_E5, N_C5, N_D5,
    N_C5, N_REST, N_C5, N_REST, N_C5, N_E5, N_C5, N_REST
};
static const int t2_bass[] = {
    N_C3, N_G3, N_C3, N_G3, N_C3, N_G3, N_C3, N_G3,
    N_F3, N_C3, N_F3, N_C3, N_G3, N_D3, N_G3, N_D3
};

/* Track 3: Cosmic Starlight (Ambient) */
static const int t3_melody[] = {
    N_A4, N_E5, N_C5, N_B4, N_G4, N_D5, N_B4, N_A4,
    N_F4, N_C5, N_A4, N_G4, N_E4, N_B4, N_G4, N_E4,
    N_A4, N_C5, N_E5, N_D5, N_B4, N_G4, N_E4, N_G4,
    N_A4, N_B4, N_C5, N_D5, N_E5, N_D5, N_C5, N_B4
};
static const int t3_bass[] = {
    N_A3, N_A3, N_G3, N_G3, N_F3, N_F3, N_E3, N_E3,
    N_A3, N_A3, N_G3, N_G3, N_F3, N_F3, N_E3, N_E3
};

static const track_t g_tracks[NUM_TRACKS] = {
    { "Cyber Horizon",  "AzamiOS Synthwave",  "0:32", 140, 32, t0_melody, t0_bass },
    { "Neon Sakura",    "Chiptune Project",   "0:32", 130, 32, t1_melody, t1_bass },
    { "Azami Fanfare",  "Arcade Orchestra",   "0:28", 150, 32, t2_melody, t2_bass },
    { "Cosmic Dreams",  "Ambient Deep Space", "0:40", 110, 32, t3_melody, t3_bass }
};

static uk_window_t g_win;
static int g_current_track = 0;
static int g_is_playing = 0;
static int g_volume = 80; /* 0..100 */
static int g_note_idx = 0;
static int g_loop_idx = 0;          /* pass through the phrase, 0..TRACK_LOOPS */
static int g_sample_in_note = 0;
static unsigned int g_tick = 0;
static int g_dsp_fd = -1;

/* Oscillator phase, 0..2^32 wrapping.  Advancing a phase by a fixed increment
 * keeps a note exactly in tune whatever its frequency, and lets a new note
 * start from the previous phase instead of snapping to zero — a phase jump
 * mid-waveform is an audible click on every note. */
static unsigned int g_phase_mel = 0;
static unsigned int g_phase_bass = 0;

/* Frames handed to the device, i.e. the play position in real time. */
static unsigned int g_frames_played = 0;

/* A chunk the device would not take yet, retried on the next pass. */
static short g_pending[CHUNK_FRAMES * CHANNELS];
static int   g_pending_bytes = 0;
static int   g_pending_off = 0;

/* Spectrum Visualizer State (16 bands) */
#define NUM_BANDS 16
static int g_spectrum[NUM_BANDS];
static int g_peaks[NUM_BANDS];

/*
 * Goertzel coefficients, 2*cos(2*pi*f/48000) in Q14, for 16 bands spaced
 * logarithmically from 120 Hz to 9 kHz.  One filter per band over the chunk
 * just generated gives a spectrum that actually follows the music, which a
 * display driven off the note index cannot do.
 */
static const int g_band_coeff[NUM_BANDS] = {
    32764, 32761, 32755, 32745, 32728, 32696, 32640, 32541,
    32365, 32052, 31498, 30520, 28807, 25837, 20791, 12540
};

/* UI interaction state */
static int g_mouse_x = -1, g_mouse_y = -1;
static int g_prev_buttons = 0;
static int g_drag_volume = 0;      /* volume slider grabbed */
static int g_drag_seek = 0;        /* progress bar grabbed  */
static int g_ui_dirty = 1;

/* phase_step(freq) — how far the oscillator advances per frame. */
static unsigned int phase_step(int freq)
{
    if (freq <= 0) return 0;
    return (unsigned int)(((unsigned long long)freq << 32) / SAMPLE_RATE);
}

/* track_frames(trk) — length of one pass through the track, in frames. */
static unsigned int samples_per_beat(const track_t *trk)
{
    int spb = (SAMPLE_RATE * 60) / (trk->bpm * 4);
    if (spb < 1) spb = 1;
    return (unsigned int)spb;
}

static unsigned int track_frames(const track_t *trk)
{
    return samples_per_beat(trk) * (unsigned int)trk->num_notes * TRACK_LOOPS;
}

/*
 * Measure the spectrum of the chunk just generated.
 *
 * One Goertzel filter per band over the mono sum: cheap (16 x 1024 multiply-
 * accumulates per 21 ms of audio) and, unlike a display driven off the note
 * index, it shows what is actually coming out of the speaker — bass notes
 * light the low bands, the square-wave melody lights its harmonics.
 */
static void analyse_chunk(const short *frames, int nframes)
{
    for (int b = 0; b < NUM_BANDS; b++) {
        long long s1 = 0, s2 = 0;
        int coeff = g_band_coeff[b];

        for (int i = 0; i < nframes; i++) {
            /* Mono sum, scaled down to leave headroom in the accumulator. */
            long long x = (frames[i * CHANNELS] + frames[i * CHANNELS + 1]) >> 5;
            long long s0 = ((coeff * s1) >> 14) - s2 + x;
            s2 = s1;
            s1 = s0;
        }

        /* |X|^2 = s1^2 + s2^2 - coeff*s1*s2 */
        long long mag2 = s1 * s1 + s2 * s2 - ((coeff * s1 % 32768) * s2 >> 14);
        if (mag2 < 0) mag2 = 0;

        /* Integer sqrt, then a shift to land in the 0..80 bar range. */
        long long r = 0, bit = 1LL << 40;
        while (bit > mag2) bit >>= 2;
        while (bit) {
            if (mag2 >= r + bit) { mag2 -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }

        int level = (int)(r >> 9);
        if (level > 80) level = 80;

        /* Fast attack, slow release — the way a bar meter is expected to move. */
        if (g_spectrum[b] < level) g_spectrum[b] = level;
        else g_spectrum[b] = (g_spectrum[b] * 7) / 8;

        if (g_peaks[b] < g_spectrum[b]) g_peaks[b] = g_spectrum[b];
        else if (g_peaks[b] > 0) g_peaks[b]--;
    }
}

/* Fill one chunk with the next slice of the track. */
static void synth_chunk(short *out, int nframes)
{
    const track_t *trk = &g_tracks[g_current_track];
    unsigned int spb = samples_per_beat(trk);

    for (int i = 0; i < nframes; i++) {
        int note_m = trk->melody[g_note_idx % trk->num_notes];
        int note_b = trk->bass[(g_note_idx / 2) % 16];

        /*
         * Envelope: a 2 ms ramp in, then a decay across the rest of the note.
         * The ramp is what stops each note beginning with a click; the decay
         * is what keeps the bass from turning into a continuous drone.
         */
        unsigned int attack = SAMPLE_RATE / 500;   /* 2 ms */
        int env;
        if ((unsigned int)g_sample_in_note < attack) {
            env = (int)((256u * (unsigned int)g_sample_in_note) / attack);
        } else {
            unsigned int rem = spb - (unsigned int)g_sample_in_note;
            env = (int)((256u * rem) / spb);
            if (env > 256) env = 256;
        }
        if (env < 0) env = 0;

        int mel = 0, bass = 0;

        /* Melody: square wave. */
        if (note_m > 0) {
            g_phase_mel += phase_step(note_m);
            int amp = (7000 * env) / 256;
            mel = (g_phase_mel < 0x80000000u) ? amp : -amp;
        }

        /* Bass: triangle, an octave of body under the melody without the
         * harshness a second square would add. */
        if (note_b > 0) {
            g_phase_bass += phase_step(note_b);
            int amp = (5000 * ((env + 512) / 3)) / 256;
            unsigned int ph = g_phase_bass;
            int tri = (ph < 0x80000000u)
                        ? (int)((ph >> 15) - 32768)
                        : (int)(32768 - ((ph - 0x80000000u) >> 15));
            bass = (tri * amp) / 32768;
        }

        /* The melody sits slightly right of centre and the bass dead centre,
         * which gives the two voices somewhere to sit apart from each other. */
        int left  = (mel * 7) / 10 + bass;
        int right = mel + bass;

        left  = (left  * g_volume) / 100;
        right = (right * g_volume) / 100;

        if (left  >  32767) left  =  32767;
        if (left  < -32768) left  = -32768;
        if (right >  32767) right =  32767;
        if (right < -32768) right = -32768;

        out[i * CHANNELS]     = (short)left;
        out[i * CHANNELS + 1] = (short)right;

        g_sample_in_note++;
        if ((unsigned int)g_sample_in_note >= spb) {
            g_sample_in_note = 0;
            g_note_idx++;
            if (g_note_idx >= trk->num_notes) {
                g_note_idx = 0;
                g_loop_idx++;
                if (g_loop_idx >= TRACK_LOOPS) {
                    /* Track over: move to the next one, as an album would. */
                    g_loop_idx = 0;
                    g_current_track = (g_current_track + 1) % NUM_TRACKS;
                    g_frames_played = 0;
                    trk = &g_tracks[g_current_track];
                    spb = samples_per_beat(trk);
                    g_ui_dirty = 1;
                }
            }
        }
    }
}

/*
 * Hand one chunk to the device.
 *
 * The driver takes what fits in its DMA ring and returns how much — a short
 * write is normal, not an error, and the remainder must be kept rather than
 * dropped or the music develops gaps.  Holding an unaccepted chunk until the
 * ring drains is also what paces generation to real time.
 */
static void audio_step(void)
{
    if (!g_is_playing) return;

    if (g_dsp_fd < 0) {
        g_dsp_fd = open("/dev/dsp", O_WRONLY, 0);
        if (g_dsp_fd < 0) {
            g_is_playing = 0;
            g_ui_dirty = 1;
            return;
        }
    }

    /* Nothing outstanding: generate the next slice. */
    if (g_pending_bytes == 0) {
        synth_chunk(g_pending, CHUNK_FRAMES);
        analyse_chunk(g_pending, CHUNK_FRAMES);
        g_pending_bytes = (int)sizeof(g_pending);
        g_pending_off   = 0;
        g_ui_dirty = 1;
    }

    int n = (int)write(g_dsp_fd,
                       (const char *)g_pending + g_pending_off,
                       (size_t)(g_pending_bytes - g_pending_off));
    if (n <= 0) return;   /* ring full — try again next pass */

    g_pending_off += n;
    g_frames_played += (unsigned int)n / (CHANNELS * (int)sizeof(short));

    if (g_pending_off >= g_pending_bytes) {
        g_pending_bytes = 0;
        g_pending_off   = 0;
    }
}

/*
 * Jump to a position in the current track, in frames from its start, and drop
 * whatever was queued for the old position so the move is heard immediately
 * rather than after the ring drains.
 */
static void seek_to_frame(unsigned int frame)
{
    const track_t *trk = &g_tracks[g_current_track];
    unsigned int spb = samples_per_beat(trk);
    unsigned int total = track_frames(trk);
    if (frame >= total) frame = total ? total - 1 : 0;

    unsigned int step = frame / spb;
    g_note_idx       = (int)(step % (unsigned int)trk->num_notes);
    g_loop_idx       = (int)(step / (unsigned int)trk->num_notes);
    g_sample_in_note = (int)(frame % spb);
    g_frames_played  = frame;

    g_pending_bytes = 0;
    g_pending_off = 0;
    g_ui_dirty = 1;
}

static void select_track(int idx)
{
    g_current_track = ((idx % NUM_TRACKS) + NUM_TRACKS) % NUM_TRACKS;
    seek_to_frame(0);
}

/* ── Layout ─────────────────────────────────────────────────────────────────
 * Drawing and hit-testing read the same numbers from here.  When they were
 * two sets of literals they drifted, and a button stopped answering where it
 * was painted. */
#define CARD_X    16
#define CARD_Y    52
#define CARD_W    330
#define CARD_H    130
#define CTRL_Y    (CARD_Y + CARD_H + 10)
#define BTN_H     32
#define PREV_X    CARD_X
#define PREV_W    44
#define PLAY_X    (CARD_X + 50)
#define PLAY_W    70
#define STOP_X    (CARD_X + 126)
#define STOP_W    54
#define NEXT_X    (CARD_X + 186)
#define NEXT_W    44
#define VOL_X     (CARD_X + 275)
#define VOL_W     45
#define VOL_Y     (CTRL_Y + 12)
#define PROG_X    (CARD_X + 16)
#define PROG_Y    (CARD_Y + 90)
#define PROG_W    (CARD_W - 32)
#define LIST_Y    260
#define LIST_ROW  34

static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/* A control is hovered when the pointer is over it and no drag is running. */
static uk_btn_state_t btn_state(int rx, int ry, int rw, int rh)
{
    if (g_mouse_x < 0) return UK_BTN_NORMAL;
    if (!hit(g_mouse_x, g_mouse_y, rx, ry, rw, rh)) return UK_BTN_NORMAL;
    return (g_prev_buttons & 1) ? UK_BTN_PRESSED : UK_BTN_HOVER;
}

static void draw_player(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* ── Header ──────────────────────────────────────────────────────────── */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_MAUVE);
    uk_draw_text(&g_win, 16, 8, "Azami Audio Player", UK_TEXT);
    uk_draw_text(&g_win, 16, 24, "Chiptune & Synthwave Synthesizer Engine", UK_OVERLAY0);
    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    /* ── Now Playing Card (Left 320px) ────────────────────────────────────── */
    int card_x = 16, card_y = 52, card_w = 330, card_h = 130;
    uk_fill_rounded_rect(&g_win, card_x, card_y, card_w, card_h, 8, UK_SURFACE0);
    uk_draw_rounded_rect_outline(&g_win, card_x, card_y, card_w, card_h, 8, UK_SURFACE1);

    /* Animated Rotating Album Vinyl Disc Icon */
    uk_fill_circle(&g_win, card_x + 40, card_y + 40, 28, UK_CRUST);
    uk_fill_circle(&g_win, card_x + 40, card_y + 40, 20, UK_SURFACE1);
    uk_fill_circle(&g_win, card_x + 40, card_y + 40, 12, UK_SURFACE2);
    uk_fill_circle(&g_win, card_x + 40, card_y + 40, 6, g_is_playing ? UK_MAUVE : UK_OVERLAY0);

    /*
     * Groove spoke, rotating with playback.  A 16-step sine/cosine table beats
     * trying to fake a circle from the angle arithmetic directly — the old
     * piecewise expression traced a diamond that jumped rather than turned.
     */
    if (g_is_playing) {
        static const int sin16[16] = {  0,  7, 13, 17, 18, 17, 13,  7,
                                        0, -7,-13,-17,-18,-17,-13, -7 };
        int step = (int)((g_frames_played / (SAMPLE_RATE / 12)) % 16);
        int dx = sin16[step];
        int dy = -sin16[(step + 4) % 16];   /* cos = sin shifted a quarter turn */
        uk_draw_line_aa(&g_win, card_x + 40, card_y + 40,
                        card_x + 40 + dx, card_y + 40 + dy, UK_LAVENDER);
    }

    /* Track metadata */
    const track_t *cur = &g_tracks[g_current_track];
    uk_draw_text(&g_win, card_x + 80, card_y + 18, cur->title, UK_TEXT);
    uk_draw_text(&g_win, card_x + 80, card_y + 36, cur->artist, UK_SUBTEXT0);

    /* Status badge */
    if (g_is_playing) {
        uk_fill_rounded_rect(&g_win, card_x + 80, card_y + 54, 60, 16, 4, UK_GREEN);
        uk_draw_text(&g_win, card_x + 86, card_y + 56, "PLAYING", UK_BASE);
    } else {
        uk_fill_rounded_rect(&g_win, card_x + 80, card_y + 54, 52, 16, 4, UK_SURFACE2);
        uk_draw_text(&g_win, card_x + 86, card_y + 56, "PAUSED", UK_TEXT);
    }

    /*
     * Progress and elapsed time come from frames actually handed to the
     * device, so the readout is the real playback position rather than a
     * count of notes dressed up as a clock.
     */
    unsigned int total_frames = track_frames(cur);
    unsigned int pos_frames   = g_frames_played;
    if (pos_frames > total_frames) pos_frames = total_frames;

    uk_fill_rounded_rect(&g_win, PROG_X, PROG_Y, PROG_W, 8, 4, UK_SURFACE1);
    int fill_w = total_frames ? (int)(((unsigned long long)pos_frames * PROG_W) / total_frames) : 0;
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, PROG_X, PROG_Y, fill_w, 8, 4, UK_MAUVE);
    }
    /* Scrub handle, so the bar reads as something you can grab. */
    uk_fill_circle(&g_win, PROG_X + fill_w, PROG_Y + 4, 5,
                   g_drag_seek ? UK_LAVENDER : UK_TEXT);

    unsigned int elapsed = pos_frames / SAMPLE_RATE;
    unsigned int total_s = total_frames / SAMPLE_RATE;
    char time_str[40];
    snprintf(time_str, sizeof(time_str), "%02u:%02u / %02u:%02u",
             elapsed / 60, elapsed % 60, total_s / 60, total_s % 60);
    uk_draw_text(&g_win, PROG_X, card_y + 104, time_str, UK_OVERLAY0);

    /* ── Controls Bar ─────────────────────────────────────────────────────── */
    int ctrl_y = CTRL_Y;
    uk_draw_button(&g_win, PREV_X, ctrl_y, PREV_W, BTN_H, "|<",
                   btn_state(PREV_X, ctrl_y, PREV_W, BTN_H));
    uk_draw_button(&g_win, PLAY_X, ctrl_y, PLAY_W, BTN_H,
                   g_is_playing ? "|| Pause" : "> Play",
                   btn_state(PLAY_X, ctrl_y, PLAY_W, BTN_H));
    uk_draw_button(&g_win, STOP_X, ctrl_y, STOP_W, BTN_H, "Stop",
                   btn_state(STOP_X, ctrl_y, STOP_W, BTN_H));
    uk_draw_button(&g_win, NEXT_X, ctrl_y, NEXT_W, BTN_H, ">|",
                   btn_state(NEXT_X, ctrl_y, NEXT_W, BTN_H));

    /* Volume slider — click, drag, or scroll over the window. */
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_volume);
    uk_draw_text(&g_win, card_x + 240, ctrl_y + 8, "Vol", UK_SUBTEXT0);
    uk_fill_rounded_rect(&g_win, VOL_X, VOL_Y, VOL_W, 8, 4, UK_SURFACE1);
    uk_fill_rounded_rect(&g_win, VOL_X, VOL_Y, (g_volume * VOL_W) / 100, 8, 4, UK_SAPPHIRE);
    uk_fill_circle(&g_win, VOL_X + (g_volume * VOL_W) / 100, VOL_Y + 4, 5,
                   g_drag_volume ? UK_SKY : UK_TEXT);
    uk_draw_text(&g_win, VOL_X + VOL_W + 8, ctrl_y + 8, vol_str, UK_OVERLAY0);

    /* ── Spectrum Visualizer (Right Panel) ────────────────────────────────── */
    int spec_x = 360, spec_y = 52, spec_w = 204, spec_h = 172;
    uk_fill_rounded_rect(&g_win, spec_x, spec_y, spec_w, spec_h, 8, UK_CRUST);
    uk_draw_rounded_rect_outline(&g_win, spec_x, spec_y, spec_w, spec_h, 8, UK_SURFACE1);
    uk_draw_text(&g_win, spec_x + 12, spec_y + 8, "Audio Spectrum Analyzer", UK_SUBTEXT1);

    int bar_w = 8;
    int bar_gap = 4;
    int total_bars_w = NUM_BANDS * (bar_w + bar_gap) - bar_gap;
    int bar_start_x = spec_x + (spec_w - total_bars_w) / 2;
    int bar_base_y = spec_y + spec_h - 16;

    for (int b = 0; b < NUM_BANDS; b++) {
        int bx = bar_start_x + b * (bar_w + bar_gap);
        int bh = (g_spectrum[b] * 120) / 100;
        if (bh < 2) bh = 2;
        if (bh > 120) bh = 120;

        unsigned int col = (b < 4) ? UK_TEAL : ((b < 9) ? UK_SAPPHIRE : ((b < 13) ? UK_MAUVE : UK_PEACH));
        uk_fill_rounded_rect(&g_win, bx, bar_base_y - bh, bar_w, bh, 2, col);

        /* Peak Hold Line */
        int ph = (g_peaks[b] * 120) / 100;
        if (ph > 0 && bar_base_y - ph - 2 >= spec_y + 24) {
            uk_fill_rect(&g_win, bx, bar_base_y - ph - 2, bar_w, 2, UK_TEXT);
        }
    }

    /* ── Playlist (Bottom 170px) ──────────────────────────────────────────── */
    uk_draw_section_header(&g_win, 16, LIST_Y - 24, (int)w - 32, "Playlist", UK_MAUVE);

    for (int t = 0; t < NUM_TRACKS; t++) {
        int row_y = LIST_Y + t * LIST_ROW;
        bool hovered = (g_mouse_x >= 0 &&
                        hit(g_mouse_x, g_mouse_y, 16, row_y, (int)w - 32, 30));
        bool active = (t == g_current_track);
        unsigned int bg = active ? UK_SURFACE1 : (hovered ? UK_SURFACE1 : UK_SURFACE0);

        uk_fill_rounded_rect(&g_win, 16, row_y, (int)w - 32, 30, 6, bg);
        if (active) {
            uk_fill_rect(&g_win, 16, row_y, 4, 30, UK_MAUVE);
        }

        char num_str[8];
        snprintf(num_str, sizeof(num_str), "%d.", t + 1);
        uk_draw_text(&g_win, 28, row_y + 7, num_str, active ? UK_MAUVE : UK_OVERLAY0);

        uk_draw_text(&g_win, 52, row_y + 7, g_tracks[t].title, active ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, 240, row_y + 7, g_tracks[t].artist, UK_OVERLAY0);

        /* Length computed from tempo and note count rather than the fixed
         * string in the table, which no longer matched anything. */
        unsigned int secs = track_frames(&g_tracks[t]) / SAMPLE_RATE;
        char dur[16];
        snprintf(dur, sizeof(dur), "%u:%02u", secs / 60, secs % 60);
        uk_draw_text(&g_win, (int)w - 70, row_y + 7, dur, UK_SUBTEXT0);
    }

    uk_invalidate(&g_win);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    az_fb_info_t fb;
    unsigned int sw = 1280, sh = 800;
    if (az_fb_info(&fb) == 0 && fb.width > 0 && fb.height > 0) {
        sw = fb.width;
        sh = fb.height;
    }

    int ret = uk_window_connect(&g_win, "Audio Player",
                                (int)(sw / 2) - WIN_W / 2,
                                (int)(sh / 2) - WIN_H / 2,
                                WIN_W, WIN_H, MAP_ADDR, SERVER_CHAN);
    if (ret < 0) return -1;

    g_is_playing = 1; /* Auto-start on launch */
    draw_player();

    unsigned int last_draw = 0;

    for (;;) {
        audio_step();
        g_tick++;

        /*
         * Events are drained without blocking.  A blocking receive here would
         * stall the synthesizer until something happened on the channel — the
         * audio ring would run dry between mouse moves, which is what used to
         * make playback stutter.
         */
        az_wm_msg_t msg;
        int quit = 0;

        while (az_channel_recv_nb(g_win.client_chan, (az_ipc_msg_t *)&msg) == 0) {
            if (msg.type == AZ_WM_DESTROY_WINDOW) { quit = 1; break; }

            if (msg.type == AZ_WM_WINDOW_RESIZED) {
                if (!uk_handle_resize(&g_win, &msg)) { quit = 1; break; }
                g_ui_dirty = 1;
                continue;
            }

            if (msg.type == AZ_WM_KEY_EVENT && msg.key.pressed) {
                if (msg.key.scancode == 57 || msg.key.keycode == ' ') {
                    g_is_playing = !g_is_playing;
                } else if (msg.key.scancode == 77 || msg.key.keycode == 'n') {
                    select_track(g_current_track + 1);
                } else if (msg.key.scancode == 75 || msg.key.keycode == 'p') {
                    select_track(g_current_track - 1);
                } else if (msg.key.scancode == 72) {
                    g_volume = (g_volume <= 95) ? g_volume + 5 : 100;
                } else if (msg.key.scancode == 80) {
                    g_volume = (g_volume >= 5) ? g_volume - 5 : 0;
                }
                g_ui_dirty = 1;

            } else if (msg.type == AZ_WM_MOUSE_EVENT) {
                int mx = msg.mouse.abs_x;
                int my = msg.mouse.abs_y;
                int btn = msg.mouse.buttons;
                int pressed = (btn & 1) && !(g_prev_buttons & 1);   /* edge */
                int released = !(btn & 1) && (g_prev_buttons & 1);

                if (mx != g_mouse_x || my != g_mouse_y) g_ui_dirty = 1;
                g_mouse_x = mx;
                g_mouse_y = my;

                /* Scroll wheel anywhere in the window sets the volume. */
                if (msg.mouse.wheel != 0) {
                    g_volume += (msg.mouse.wheel < 0) ? 5 : -5;
                    if (g_volume < 0) g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    g_ui_dirty = 1;
                }

                if (released) {
                    g_drag_volume = 0;
                    g_drag_seek = 0;
                    g_ui_dirty = 1;
                }

                /* A grabbed slider keeps following the pointer, so it can be
                 * dragged rather than only clicked at one spot. */
                if (g_drag_volume) {
                    g_volume = ((mx - VOL_X) * 100) / VOL_W;
                    if (g_volume < 0) g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    g_ui_dirty = 1;
                } else if (g_drag_seek) {
                    int rel = mx - PROG_X;
                    if (rel < 0) rel = 0;
                    if (rel > PROG_W) rel = PROG_W;
                    seek_to_frame((unsigned int)
                        (((unsigned long long)rel *
                          track_frames(&g_tracks[g_current_track])) / PROG_W));
                }

                if (pressed) {
                    if (hit(mx, my, PREV_X, CTRL_Y, PREV_W, BTN_H)) {
                        select_track(g_current_track - 1);
                    } else if (hit(mx, my, PLAY_X, CTRL_Y, PLAY_W, BTN_H)) {
                        g_is_playing = !g_is_playing;
                    } else if (hit(mx, my, STOP_X, CTRL_Y, STOP_W, BTN_H)) {
                        g_is_playing = 0;
                        seek_to_frame(0);
                    } else if (hit(mx, my, NEXT_X, CTRL_Y, NEXT_W, BTN_H)) {
                        select_track(g_current_track + 1);
                    } else if (hit(mx, my, VOL_X - 6, CTRL_Y, VOL_W + 12, BTN_H)) {
                        g_drag_volume = 1;
                        g_volume = ((mx - VOL_X) * 100) / VOL_W;
                        if (g_volume < 0) g_volume = 0;
                        if (g_volume > 100) g_volume = 100;
                    } else if (hit(mx, my, PROG_X, PROG_Y - 8, PROG_W, 24)) {
                        g_drag_seek = 1;
                        int rel = mx - PROG_X;
                        if (rel < 0) rel = 0;
                        if (rel > PROG_W) rel = PROG_W;
                        seek_to_frame((unsigned int)
                            (((unsigned long long)rel *
                              track_frames(&g_tracks[g_current_track])) / PROG_W));
                    } else {
                        for (int t = 0; t < NUM_TRACKS; t++) {
                            int row_y = LIST_Y + t * LIST_ROW;
                            if (hit(mx, my, 16, row_y, WIN_W - 32, 30)) {
                                select_track(t);
                                g_is_playing = 1;
                                break;
                            }
                        }
                    }
                    g_ui_dirty = 1;
                }

                g_prev_buttons = btn;
            }
        }
        if (quit) break;

        /*
         * Redraw at about 25 Hz, and only when something moved.  Every redraw
         * publishes the whole window to the compositor, so painting on every
         * audio slice would spend more time on pixels than on sound.
         */
        if (g_ui_dirty && (g_tick - last_draw) >= 8) {
            draw_player();
            last_draw = g_tick;
            g_ui_dirty = 0;
        }

        usleep(2000);
    }

    if (g_dsp_fd >= 0) close(g_dsp_fd);
    return 0;
}
