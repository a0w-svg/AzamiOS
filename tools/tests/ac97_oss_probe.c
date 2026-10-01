/* Stock Linux OSS client; no Azami headers or libc required. */
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { printf("FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno); failures++; } } while (0)
static void delay(void)
{
    struct timespec ts = { .tv_sec = 1 };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
static int16_t pcm[8192 * 2];
int main(void)
{
    setbuf(stdout, NULL);
    int fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0);
        int value = 0;
        CHECK(ioctl(fd, SNDCTL_DSP_GETFMTS, &value) == 0 && (value & AFMT_S16_LE));
        value = AFMT_S16_LE;
        CHECK(ioctl(fd, SNDCTL_DSP_SETFMT, &value) == 0 && value == AFMT_S16_LE);
        value = 2;
        CHECK(ioctl(fd, SNDCTL_DSP_CHANNELS, &value) == 0 && value == 2);
        value = 44100;
        CHECK(ioctl(fd, SNDCTL_DSP_SPEED, &value) == 0 && value == 44100);
        value = 0;
        CHECK(ioctl(fd, SOUND_PCM_READ_RATE, &value) == 0 && value == 44100);
        CHECK(ioctl(fd, SNDCTL_DSP_GETBLKSIZE, &value) == 0 && value == 4096);
        audio_buf_info info;
        CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.bytes == 32 * 4096);
        /* Two distinct channel amplitudes allow the host to verify playback
         * before and after DMA starvation, including stereo ordering. */
        for (int pass = 0; pass < 2; pass++) {
            for (unsigned i = 0; i < 8192; i++) {
                int sign = (i / 50) & 1 ? 1 : -1;
                pcm[2 * i] = sign * (pass ? 12000 : 6000);
                pcm[2 * i + 1] = sign * (pass ? 3000 : 1500);
            }
            CHECK(write(fd, pcm, sizeof(pcm)) == sizeof(pcm));
            delay(); /* drain completely, then append without reset */
            CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.fragments == 31);
        }
        CHECK(ioctl(fd, SNDCTL_DSP_RESET, 0) == 0);
        CHECK(ioctl(fd, SNDCTL_DSP_GETOSPACE, &info) == 0 && info.fragments == 32);
        close(fd);
    }
    printf("AC97 probe complete: %d failed\n", failures);
    for (;;) delay();
}
