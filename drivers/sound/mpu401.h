/* ============================================================================
 * AzamiOS — MPU-401 MIDI Interface Driver Header
 * File: drivers/sound/mpu401.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/defs.h"
#include "../../drivers/base/platform.h"

#define MPU401_DEFAULT_DATA_PORT    0x330
#define MPU401_DEFAULT_CMD_PORT     0x331

#define MPU401_CMD_RESET            0xFF
#define MPU401_CMD_UART_MODE        0x3F
#define MPU401_ACK                  0xFE

#define MPU401_STATUS_OUTPUT_BUSY   0x40
#define MPU401_STATUS_INPUT_EMPTY   0x80

/** mpu401_write_byte(byte) — Send one MIDI byte over MPU-401 UART. */
void mpu401_write_byte(u8 byte);

/** mpu401_init() — Initialize MPU-401 MIDI controller and register /dev/midi. */
int mpu401_init(void);
