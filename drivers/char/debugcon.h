/* ============================================================================
 * AzamiOS — Bochs / QEMU Debug Console (debugcon) Driver
 * File: drivers/char/debugcon.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

#define DEBUGCON_PORT 0xE9

void debugcon_init(void);
bool debugcon_is_present(void);
void debugcon_putc(char c);
void debugcon_write(const char *buf, size_t len);
void debugcon_puts(const char *s);
