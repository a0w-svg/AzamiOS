/* ============================================================================
 * AzamiOS Userspace — Audio Playback Utility (play.elf)
 * File: userland/apps/play/main.c
 * ============================================================================ */

#include "../../libc/include/stdio.h"
#include "../../libc/include/stdlib.h"
#include "../../libc/include/fcntl.h"
#include "../../libc/include/unistd.h"

#define SAMPLE_RATE 44100
#define FREQUENCY 440
#define DURATION_SEC 2

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("Playing 440Hz tone on /dev/dsp...");
    
    int fd = open("/dev/dsp", O_WRONLY, 0);
    if (fd < 0) {
        puts("Failed to open /dev/dsp");
        return 1;
    }

    int samples_per_cycle = SAMPLE_RATE / FREQUENCY;
    int half_cycle = samples_per_cycle / 2;
    
    short buffer[4096];
    int total_samples = SAMPLE_RATE * DURATION_SEC;
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
        
        write(fd, buffer, chunk * sizeof(short));
        written += chunk;
    }
    
    close(fd);
    puts("Playback finished.");
    return 0;
}
