/* ============================================================================
 * AzamiOS — ACPI Power Supply (Battery & AC Adapter) Header
 * File: drivers/acpi/battery.h
 *
 * Implements Linux-compatible power supply subsystem for laptops and desktop
 * PCs, exposing /sys/class/power_supply/BAT0 and /sys/class/power_supply/AC.
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"
#include "../../include/azami/defs.h"

typedef enum {
    BATTERY_STATUS_UNKNOWN,
    BATTERY_STATUS_CHARGING,
    BATTERY_STATUS_DISCHARGING,
    BATTERY_STATUS_NOT_CHARGING,
    BATTERY_STATUS_FULL,
} battery_status_t;

typedef struct battery_info {
    bool             present;
    battery_status_t status;
    u32              capacity_percent;   /* 0 - 100 */
    u32              voltage_now_uv;     /* Microvolts */
    u32              current_now_ua;     /* Microamps */
    u32              design_capacity_mah;
    char             technology[16];
    char             model[32];
    char             serial[32];
} battery_info_t;

typedef struct ac_adapter_info {
    bool online;
} ac_adapter_info_t;

/** battery_init() — Register power supply class and probe ACPI battery/AC. */
void battery_init(void);

/** battery_get_info(info) — Fill current battery state. */
int battery_get_info(battery_info_t *info);

/** ac_adapter_is_online() — Return true if AC power is connected. */
bool ac_adapter_is_online(void);
