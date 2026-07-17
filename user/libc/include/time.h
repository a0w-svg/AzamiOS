/**
 * time.h — AzamiOS time header wrapper
 *
 * Includes newlib's standard <time.h> for POSIX time types and functions,
 * and adds AzamiOS-specific RTC structures and syscall wrappers.
 */

#ifndef _AZAMI_TIME_H
#define _AZAMI_TIME_H

/* Include system/newlib time.h */
#include_next <time.h>

#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} __attribute__((packed)) rtc_time_t;

void rtc_get_time(rtc_time_t *time);

#endif /* _AZAMI_TIME_H */
