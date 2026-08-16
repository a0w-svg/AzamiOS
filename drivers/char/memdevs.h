/* ============================================================================
 * AzamiOS — Standard Memory Devices Driver Header (/dev/null, /dev/zero, etc.)
 * File: drivers/char/memdevs.h
 * ============================================================================ */
#pragma once

/** memdevs_init() — Register /dev/null, /dev/zero, /dev/full, /dev/random, /dev/urandom. */
void memdevs_init(void);
