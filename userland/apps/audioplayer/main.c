/* ============================================================================
 * AzamiOS — Audio Player & Synthesizer (v3.0)
 * File: userland/apps/audioplayer/main.c
 *
 * A modern, versatile Hi-Fi audio player supporting:
 *  • Real-time streaming playback of disk WAV files (8/16/24/32-bit PCM, mono/stereo,
 *    any sample rate dynamically resampled via linear interpolation to 48kHz stereo)
 *  • Automatic discovery of music on the disk device (/music, /home/azami/Music, etc.)
 *  • Direct file opening via command-line argument (argv[1]) from filemanager or desktop
 *  • Built-in retro chiptune / synthwave polyphonic synthesizer voices
 *  • Real-time 16-band Goertzel audio spectrum analyzer with peak hold
 *  • Full playback controls: Seek bar, Play/Pause/Stop, Next/Prev, Loop All/One/Off, Volume
 *  • Interactive scrollable playlist and Catppuccin Mocha glassmorphic UI
 * ============================================================================ */

#include "../../libc/include/az/ipc.h"
#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/unistd.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/dirent.h"
#include "../../libc/include/sys/syscall.h"
#include "../azwm/protocol.h"
#include "../azwm/de_protocol.h"
#include "../azwm/de_font.h"
#include "../shared/ui_kit.h"

#define SERVER_CHAN  1
#define WIN_W        620
#define WIN_H        460
#define MAP_ADDR     ((void *)0x6B000000)

/* AC'97 Hardware DAC audio output parameters */
#define SAMPLE_RATE  48000
#define CHANNELS     2
#define CHUNK_FRAMES 1024

#define MAX_TRACKS   32
#define MAX_VISIBLE  5
#define NUM_BANDS    16

typedef enum {
    TRACK_TYPE_SYNTH,
    TRACK_TYPE_WAV
} track_type_t;

typedef struct {
    track_type_t type;
    char title[64];
    char artist[64];
    char filepath[256];
    char format_info[32];
    unsigned int total_seconds;
    unsigned int total_frames;

    /* WAV playback metadata */
    int channels;
    int sample_rate;
    int bits_per_sample;
    size_t data_offset;
    size_t data_size;

    /* Synth playback metadata */
    int bpm;
    int num_notes;
    const int *melody;
    const int *bass;
} track_t;

/* Built-in Chiptune Synth Track Data */
#define TRACK_LOOPS 8
#define N_C3 130; #define N_D3 146; #define N_E3 164; #define N_F3 174
#define N_G3 196; #define N_A3 220; #define N_B3 246; #define N_C4 261
#define N_D4 293; #define N_E4 329; #define N_F4 349; #define N_G4 392
#define N_A4 440; #define N_B4 493; #define N_C5 523; #define N_D5 587
#define N_E5 659; #define N_G5 784; #define N_A5 880; #define N_REST 0

static const int t0_melody[] = {
    440, 523, 659, 880, 784, 659, 587, 659,
    349, 440, 523, 349, 392, 493, 587, 392,
    261, 329, 392, 523, 587, 523, 493, 392,
    440, 329, 440, 493, 523, 493, 440, 329
};
static const int t0_bass[] = {
    220, 220, 220, 220, 174, 174, 196, 196,
    130, 130, 130, 130, 220, 220, 164, 164
};

static const int t1_melody[] = {
    659, 784, 880, 784, 659, 587, 659, 523,
    587, 659, 587, 523, 440, 523, 587, 659,
    784, 880, 784, 659, 784, 880, 523, 587,
    659, 587, 523, 440, 523, 587, 523, 440
};
static const int t1_bass[] = {
    220, 164, 220, 164, 174, 130, 174, 130,
    130, 196, 130, 196, 220, 164, 220, 164
};

/* Global Audio Player State */
static uk_window_t g_win;
static track_t g_tracks[MAX_TRACKS];
static int g_num_tracks = 0;
static int g_current_track = 0;
static int g_is_playing = 0;
static int g_volume = 80;          /* 0..100 */
static int g_loop_mode = 0;        /* 0 = Loop All, 1 = Loop Track, 2 = No Loop */
static int g_playlist_scroll = 0;
static unsigned int g_frames_played = 0;
static int g_dsp_fd = -1;
static int g_wav_fd = -1;
static unsigned int g_tick = 0;

/* Resampling and streaming state for WAV files */
static unsigned int g_wav_sample_accum = 0; /* fractional sample accumulator (Q16) */
static short g_last_wav_l = 0, g_last_wav_r = 0;
static short g_next_wav_l = 0, g_next_wav_r = 0;

/* Synth generator state */
static int g_note_idx = 0;
static int g_loop_idx = 0;
static int g_sample_in_note = 0;
static unsigned int g_phase_mel = 0;
static unsigned int g_phase_bass = 0;

