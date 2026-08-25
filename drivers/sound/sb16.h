/* ============================================================================
 * AzamiOS — Sound Blaster 16 Audio Driver
 * File: drivers/sound/sb16.h
 * ============================================================================ */

#ifndef _AZAMI_DRIVERS_SB16_H
#define _AZAMI_DRIVERS_SB16_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <azami/defs.h>

#define SB16_DEFAULT_BASE 0x220
#define SB16_DEFAULT_IRQ  5
#define SB16_DEFAULT_DMA8 1
#define SB16_DEFAULT_DMA16 5

void sb16_init(void);

#endif /* _AZAMI_DRIVERS_SB16_H */
