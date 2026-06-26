#ifndef TIME_H
#define TIME_H

#include <stdint.h>

typedef struct {
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
} __attribute__((packed)) time_t;

void rtc_get_time(time_t* time);

#endif /* TIME_H */