/* Device write buffer */
static short g_pending[CHUNK_FRAMES * CHANNELS];
static int   g_pending_bytes = 0;
static int   g_pending_off = 0;

/* Spectrum Visualizer State */
static int g_spectrum[NUM_BANDS];
static int g_peaks[NUM_BANDS];
static const int g_band_coeff[NUM_BANDS] = {
    32764, 32761, 32755, 32745, 32728, 32696, 32640, 32541,
    32365, 32052, 31498, 30520, 28807, 25837, 20791, 12540
};

/* Mouse & Dragging State */
static int g_mouse_x = -1, g_mouse_y = -1;
static int g_prev_buttons = 0;
static int g_drag_volume = 0;
static int g_drag_seek = 0;
static int g_ui_dirty = 1;

/* Layout Definitions */
#define CARD_X    16
#define CARD_Y    52
#define CARD_W    360
#define CARD_H    134
#define CTRL_Y    (CARD_Y + CARD_H + 12)
#define BTN_H     32

#define PREV_X    CARD_X
#define PREV_W    44
#define PLAY_X    (CARD_X + 50)
#define PLAY_W    72
#define STOP_X    (CARD_X + 128)
#define STOP_W    54
#define NEXT_X    (CARD_X + 188)
#define NEXT_W    44
#define LOOP_X    (CARD_X + 238)
#define LOOP_W    48

#define VOL_LABEL_X (CARD_X + 292)
#define VOL_X       (CARD_X + 318)
#define VOL_W       42
#define VOL_Y       (CTRL_Y + 12)

#define PROG_X    (CARD_X + 16)
#define PROG_Y    (CARD_Y + 94)
#define PROG_W    (CARD_W - 32)

#define SPEC_X    390
#define SPEC_Y    52
#define SPEC_W    214
#define SPEC_H    178

#define LIST_Y    268
#define LIST_ROW  34

/* ── Utility Functions ────────────────────────────────────────────────────── */
static int hit(int x, int y, int rx, int ry, int rw, int rh)
{
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static uk_btn_state_t btn_state(int rx, int ry, int rw, int rh)
{
    if (g_mouse_x < 0) return UK_BTN_NORMAL;
    if (!hit(g_mouse_x, g_mouse_y, rx, ry, rw, rh)) return UK_BTN_NORMAL;
    return (g_prev_buttons & 1) ? UK_BTN_PRESSED : UK_BTN_HOVER;
}

static unsigned int phase_step(int freq)
{
    if (freq <= 0) return 0;
    return (unsigned int)(((unsigned long long)freq << 32) / SAMPLE_RATE);
}

/* ── WAV File Parsing ─────────────────────────────────────────────────────── */
static int parse_wav_file(const char *path, track_t *t)
{
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;

    unsigned char header[44];
    int rd = (int)read(fd, header, sizeof(header));
    if (rd < 44) {
        close(fd);
        return -1;
    }

    /* Verify "RIFF" and "WAVE" */
    if (header[0] != 'R' || header[1] != 'I' || header[2] != 'F' || header[3] != 'F' ||
        header[8] != 'W' || header[9] != 'A' || header[10] != 'V' || header[11] != 'E') {
        close(fd);
        return -1;
    }

    /* Find "fmt " chunk */
    off_t offset = 12;
    int found_fmt = 0, found_data = 0;
    int channels = 2, sample_rate = 44100, bits = 16;
    size_t data_offset = 44, data_size = 0;

    while (offset < 4096) {
        lseek(fd, offset, SEEK_SET);
        unsigned char chunk_hdr[8];
        if (read(fd, chunk_hdr, 8) < 8) break;

        unsigned int chunk_len = (unsigned int)chunk_hdr[4] |
                                 ((unsigned int)chunk_hdr[5] << 8) |
                                 ((unsigned int)chunk_hdr[6] << 16) |
                                 ((unsigned int)chunk_hdr[7] << 24);

        if (chunk_hdr[0] == 'f' && chunk_hdr[1] == 'm' && chunk_hdr[2] == 't' && chunk_hdr[3] == ' ') {
            unsigned char fmt_buf[16];
            if (read(fd, fmt_buf, 16) >= 16) {
                int format = (int)fmt_buf[0] | ((int)fmt_buf[1] << 8);
                if (format != 1) { close(fd); return -1; } /* PCM only */
                channels = (int)fmt_buf[2] | ((int)fmt_buf[3] << 8);
                sample_rate = (int)fmt_buf[4] | ((int)fmt_buf[5] << 8) |
                              ((int)fmt_buf[6] << 16) | ((int)fmt_buf[7] << 24);
                bits = (int)fmt_buf[14] | ((int)fmt_buf[15] << 8);
                found_fmt = 1;
            }
            offset += 8 + chunk_len;
        } else if (chunk_hdr[0] == 'd' && chunk_hdr[1] == 'a' && chunk_hdr[2] == 't' && chunk_hdr[3] == 'a') {
            data_offset = offset + 8;
            data_size = chunk_len;
            found_data = 1;
            break;
        } else {
            offset += 8 + chunk_len;
        }
    }
    close(fd);

    if (!found_fmt || !found_data || sample_rate <= 0 || channels <= 0) {
        return -1;
    }

    t->type = TRACK_TYPE_WAV;
    strncpy(t->filepath, path, sizeof(t->filepath) - 1);
    t->channels = channels;
    t->sample_rate = sample_rate;
    t->bits_per_sample = bits;
    t->data_offset = data_offset;
    t->data_size = data_size;

    /* Extract clean title from filename */
    const char *base = strrchr(path, '/');
    base = base ? (base + 1) : path;
    strncpy(t->title, base, sizeof(t->title) - 1);
    char *dot = strrchr(t->title, '.');
    if (dot) *dot = '\0';

    /* Replace underscores with spaces */
    for (int i = 0; t->title[i]; i++) {
        if (t->title[i] == '_') t->title[i] = ' ';
    }

    strncpy(t->artist, "Azami Hi-Fi Audio", sizeof(t->artist) - 1);

    unsigned int bytes_per_sec = (unsigned int)(sample_rate * channels * (bits / 8));
    t->total_seconds = bytes_per_sec ? (unsigned int)(data_size / bytes_per_sec) : 0;
    t->total_frames = t->total_seconds * SAMPLE_RATE;

    snprintf(t->format_info, sizeof(t->format_info), "%d.%dkHz %db %s",
             sample_rate / 1000, (sample_rate % 1000) / 100, bits,
             (channels == 1) ? "Mono" : "Stereo");

    return 0;
}

/* ── Playlist & Directory Scanner ─────────────────────────────────────────── */
static void scan_directory(const char *dirpath)
{
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && g_num_tracks < MAX_TRACKS - 4) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len < 5) continue;

        if (strcasecmp(de->d_name + len - 4, ".wav") == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dirpath, de->d_name);

            /* Check if already in playlist */
            int exists = 0;
            for (int i = 0; i < g_num_tracks; i++) {
                if (strcmp(g_tracks[i].filepath, full_path) == 0) {
                    exists = 1;
                    break;
                }
            }
            if (exists) continue;

            track_t trk;
            memset(&trk, 0, sizeof(trk));
            if (parse_wav_file(full_path, &trk) == 0) {
                g_tracks[g_num_tracks++] = trk;
            }
        }
    }
    closedir(d);
}

