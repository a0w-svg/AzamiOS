/* ============================================================================
 * AzamiOS — CPU Digital Thermal Sensor (coretemp) Driver
 * File: drivers/hwmon/coretemp.h
 * ============================================================================ */
#pragma once

#include "../../include/azami/types.h"

#define MSR_IA32_THERM_STATUS         0x019C
#define MSR_IA32_TEMPERATURE_TARGET   0x01A2

void coretemp_init(void);
bool coretemp_is_supported(void);
s32  coretemp_get_temp_celsius(u32 cpu);
s32  coretemp_get_temp_millicelsius(u32 cpu);
