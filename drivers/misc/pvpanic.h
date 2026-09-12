/* ============================================================================
 * AzamiOS — QEMU Paravirtualized Panic (pvpanic) Driver
 * File: drivers/misc/pvpanic.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

#define PVPANIC_PORT             0x505

/* Events supported by QEMU pvpanic */
#define PVPANIC_PANICKED         (1U << 0)
#define PVPANIC_CRASHLOADED      (1U << 1)

void pvpanic_init(void);
bool pvpanic_is_present(void);
void pvpanic_notify(u8 event);