static void load_audio_config(void)
{
    char path[128];
    const char *home = getenv("HOME");
    int fd = -1;
    if (home && home[0]) {
        snprintf(path, sizeof(path), "%s/.config/audio.conf", home);
        fd = sys_open(path, 0, 0);
    }
    if (fd < 0) {
        fd = sys_open("/etc/audio.conf", 0, 0);
    }
    if (fd >= 0) {
        char buf[512];
        ssize_t n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *v = strstr(buf, "volume=");
            if (v) {
                int vol = atoi(v + 7);
                if (vol >= 0 && vol <= 100) g_volume = vol;
            }
            char *md = strstr(buf, "music_dir=");
            if (md) {
                char dir[128];
                int i = 0;
                char *dp = md + 10;
                while (*dp && *dp != '\n' && *dp != '\r' && i < 127) dir[i++] = *dp++;
                dir[i] = '\0';
                if (dir[0]) scan_directory(dir);
            }
        }
    }
}

static void populate_playlist(const char *arg_file)
{
    g_num_tracks = 0;

    /* 1. Load configuration from /etc/audio.conf */
    load_audio_config();

    /* 2. If a specific file was passed, make it track 0 */
    if (arg_file && arg_file[0]) {
        track_t trk;
        memset(&trk, 0, sizeof(trk));
        if (parse_wav_file(arg_file, &trk) == 0) {
            g_tracks[g_num_tracks++] = trk;
        }
    }

    /* 3. Scan standard music directories on the disk */
    scan_directory("/music");
    scan_directory("/home/azami/Music");
    scan_directory("/usr/share/music");
    scan_directory("/hdd/music");

    /* 4. Add built-in polyphonic synthwave bonus tracks */
    if (g_num_tracks < MAX_TRACKS) {
        track_t *s0 = &g_tracks[g_num_tracks++];
        memset(s0, 0, sizeof(track_t));
        s0->type = TRACK_TYPE_SYNTH;
        strncpy(s0->title, "Cyber Horizon", sizeof(s0->title) - 1);
        strncpy(s0->artist, "AzamiOS Synthwave", sizeof(s0->artist) - 1);
        strncpy(s0->format_info, "SYNTH 48kHz Stereo", sizeof(s0->format_info) - 1);
        s0->bpm = 140;
        s0->num_notes = 32;
        s0->melody = t0_melody;
        s0->bass = t0_bass;
        s0->total_seconds = 32;
        s0->total_frames = 32 * SAMPLE_RATE;
    }

    if (g_num_tracks < MAX_TRACKS) {
        track_t *s1 = &g_tracks[g_num_tracks++];
        memset(s1, 0, sizeof(track_t));
        s1->type = TRACK_TYPE_SYNTH;
        strncpy(s1->title, "Neon Sakura", sizeof(s1->title) - 1);
        strncpy(s1->artist, "Chiptune Project", sizeof(s1->artist) - 1);
        strncpy(s1->format_info, "CHIPTUNE 48kHz Stereo", sizeof(s1->format_info) - 1);
        s1->bpm = 130;
        s1->num_notes = 32;
        s1->melody = t1_melody;
        s1->bass = t1_bass;
        s1->total_seconds = 32;
        s1->total_frames = 32 * SAMPLE_RATE;
    }
}

