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
#define AC97_EXT_VRA 0x0001

#define AC97_GLOB_CNT 0x2C
#define AC97_GLOB_STA 0x30
#define AC97_CAS      0x34
#define AC97_GLOB_COLD 0x00000002
#define AC97_GLOB_WARM 0x00000004
#define AC97_GLOB_READY 0x00000100
#define AC97_GLOB_RCS 0x00008000

/* Bus Master Audio Offsets */
#define AC97_PO_BDBAR 0x10  /* PCM Out Buffer Descriptor list Base Address */
#define AC97_PO_CIV   0x14  /* PCM Out Current Index Value */
#define AC97_PO_LVI   0x15  /* PCM Out Last Valid Index */
#define AC97_PO_SR    0x16  /* PCM Out Status Register */
#define AC97_PO_PICB  0x18  /* PCM Out Position In Current Buffer */
#define AC97_PO_CR    0x1B  /* PCM Out Control Register */

#define AC97_SR_DCH   0x01
#define AC97_SR_CELV  0x02
#define AC97_SR_ACK   0x1C
#define AC97_CR_RUN   0x01
#define AC97_CR_RESET 0x02

#define AC97_BDL_IOC 0x8000 /* Interrupt On Completion */
#define AC97_BDL_BUP 0x4000 /* Buffer Under-run Policy */

/* Sound ioctl codes */
#define SOUND_PCM_WRITE_RATE   0x40045002
#define SOUND_PCM_READ_RATE    0x80045002
#define SOUND_PCM_WRITE_VOLUME 0x40045004
#define SOUND_PCM_READ_VOLUME  0x80045004

/* Linux x86_64 OSS ABI. Keep the historical Azami volume/rate requests
 * above: the desktop uses them and their direction bits differ from OSS. */
#define AC97_DSP_RESET       0x00005000
#define AC97_DSP_SPEED       0xC0045002
#define AC97_DSP_STEREO      0xC0045003
#define AC97_DSP_GETBLKSIZE  0xC0045004
#define AC97_DSP_SETFMT      0xC0045005
#define AC97_DSP_CHANNELS    0xC0045006
#define AC97_DSP_GETFMTS     0x8004500B
#define AC97_DSP_GETOSPACE   0x8010500C
#define AC97_PCM_READ_BITS  0x80045005
#define AC97_PCM_READ_CHANNELS 0x80045006
#define AC97_AFMT_S16_LE     0x10

typedef struct {
    s32 fragments;
    s32 fragstotal;
    s32 fragsize;
    s32 bytes;
} ac97_audio_buf_info_t;

/* Buffer Descriptor List Entry */
typedef struct __packed {
    u32 ptr;      /* Physical address of the buffer */
    u16 samples;  /* Length in samples (usually length_in_bytes / 2) */
    u16 flags;
} ac97_bdl_entry_t;

void ac97_init(void);
