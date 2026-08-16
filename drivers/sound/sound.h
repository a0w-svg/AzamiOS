/* ============================================================================
 * AzamiOS — Sound Subsystem Core Header
 * File: drivers/sound/sound.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../fs/vfs.h"

/* Simple sound operations */
typedef struct sound_ops {
    /* Play PCM data (blocks until written or buffer full, returns bytes written) */
    s64 (*write_pcm)(const u8 *data, u64 len);
    /* Control/setup (e.g. sample rate, channels, bit depth) */
    s64 (*ioctl)(u64 cmd, void *arg);
} sound_ops_t;

/* Sound Device registration */
typedef struct sound_device {
    char name[32];
    sound_ops_t *ops;
} sound_device_t;

/* Register a sound device. Currently supports one active device as /dev/dsp */
void sound_register_device(sound_device_t *dev);