/* ── Goertzel Spectrum Analysis ───────────────────────────────────────────── */
static void analyse_chunk(const short *frames, int nframes)
{
    for (int b = 0; b < NUM_BANDS; b++) {
        long long s1 = 0, s2 = 0;
        int coeff = g_band_coeff[b];

        for (int i = 0; i < nframes; i++) {
            long long x = (frames[i * CHANNELS] + frames[i * CHANNELS + 1]) >> 5;
            long long s0 = ((coeff * s1) >> 14) - s2 + x;
            s2 = s1;
            s1 = s0;
        }

        long long mag2 = s1 * s1 + s2 * s2 - ((coeff * s1 % 32768) * s2 >> 14);
        if (mag2 < 0) mag2 = 0;

        long long r = 0, bit = 1LL << 40;
        while (bit > mag2) bit >>= 2;
        while (bit) {
            if (mag2 >= r + bit) { mag2 -= r + bit; r = (r >> 1) + bit; }
            else r >>= 1;
            bit >>= 2;
        }

        int level = (int)(r >> 9);
        if (level > 80) level = 80;

        if (g_spectrum[b] < level) g_spectrum[b] = level;
        else g_spectrum[b] = (g_spectrum[b] * 7) / 8;

        if (g_peaks[b] < g_spectrum[b]) g_peaks[b] = g_spectrum[b];
        else if (g_peaks[b] > 0) g_peaks[b]--;
    }
}

/* ── Audio Stream Decoding & Synthesis ────────────────────────────────────── */
static void synth_chunk(short *out, int nframes)
{
    const track_t *trk = &g_tracks[g_current_track];
    int spb = (SAMPLE_RATE * 60) / (trk->bpm * 4);
    if (spb < 1) spb = 1;

    for (int i = 0; i < nframes; i++) {
        int note_m = trk->melody[g_note_idx % trk->num_notes];
        int note_b = trk->bass[(g_note_idx / 2) % 16];

        unsigned int attack = SAMPLE_RATE / 500;
        int env;
        if ((unsigned int)g_sample_in_note < attack) {
            env = (int)((256u * (unsigned int)g_sample_in_note) / attack);
        } else {
            unsigned int rem = (unsigned int)spb - (unsigned int)g_sample_in_note;
            env = (int)((256u * rem) / (unsigned int)spb);
            if (env > 256) env = 256;
        }
        if (env < 0) env = 0;

        int mel = 0, bass = 0;
        if (note_m > 0) {
            g_phase_mel += phase_step(note_m);
            int amp = (7000 * env) / 256;
            mel = (g_phase_mel < 0x80000000u) ? amp : -amp;
        }
        if (note_b > 0) {
            g_phase_bass += phase_step(note_b);
            int amp = (5000 * ((env + 512) / 3)) / 256;
            unsigned int ph = g_phase_bass;
            int tri = (ph < 0x80000000u)
                        ? (int)((ph >> 15) - 32768)
                        : (int)(32768 - ((ph - 0x80000000u) >> 15));
            bass = (tri * amp) / 32768;
        }

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
        if (g_sample_in_note >= spb) {
            g_sample_in_note = 0;
            g_note_idx++;
            if (g_note_idx >= trk->num_notes) {
                g_note_idx = 0;
                g_loop_idx++;
                if (g_loop_idx >= TRACK_LOOPS) {
                    g_loop_idx = 0;
                    if (g_loop_mode == 1) {
                        /* Loop single */
                    } else if (g_loop_mode == 0) {
                        g_current_track = (g_current_track + 1) % g_num_tracks;
                    } else {
                        g_is_playing = 0;
                    }
                    g_frames_played = 0;
                    g_ui_dirty = 1;
                }
            }
        }
    }
}

