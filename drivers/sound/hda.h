/* ============================================================================
 * AzamiOS — Intel High Definition Audio (HDA / Azalia) Driver Header
 * File: drivers/sound/hda.h
 *
 * Implements the Intel High Definition Audio Specification (Rev 1.0a)
 * with full CORB/RIRB codec communication, widget configuration, and
 * Output Stream DMA with Buffer Descriptor Lists (BDL).
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/device.h"

#define HDA_PCI_CLASS 0x0403

/** hda_init() — Register the PCI driver; probe() binds to a matching Intel
 *  HDA controller automatically. */
void hda_init(void);

/** hda_play_pcm() — Output raw 16-bit 48kHz stereo PCM audio via DMA. */
s64 hda_play_pcm(const void *samples, size_t len);
