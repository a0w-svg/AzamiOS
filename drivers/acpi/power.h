/* ============================================================================
 * AzamiOS — Power Management & ACPI System Control Driver Header
 * File: drivers/acpi/power.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

/** power_shutdown() — Power off the system via QEMU port, ACPI S5, APM, or halt. */
__attribute__((noreturn)) void power_shutdown(void);

/** power_reboot() — Reset and reboot the computer via 8042, ACPI reset, or triple fault. */
__attribute__((noreturn)) void power_reboot(void);