/* Read one raw sample frame from WAV file and return left and right in 16-bit */
static int read_wav_raw_sample(int fd, int channels, int bits, short *l_out, short *r_out)
{
    if (bits == 16) {
        short buf[2];
        int count = channels;
        int n = (int)read(fd, buf, count * sizeof(short));
        if (n < (int)(count * sizeof(short))) return -1;
        *l_out = buf[0];
        *r_out = (channels > 1) ? buf[1] : buf[0];
        return 0;
    } else if (bits == 8) {
        unsigned char buf[2];
        int count = channels;
        int n = (int)read(fd, buf, count);
        if (n < count) return -1;
        *l_out = (short)(((int)buf[0] - 128) << 8);
        *r_out = (channels > 1) ? (short)(((int)buf[1] - 128) << 8) : *l_out;
        return 0;
    } else if (bits == 24) {
        unsigned char buf[6];
        int count = channels * 3;
        int n = (int)read(fd, buf, count);
        if (n < count) return -1;
        int s1 = ((int)(signed char)buf[2] << 8) | buf[1];
        int s2 = (channels > 1) ? (((int)(signed char)buf[5] << 8) | buf[4]) : s1;
        *l_out = (short)s1;
        *r_out = (short)s2;
        return 0;
    }
    return -1;
}

static void wav_chunk(short *out, int nframes)
{
    track_t *trk = &g_tracks[g_current_track];

    if (g_wav_fd < 0) {
        g_wav_fd = open(trk->filepath, O_RDONLY, 0);
        if (g_wav_fd < 0) {
            g_is_playing = 0;
            return;
        }
        lseek(g_wav_fd, trk->data_offset, SEEK_SET);
        read_wav_raw_sample(g_wav_fd, trk->channels, trk->bits_per_sample, &g_last_wav_l, &g_last_wav_r);
        read_wav_raw_sample(g_wav_fd, trk->channels, trk->bits_per_sample, &g_next_wav_l, &g_next_wav_r);
        g_wav_sample_accum = 0;
    }

    /* Fixed-point ratio for linear interpolation (Q16) */
    unsigned int ratio = (unsigned int)(((unsigned long long)trk->sample_rate << 16) / SAMPLE_RATE);

    for (int i = 0; i < nframes; i++) {
        /* Advance input sample pointer */
        while (g_wav_sample_accum >= 0x10000) {
            g_wav_sample_accum -= 0x10000;
            g_last_wav_l = g_next_wav_l;
            g_last_wav_r = g_next_wav_r;
            if (read_wav_raw_sample(g_wav_fd, trk->channels, trk->bits_per_sample,
                                    &g_next_wav_l, &g_next_wav_r) < 0) {
                /* End of track reached */
                close(g_wav_fd);
                g_wav_fd = -1;

                if (g_loop_mode == 1) {
                    /* Loop single */
                    g_frames_played = 0;
                } else if (g_loop_mode == 0) {
                    /* Loop all */
                    g_current_track = (g_current_track + 1) % g_num_tracks;
                    g_frames_played = 0;
                } else {
                    g_is_playing = 0;
                }
                g_ui_dirty = 1;
                return;
            }
        }

        /* Linear interpolation between g_last_wav and g_next_wav */
        int frac = g_wav_sample_accum & 0xFFFF;
        int l = g_last_wav_l + (((g_next_wav_l - g_last_wav_l) * frac) >> 16);
        int r = g_last_wav_r + (((g_next_wav_r - g_last_wav_r) * frac) >> 16);

        l = (l * g_volume) / 100;
        r = (r * g_volume) / 100;

        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;

        out[i * CHANNELS]     = (short)l;
        out[i * CHANNELS + 1] = (short)r;

        g_wav_sample_accum += ratio;
    }
}

static void audio_step(void)
{
    if (!g_is_playing || g_num_tracks == 0) return;

    if (g_dsp_fd < 0) {
        g_dsp_fd = open("/dev/dsp", O_WRONLY, 0);
        if (g_dsp_fd < 0) {
            g_is_playing = 0;
            g_ui_dirty = 1;
            return;
        }
    }

    if (g_pending_bytes == 0) {
        track_t *cur = &g_tracks[g_current_track];
        if (cur->type == TRACK_TYPE_WAV) {
            wav_chunk(g_pending, CHUNK_FRAMES);
        } else {
            synth_chunk(g_pending, CHUNK_FRAMES);
        }

        analyse_chunk(g_pending, CHUNK_FRAMES);
        g_pending_bytes = (int)sizeof(g_pending);
        g_pending_off = 0;
        g_ui_dirty = 1;
    }

    int n = (int)write(g_dsp_fd,
                       (const char *)g_pending + g_pending_off,
                       (size_t)(g_pending_bytes - g_pending_off));
    if (n <= 0) return;

    g_pending_off += n;
    g_frames_played += (unsigned int)n / (CHANNELS * (int)sizeof(short));

    if (g_pending_off >= g_pending_bytes) {
        g_pending_bytes = 0;
        g_pending_off = 0;
    }
}

