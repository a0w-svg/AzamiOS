/* ============================================================================
 * AzamiOS — Intel AC97 Audio Driver Header
 * File: drivers/sound/ac97.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../hal/pci.h"

#define AC97_NAMBAR_MASTER_VOL 0x02
#define AC97_NAMBAR_PCM_OUT_VOL 0x18
#define AC97_NAMBAR_EXT_AUDIO_ID 0x28
#define AC97_NAMBAR_EXT_AUDIO_CTRL 0x2A
#define AC97_NAMBAR_PCM_FRONT_RATE 0x2C

/* Bus Master Audio Offsets */
#define AC97_PO_BDBAR 0x10  /* PCM Out Buffer Descriptor list Base Address */
#define AC97_PO_CIV   0x14  /* PCM Out Current Index Value */
#define AC97_PO_LVI   0x15  /* PCM Out Last Valid Index */
#define AC97_PO_SR    0x16  /* PCM Out Status Register */
#define AC97_PO_PICB  0x18  /* PCM Out Position In Current Buffer */
#define AC97_PO_CR    0x1B  /* PCM Out Control Register */

#define AC97_BDL_IOC 0x8000 /* Interrupt On Completion */
#define AC97_BDL_BUP 0x4000 /* Buffer Under-run Policy */

/* Sound ioctl codes */
#define SOUND_PCM_WRITE_RATE   0x40045002
#define SOUND_PCM_READ_RATE    0x80045002
#define SOUND_PCM_WRITE_VOLUME 0x40045004
#define SOUND_PCM_READ_VOLUME  0x80045004

/* Buffer Descriptor List Entry */
typedef struct __packed {
    u32 ptr;      /* Physical address of the buffer */
    u16 samples;  /* Length in samples (usually length_in_bytes / 2) */
    u16 flags;
} ac97_bdl_entry_t;

void ac97_init(void);
