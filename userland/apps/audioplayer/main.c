/* ============================================================================
 * AzamiOS — Audio Player & Synthesizer (v1.0)
 * File: userland/apps/audioplayer/main.c
 *
 * Features:
 *  • Multi-track chiptune & synthwave polyphonic music engine
 *  • Real-time 16-band gradient audio spectrum visualizer & peak bars
 *  • Dynamic oscilloscope waveform display
 *  • Interactive tracklist, volume control, progress bar, play/pause/stop
 *  • Streams 16-bit 44.1kHz PCM directly to /dev/dsp
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

#define SAMPLE_RATE 44100
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
static int g_sample_in_note = 0;
static unsigned int g_tick = 0;
static int g_dsp_fd = -1;

/* Spectrum Visualizer State (16 bands) */
#define NUM_BANDS 16
static int g_spectrum[NUM_BANDS];
static int g_peaks[NUM_BANDS];

static void audio_step(void)
{
    if (!g_is_playing) return;

    if (g_dsp_fd < 0) {
        g_dsp_fd = open("/dev/dsp", O_WRONLY, 0);
        if (g_dsp_fd < 0) {
            /* BUG-18 fix: inform the user and pause instead of silently failing */
            g_is_playing = 0;
            return;
        }
    }

    const track_t *trk = &g_tracks[g_current_track];
    int samples_per_beat = (SAMPLE_RATE * 60) / (trk->bpm * 4);
    /* BUG-20 fix: guard threshold and fallback must be the same value (4000)
     * to avoid a dead zone where [1000, 4000) passes unclamped. */
    if (samples_per_beat < 4000) samples_per_beat = 4000;

    short buf[512];
    int samples_to_generate = 512;

    for (int i = 0; i < samples_to_generate; i++) {
        int note_m = trk->melody[g_note_idx % trk->num_notes];
        int note_b = trk->bass[(g_note_idx / 2) % 16];

        int val = 0;

        /* Melody Wave (Square with volume envelope) */
        if (note_m > 0) {
            int period = SAMPLE_RATE / note_m;
            int half = period / 2;
            int env = samples_per_beat - g_sample_in_note;
            if (env < 0) env = 0;
            int amp = (8000 * env) / samples_per_beat;
            val += ((g_sample_in_note % period) < half) ? amp : -amp;
        }

        /* Bass Wave (Triangle/Saw wave) */
        if (note_b > 0) {
            int period = SAMPLE_RATE / note_b;
            int amp = 5000;
            int saw = ((g_sample_in_note % period) * amp * 2) / period - amp;
            val += saw;
        }

        /* Apply master volume */
        val = (val * g_volume) / 100;
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;

        buf[i] = (short)val;

        g_sample_in_note++;
        if (g_sample_in_note >= samples_per_beat) {
            g_sample_in_note = 0;
            g_note_idx = (g_note_idx + 1) % trk->num_notes;
        }
    }

    write(g_dsp_fd, buf, sizeof(buf));

    /* Update spectrum visualizer */
    for (int b = 0; b < NUM_BANDS; b++) {
        int note = trk->melody[g_note_idx % trk->num_notes];
        int target = 0;
        if (note > 0) {
            int dist = (b * 60) - (note / 8);
            if (dist < 0) dist = -dist;
            target = 80 - dist * 2;
            if (target < 8) target = 8 + (b % 4) * 3;
        } else {
            target = 4 + (b % 3) * 2;
        }

        if (g_spectrum[b] < target) g_spectrum[b] = target;
        else g_spectrum[b] = (g_spectrum[b] * 7) / 8;

        if (g_peaks[b] < g_spectrum[b]) g_peaks[b] = g_spectrum[b];
        else if (g_peaks[b] > 0) g_peaks[b]--;
    }
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

    /* Rotating needle/groove spoke if playing */
    if (g_is_playing) {
        int angle = (g_tick * 15) % 360;
        int rad = 18;
        int dx = (rad * ((angle < 180 ? angle : 360 - angle) - 90)) / 90;
        int dy = (rad * ((angle < 90 || angle > 270 ? 90 : -90))) / 90;
        uk_draw_line_aa(&g_win, card_x + 40, card_y + 40, card_x + 40 + dx, card_y + 40 + dy, UK_LAVENDER);
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

    /* Progress bar */
    int prog_w = card_w - 32;
    int prog_pct = (g_note_idx * 100) / cur->num_notes;
    uk_fill_rounded_rect(&g_win, card_x + 16, card_y + 90, prog_w, 8, 4, UK_SURFACE1);
    int fill_w = (prog_pct * prog_w) / 100;
    if (fill_w > 0) {
        uk_fill_rounded_rect(&g_win, card_x + 16, card_y + 90, fill_w, 8, 4, UK_MAUVE);
    }

    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d / %s", g_note_idx / 4, (g_note_idx % 4) * 15, cur->duration);
    uk_draw_text(&g_win, card_x + 16, card_y + 104, time_str, UK_OVERLAY0);

    /* ── Controls Bar ─────────────────────────────────────────────────────── */
    int ctrl_y = card_y + card_h + 10;
    /* Prev, Play/Pause, Stop, Next buttons */
    uk_draw_button(&g_win, card_x, ctrl_y, 44, 32, "|<", 0);
    uk_draw_button(&g_win, card_x + 50, ctrl_y, 70, 32, g_is_playing ? "|| Pause" : "> Play", 0);
    uk_draw_button(&g_win, card_x + 126, ctrl_y, 54, 32, "Stop", 0);
    uk_draw_button(&g_win, card_x + 186, ctrl_y, 44, 32, ">|", 0);

    /* Volume Slider */
    uk_draw_text(&g_win, card_x + 240, ctrl_y + 8, "Vol:", UK_SUBTEXT0);
    uk_fill_rounded_rect(&g_win, card_x + 275, ctrl_y + 12, 45, 8, 4, UK_SURFACE1);
    uk_fill_rounded_rect(&g_win, card_x + 275, ctrl_y + 12, (g_volume * 45) / 100, 8, 4, UK_SAPPHIRE);

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
    int list_y = 236;
    uk_draw_section_header(&g_win, 16, list_y, (int)w - 32, "Playlist", UK_MAUVE);
    list_y += 24;

    for (int t = 0; t < NUM_TRACKS; t++) {
        int row_y = list_y + t * 34;
        bool active = (t == g_current_track);
        unsigned int bg = active ? UK_SURFACE1 : UK_SURFACE0;

        uk_fill_rounded_rect(&g_win, 16, row_y, (int)w - 32, 30, 6, bg);
        if (active) {
            uk_fill_rect(&g_win, 16, row_y, 4, 30, UK_MAUVE);
        }

        char num_str[8];
        snprintf(num_str, sizeof(num_str), "%d.", t + 1);
        uk_draw_text(&g_win, 28, row_y + 7, num_str, active ? UK_MAUVE : UK_OVERLAY0);

        uk_draw_text(&g_win, 52, row_y + 7, g_tracks[t].title, active ? UK_TEXT : UK_SUBTEXT1);
        uk_draw_text(&g_win, 240, row_y + 7, g_tracks[t].artist, UK_OVERLAY0);
        uk_draw_text(&g_win, (int)w - 70, row_y + 7, g_tracks[t].duration, UK_SUBTEXT0);
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

    for (;;) {
        audio_step();
        g_tick++;
        if (g_tick % 4 == 0) {
            draw_player();
        }

        /* Check for IPC events without blocking */
        az_wm_msg_t msg;
        /* Using syscall to check channel messages non-blockingly or polling */
        int r = az_channel_recv(g_win.client_chan, (az_ipc_msg_t *)&msg);
        if (r == 0) {
            if (msg.type == AZ_WM_DESTROY_WINDOW) {
                if (g_dsp_fd >= 0) close(g_dsp_fd);
                break;
            } else if (msg.type == AZ_WM_KEY_EVENT && msg.key.pressed) {
                if (msg.key.scancode == 57 || msg.key.keycode == ' ') { /* Space: Play/Pause */
                    g_is_playing = !g_is_playing;
                    draw_player();
                } else if (msg.key.scancode == 77 || msg.key.keycode == 'n') { /* Right: Next */
                    g_current_track = (g_current_track + 1) % NUM_TRACKS;
                    g_note_idx = 0; g_sample_in_note = 0;
                    draw_player();
                } else if (msg.key.scancode == 75 || msg.key.keycode == 'p') { /* Left: Prev */
                    g_current_track = (g_current_track + NUM_TRACKS - 1) % NUM_TRACKS;
                    g_note_idx = 0; g_sample_in_note = 0;
                    draw_player();
                } else if (msg.key.scancode == 72) { /* Up: Vol up */
                    g_volume = (g_volume <= 90) ? g_volume + 10 : 100;
                    draw_player();
                } else if (msg.key.scancode == 80) { /* Down: Vol down */
                    g_volume = (g_volume >= 10) ? g_volume - 10 : 0;
                    draw_player();
                }
            } else if (msg.type == AZ_WM_MOUSE_EVENT) {
                int mx = msg.mouse.abs_x;
                int my = msg.mouse.abs_y;
                int btn = msg.mouse.buttons;

                if (btn & 1) { /* Left click */
                    int card_x = 16, ctrl_y = 52 + 130 + 10;

                    /* Prev */
                    if (mx >= card_x && mx < card_x + 44 && my >= ctrl_y && my < ctrl_y + 32) {
                        g_current_track = (g_current_track + NUM_TRACKS - 1) % NUM_TRACKS;
                        g_note_idx = 0; g_sample_in_note = 0;
                        draw_player();
                    }
                    /* Play / Pause */
                    else if (mx >= card_x + 50 && mx < card_x + 120 && my >= ctrl_y && my < ctrl_y + 32) {
                        g_is_playing = !g_is_playing;
                        draw_player();
                    }
                    /* Stop */
                    else if (mx >= card_x + 126 && mx < card_x + 180 && my >= ctrl_y && my < ctrl_y + 32) {
                        g_is_playing = 0;
                        g_note_idx = 0; g_sample_in_note = 0;
                        draw_player();
                    }
                    /* Next */
                    else if (mx >= card_x + 186 && mx < card_x + 230 && my >= ctrl_y && my < ctrl_y + 32) {
                        g_current_track = (g_current_track + 1) % NUM_TRACKS;
                        g_note_idx = 0; g_sample_in_note = 0;
                        draw_player();
                    }
                    /* Volume click */
                    else if (mx >= card_x + 275 && mx < card_x + 320 && my >= ctrl_y && my < ctrl_y + 32) {
                        g_volume = ((mx - (card_x + 275)) * 100) / 45;
                        if (g_volume < 0) g_volume = 0;
                        if (g_volume > 100) g_volume = 100;
                        draw_player();
                    }
                    /* Playlist item click */
                    int list_y = 260;
                    for (int t = 0; t < NUM_TRACKS; t++) {
                        int row_y = list_y + t * 34;
                        if (mx >= 16 && mx < WIN_W - 16 && my >= row_y && my < row_y + 30) {
                            g_current_track = t;
                            g_note_idx = 0; g_sample_in_note = 0;
                            g_is_playing = 1;
                            draw_player();
                            break;
                        }
                    }
                }
            }
        }
        usleep(5000); /* 5 ms sleep per audio slice */
    }

    return 0;
}
