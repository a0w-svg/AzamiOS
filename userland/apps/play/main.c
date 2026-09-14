/* ============================================================================
 * AzamiOS Userspace — Audio Playback Utility (play.elf)
 * File: userland/apps/play/main.c
 *
 * Versatile command-line audio player supporting:
 *  • SIMD-accelerated real-time MP3 playback via minimp3
 *  • Direct WAV PCM playback
 *  • Default 440Hz calibration test tone when invoked without arguments
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/string.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define MINIMP3_IMPLEMENTATION
#include "../shared/minimp3.h"

#define DEFAULT_SAMPLE_RATE 44100
#define FREQUENCY 440
#define DURATION_SEC 2

static int play_mp3(const char *path, int dsp_fd, int max_sec)
{
    int file_fd = open(path, O_RDONLY, 0);
    if (file_fd < 0) {
        printf("Error: Could not open file '%s'\n", path);
        return 1;
    }

    /* Check ID3v2 tag at start */
    unsigned char id3_hdr[10];
    int n = (int)read(file_fd, id3_hdr, 10);
    if (n == 10 && id3_hdr[0] == 'I' && id3_hdr[1] == 'D' && id3_hdr[2] == '3') {
        int tag_size = ((id3_hdr[6] & 0x7f) << 21) |
                       ((id3_hdr[7] & 0x7f) << 14) |
                       ((id3_hdr[8] & 0x7f) << 7)  |
                       (id3_hdr[9] & 0x7f);
        lseek(file_fd, 10 + (off_t)tag_size, SEEK_SET);
    } else {
        lseek(file_fd, 0, SEEK_SET);
    }

    printf("Streaming MP3 audio: '%s' (SIMD accelerated)...\n", path);

    mp3dec_t dec;
    mp3dec_init(&dec);

    uint8_t inbuf[8192];
    int inbytes = 0;
    int inoff = 0;
    short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int total_frames = 0;
    int first_frame = 1;

    for (;;) {
        if (inbytes - inoff < 2048) {
            int rem = inbytes - inoff;
            if (rem > 0 && inoff > 0) {
                memmove(inbuf, inbuf + inoff, (size_t)rem);
            }
            inoff = 0;
            inbytes = rem;
            int rd = (int)read(file_fd, inbuf + inbytes, sizeof(inbuf) - (size_t)inbytes);
            if (rd > 0) inbytes += rd;
        }

        if (inbytes - inoff <= 0) break; /* EOF */

        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&dec,
                                          inbuf + inoff,
                                          inbytes - inoff,
                                          pcm,
                                          &info);
        inoff += info.frame_bytes;

        if (samples > 0) {
            if (first_frame) {
                printf("  • Audio Format: %d Hz, %d channel(s), %d kbps\n",
                       info.hz, info.channels, info.bitrate_kbps);
                first_frame = 0;
            }

            int pcm_bytes = samples * info.channels * (int)sizeof(short);
            int wr = 0;
            while (wr < pcm_bytes) {
                int w = (int)write(dsp_fd, (const char *)pcm + wr, (size_t)(pcm_bytes - wr));
                if (w <= 0) break;
                wr += w;
            }
            total_frames++;
            if (max_sec > 0 && (total_frames * samples) / (info.hz ? info.hz : 44100) >= max_sec) {
                break;
            }
        } else if (info.frame_bytes == 0) {
            inoff++;
        }
    }

    close(file_fd);
    printf("Playback finished (%d MP3 frames decoded).\n", total_frames);
    return 0;
}

static int play_wav(const char *path, int dsp_fd)
{
    int file_fd = open(path, O_RDONLY, 0);
    if (file_fd < 0) {
        printf("Error: Could not open file '%s'\n", path);
        return 1;
    }

    unsigned char hdr[44];
    int rd = (int)read(file_fd, hdr, 44);
    if (rd < 44 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        printf("Error: '%s' is not a valid RIFF/WAVE file\n", path);
        close(file_fd);
        return 1;
    }

    int sample_rate = (int)hdr[24] | ((int)hdr[25] << 8);
    int channels = (int)hdr[22] | ((int)hdr[23] << 8);
    int bits = (int)hdr[34] | ((int)hdr[35] << 8);

    printf("Streaming WAV audio: '%s' (%d Hz, %d-ch, %d-bit)...\n",
           path, sample_rate, channels, bits);

    char buf[4096];
    int total_bytes = 0;
    while ((rd = (int)read(file_fd, buf, sizeof(buf))) > 0) {
        int wr = 0;
        while (wr < rd) {
            int w = (int)write(dsp_fd, buf + wr, (size_t)(rd - wr));
            if (w <= 0) break;
            wr += w;
        }
        total_bytes += rd;
    }

    close(file_fd);
    printf("Playback finished (%d bytes played).\n", total_bytes);
    return 0;
}

static int play_tone(int dsp_fd)
{
    puts("Playing 440Hz calibration tone on /dev/dsp...");

    int samples_per_cycle = DEFAULT_SAMPLE_RATE / FREQUENCY;
    int half_cycle = samples_per_cycle / 2;

    short buffer[4096];
    int total_samples = DEFAULT_SAMPLE_RATE * DURATION_SEC;
    int written = 0;
    int t = 0;

    while (written < total_samples) {
        int chunk = 4096;
        if (total_samples - written < chunk) {
            chunk = total_samples - written;
        }

        for (int i = 0; i < chunk; i++) {
            if ((t % samples_per_cycle) < half_cycle) {
                buffer[i] = 16000;
            } else {
                buffer[i] = -16000;
            }
            t++;
        }

        write(dsp_fd, buffer, chunk * sizeof(short));
        written += chunk;
    }

    puts("Tone playback finished.");
    return 0;
}

int main(int argc, char **argv)
{
    int dsp_fd = open("/dev/dsp", O_WRONLY, 0);
    if (dsp_fd < 0) {
        puts("Error: Failed to open /dev/dsp sound device");
        return 1;
    }

    int ret = 0;
    if (argc > 1) {
        const char *path = argv[1];
        int max_sec = (argc > 2) ? atoi(argv[2]) : 0;
        size_t len = strlen(path);
        if (len >= 4 && strcasecmp(path + len - 4, ".mp3") == 0) {
            ret = play_mp3(path, dsp_fd, max_sec);
        } else if (len >= 4 && strcasecmp(path + len - 4, ".wav") == 0) {
            ret = play_wav(path, dsp_fd);
        } else {
            /* Try mp3 first, then wav */
            if (play_mp3(path, dsp_fd, max_sec) != 0) {
                ret = play_wav(path, dsp_fd);
            }
        }
    } else {
        ret = play_tone(dsp_fd);
    }

    close(dsp_fd);
    return ret;
}