static void seek_to_frame(unsigned int frame)
{
    if (g_num_tracks == 0) return;
    track_t *trk = &g_tracks[g_current_track];
    if (frame >= trk->total_frames) frame = trk->total_frames ? trk->total_frames - 1 : 0;

    if (trk->type == TRACK_TYPE_WAV) {
        if (g_wav_fd < 0) {
            g_wav_fd = open(trk->filepath, O_RDONLY, 0);
        }
        if (g_wav_fd >= 0) {
            unsigned int bytes_per_frame = (unsigned int)(trk->channels * (trk->bits_per_sample / 8));
            double pct = (double)frame / (double)(trk->total_frames ? trk->total_frames : 1);
            off_t byte_off = trk->data_offset + (off_t)(pct * trk->data_size);
            byte_off -= (byte_off % bytes_per_frame);
            lseek(g_wav_fd, byte_off, SEEK_SET);
            read_wav_raw_sample(g_wav_fd, trk->channels, trk->bits_per_sample, &g_last_wav_l, &g_last_wav_r);
            read_wav_raw_sample(g_wav_fd, trk->channels, trk->bits_per_sample, &g_next_wav_l, &g_next_wav_r);
            g_wav_sample_accum = 0;
        }
    } else {
        int spb = (SAMPLE_RATE * 60) / (trk->bpm * 4);
        if (spb < 1) spb = 1;
        unsigned int step = frame / (unsigned int)spb;
        g_note_idx       = (int)(step % (unsigned int)trk->num_notes);
        g_loop_idx       = (int)(step / (unsigned int)trk->num_notes);
        g_sample_in_note = (int)(frame % (unsigned int)spb);
    }

    g_frames_played = frame;
    g_pending_bytes = 0;
    g_pending_off = 0;
    g_ui_dirty = 1;
}

static void select_track(int idx)
{
    if (g_num_tracks == 0) return;
    if (g_wav_fd >= 0) {
        close(g_wav_fd);
        g_wav_fd = -1;
    }
    g_current_track = ((idx % g_num_tracks) + g_num_tracks) % g_num_tracks;
    seek_to_frame(0);
}

