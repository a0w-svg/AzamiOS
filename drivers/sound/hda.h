/* ============================================================================
 * AzamiOS — Intel High Definition Audio (HDA / Azalia) Driver Header
 * File: drivers/sound/hda.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"

#define HDA_PCI_CLASS 0x0403

/** hda_init() — Register the PCI driver; probe() binds to a matching Intel
 *  HDA controller automatically, so this is safe to call whether or not the
 *  host has one. */
void hda_init(void);

/** hda_play_pcm() — Output raw 16-bit stereo PCM audio. */
s64 hda_play_pcm(const void *samples, size_t len);
