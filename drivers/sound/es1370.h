/* ============================================================================
 * AzamiOS — Ensoniq AudioPCI ES1370 / ES1371 Audio Driver
 * File: drivers/sound/es1370.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

void es1370_init(void);
s64  es1370_write_pcm(const u8 *data, u64 len);