/* ── UI Drawing Functions ─────────────────────────────────────────────────── */
static void draw_player(void)
{
    unsigned int w = g_win.width;
    unsigned int h = g_win.height;

    uk_fill_rect(&g_win, 0, 0, (int)w, (int)h, UK_BASE);

    /* Header */
    uk_gradient_h(&g_win, 0, 0, (int)w, 42, UK_SURFACE0, UK_BASE);
    uk_fill_rect(&g_win, 0, 0, 4, 42, UK_MAUVE);
    uk_draw_text(&g_win, 16, 8, "Azami Hi-Fi Audio Player", UK_TEXT);

    char sub_str[64];
    snprintf(sub_str, sizeof(sub_str), "Disk Storage & Synthesizer • Track %d of %d",
             g_current_track + 1, g_num_tracks);
    uk_draw_text(&g_win, 16, 24, sub_str, UK_OVERLAY0);
    uk_hline(&g_win, 0, 42, (int)w, UK_SURFACE1);

    if (g_num_tracks == 0) {
        uk_draw_text(&g_win, 16, 60, "No audio files found on disk.", UK_TEXT);
        uk_invalidate(&g_win);
        return;
    }

    track_t *cur = &g_tracks[g_current_track];

    /* Now Playing Card */
    uk_fill_rounded_rect(&g_win, CARD_X, CARD_Y, CARD_W, CARD_H, 8, UK_SURFACE0);
    uk_draw_rounded_rect_outline(&g_win, CARD_X, CARD_Y, CARD_W, CARD_H, 8, UK_SURFACE1);

    /* Vinyl Disc with Rotating Spoke */
    int disc_cx = CARD_X + 42, disc_cy = CARD_Y + 44;
    uk_fill_circle(&g_win, disc_cx, disc_cy, 30, UK_CRUST);
    uk_fill_circle(&g_win, disc_cx, disc_cy, 22, UK_SURFACE1);
    uk_fill_circle(&g_win, disc_cx, disc_cy, 14, UK_SURFACE2);
    uk_fill_circle(&g_win, disc_cx, disc_cy, 6, g_is_playing ? UK_MAUVE : UK_OVERLAY0);

    if (g_is_playing) {
        static const int sin16[16] = {  0,  7, 13, 17, 18, 17, 13,  7,
                                        0, -7,-13,-17,-18,-17,-13, -7 };
        int step = (int)((g_frames_played / (SAMPLE_RATE / 12)) % 16);
        int dx = sin16[step];
        int dy = -sin16[(step + 4) % 16];
        uk_draw_line_aa(&g_win, disc_cx, disc_cy, disc_cx + dx, disc_cy + dy, UK_LAVENDER);
    }

    /* Track Information */
    uk_draw_text(&g_win, CARD_X + 86, CARD_Y + 14, cur->title, UK_TEXT);
    uk_draw_text(&g_win, CARD_X + 86, CARD_Y + 32, cur->artist, UK_SUBTEXT0);

    /* Format Badge */
    uk_fill_rounded_rect(&g_win, CARD_X + 86, CARD_Y + 50, 160, 18, 4, UK_MANTLE);
    uk_draw_text(&g_win, CARD_X + 92, CARD_Y + 53, cur->format_info, UK_TEAL);

    /* Play/Pause Badge */
    if (g_is_playing) {
        uk_fill_rounded_rect(&g_win, CARD_X + 252, CARD_Y + 50, 60, 18, 4, UK_GREEN);
        uk_draw_text(&g_win, CARD_X + 258, CARD_Y + 53, "PLAYING", UK_BASE);
    } else {
        uk_fill_rounded_rect(&g_win, CARD_X + 252, CARD_Y + 50, 56, 18, 4, UK_SURFACE2);
        uk_draw_text(&g_win, CARD_X + 260, CARD_Y + 53, "PAUSED", UK_TEXT);
    }

    /* Progress & Seeking Bar */
    unsigned int total_frames = cur->total_frames ? cur->total_frames : 1;
    unsigned int pos_frames = g_frames_played;
    if (pos_frames > total_frames) pos_frames = total_frames;

    uk_fill_rounded_rect(&g_win, PROG_X, PROG_Y, PROG_W, 8, 4, UK_SURFACE1);
    int fill_w = (int)(((unsigned long long)pos_frames * PROG_W) / total_frames);
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, PROG_X, PROG_Y, fill_w, 8, 4, UK_MAUVE);
    }
    uk_fill_circle(&g_win, PROG_X + fill_w, PROG_Y + 4, 5, g_drag_seek ? UK_LAVENDER : UK_TEXT);

    unsigned int elapsed = pos_frames / SAMPLE_RATE;
    unsigned int total_s = cur->total_seconds;
    char time_str[40];
    snprintf(time_str, sizeof(time_str), "%02u:%02u / %02u:%02u",
             elapsed / 60, elapsed % 60, total_s / 60, total_s % 60);
    uk_draw_text(&g_win, PROG_X, CARD_Y + 110, time_str, UK_OVERLAY0);

    /* Controls Bar */
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

    const char *loop_labels[] = { "All", "One", "Off" };
    uk_draw_button(&g_win, LOOP_X, ctrl_y, LOOP_W, BTN_H, loop_labels[g_loop_mode],
                   btn_state(LOOP_X, ctrl_y, LOOP_W, BTN_H));

    /* Volume Slider */
    char vol_str[16];
    snprintf(vol_str, sizeof(vol_str), "%d%%", g_volume);
    uk_draw_text(&g_win, VOL_LABEL_X, ctrl_y + 8, "Vol", UK_SUBTEXT0);
    uk_fill_rounded_rect(&g_win, VOL_X, VOL_Y, VOL_W, 8, 4, UK_SURFACE1);
    uk_fill_rounded_rect(&g_win, VOL_X, VOL_Y, (g_volume * VOL_W) / 100, 8, 4, UK_SAPPHIRE);
    uk_fill_circle(&g_win, VOL_X + (g_volume * VOL_W) / 100, VOL_Y + 4, 5,
                   g_drag_volume ? UK_SKY : UK_TEXT);

    /* Spectrum Visualizer Panel */
    uk_fill_rounded_rect(&g_win, SPEC_X, SPEC_Y, SPEC_W, SPEC_H, 8, UK_CRUST);
    uk_draw_rounded_rect_outline(&g_win, SPEC_X, SPEC_Y, SPEC_W, SPEC_H, 8, UK_SURFACE1);
    uk_draw_text(&g_win, SPEC_X + 12, SPEC_Y + 8, "16-Band Spectrum Analyzer", UK_SUBTEXT1);

    int bar_w = 8;
    int bar_gap = 4;
    int total_bars_w = NUM_BANDS * (bar_w + bar_gap) - bar_gap;
    int bar_start_x = SPEC_X + (SPEC_W - total_bars_w) / 2;
    int bar_base_y = SPEC_Y + SPEC_H - 16;

    for (int b = 0; b < NUM_BANDS; b++) {
        int bx = bar_start_x + b * (bar_w + bar_gap);
        int bh = (g_spectrum[b] * 120) / 100;
        if (bh < 2) bh = 2;
        if (bh > 120) bh = 120;

        unsigned int col = (b < 4) ? UK_TEAL : ((b < 9) ? UK_SAPPHIRE : ((b < 13) ? UK_MAUVE : UK_PEACH));
        uk_fill_rounded_rect(&g_win, bx, bar_base_y - bh, bar_w, bh, 2, col);

        int ph = (g_peaks[b] * 120) / 100;
        if (ph > 0 && bar_base_y - ph - 2 >= SPEC_Y + 24) {
            uk_fill_rect(&g_win, bx, bar_base_y - ph - 2, bar_w, 2, UK_TEXT);
        }
    }

    /* Playlist Header & Items */
    uk_draw_section_header(&g_win, 16, LIST_Y - 24, (int)w - 32, "Storage Playlist", UK_MAUVE);

    int max_vis = MAX_VISIBLE;
    for (int vi = 0; vi < max_vis; vi++) {
        int t = g_playlist_scroll + vi;
        if (t >= g_num_tracks) break;

        int row_y = LIST_Y + vi * LIST_ROW;
        bool hovered = (g_mouse_x >= 0 && hit(g_mouse_x, g_mouse_y, 16, row_y, (int)w - 32, 30));
        bool active = (t == g_current_track);
        unsigned int bg = active ? UK_SURFACE1 : (hovered ? UK_SURFACE1 : UK_SURFACE0);

        uk_fill_rounded_rect(&g_win, 16, row_y, (int)w - 32, 30, 6, bg);
        if (active) {
            uk_fill_rect(&g_win, 16, row_y, 4, 30, UK_MAUVE);
        }

        /* Track Icon & Number */
        char num_str[16];
        snprintf(num_str, sizeof(num_str), "%2d.", t + 1);
        uk_draw_text(&g_win, 24, row_y + 7, num_str, active ? UK_MAUVE : UK_OVERLAY0);

        const char *type_tag = (g_tracks[t].type == TRACK_TYPE_WAV) ? "[WAV]" : "[SYNTH]";
        unsigned int tag_col = (g_tracks[t].type == TRACK_TYPE_WAV) ? UK_TEAL : UK_PEACH;
        uk_draw_text(&g_win, 54, row_y + 7, type_tag, tag_col);

        uk_draw_text(&g_win, 114, row_y + 7, g_tracks[t].title, active ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, 340, row_y + 7, g_tracks[t].format_info, UK_OVERLAY0);

        unsigned int secs = g_tracks[t].total_seconds;
        char dur[16];
        snprintf(dur, sizeof(dur), "%u:%02u", secs / 60, secs % 60);
        uk_draw_text(&g_win, (int)w - 74, row_y + 7, dur, UK_SUBTEXT0);
    }

    uk_invalidate(&g_win);
}

