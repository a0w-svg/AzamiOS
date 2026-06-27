#ifndef AC97_H
#define AC97_H
#include <stdint.h>

/*
    Initialize the AC'97 sound driver;
    Probes PCI bus for Intel 82801AA AC'97 Audio (VID 0x8086, DID 0x2415),
    resets the codec, configures volumes, and sets up DMA descriptors;
*/
void ac97_init(void);

/*
    Set master playback volume;
    vol: 0 = maximum volume, 63 = mute (AC'97 attenuation scale);
*/
void ac97_set_volume(uint8_t vol);

/*
    Play signed 16-bit stereo PCM samples through the AC'97 DMA engine;
    samples    : pointer to interleaved L/R 16-bit signed samples;
    num_samples: total number of 16-bit samples (L+R combined);
    sample_rate: playback rate in Hz (48000 recommended, codec native rate);
*/
void ac97_play(const int16_t *samples, uint32_t num_samples, uint32_t sample_rate);

/*
    Stop any ongoing DMA playback and silence the output;
*/
void ac97_stop(void);

#endif