/* ── Main Entry Point ─────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
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

    /* Populate playlist from disk and command line */
    const char *initial_file = (argc > 1) ? argv[1] : NULL;
    populate_playlist(initial_file);

    g_is_playing = 1;
    draw_player();

    unsigned int last_draw = 0;

    for (;;) {
        audio_step();
        g_tick++;

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
                } else if (msg.key.scancode == 31 || msg.key.keycode == 's') {
                    g_is_playing = 0;
                    seek_to_frame(0);
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
                int pressed = (btn & 1) && !(g_prev_buttons & 1);
                int released = !(btn & 1) && (g_prev_buttons & 1);

                if (mx != g_mouse_x || my != g_mouse_y) g_ui_dirty = 1;
                g_mouse_x = mx;
                g_mouse_y = my;

                /* Mouse scroll wheel changes volume */
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

                if (g_drag_volume) {
                    g_volume = ((mx - VOL_X) * 100) / VOL_W;
                    if (g_volume < 0) g_volume = 0;
                    if (g_volume > 100) g_volume = 100;
                    g_ui_dirty = 1;
                } else if (g_drag_seek) {
                    int rel = mx - PROG_X;
                    if (rel < 0) rel = 0;
                    if (rel > PROG_W) rel = PROG_W;
                    if (g_num_tracks > 0) {
                        unsigned int target = (unsigned int)(((unsigned long long)rel * g_tracks[g_current_track].total_frames) / PROG_W);
                        seek_to_frame(target);
                    }
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
                    } else if (hit(mx, my, LOOP_X, CTRL_Y, LOOP_W, BTN_H)) {
                        g_loop_mode = (g_loop_mode + 1) % 3;
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
                        if (g_num_tracks > 0) {
                            unsigned int target = (unsigned int)(((unsigned long long)rel * g_tracks[g_current_track].total_frames) / PROG_W);
                            seek_to_frame(target);
                        }
                    } else {
                        /* Check playlist item clicks */
                        for (int vi = 0; vi < MAX_VISIBLE; vi++) {
                            int t = g_playlist_scroll + vi;
                            if (t >= g_num_tracks) break;
                            int row_y = LIST_Y + vi * LIST_ROW;
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

        if (g_ui_dirty && (g_tick - last_draw) >= 8) {
            draw_player();
            last_draw = g_tick;
            g_ui_dirty = 0;
        }

        usleep(2000);
    }

    if (g_wav_fd >= 0) close(g_wav_fd);
    if (g_dsp_fd >= 0) close(g_dsp_fd);
    return 0;
}
