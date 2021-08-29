#ifndef RTC_H
#define RTC_H
#include <stdint.h>
/*
    this is a returned rtc structure 
*/
typedef struct 
{
    uint8_t second;
    uint8_t minute;
    uint8_t hour;
    uint8_t day;
    uint8_t month;
    uint32_t year;
}time_t;
/*
    Get the current time from RTC;
*/
void rtc_get_time(time_t* time);
/*
    Initialize Real Time Clock;
*/
void rtc_init();
/*
    Send value to RTC reg;
*/
void rtc_send_value(uint8_t reg, uint8_t value);
/*
    Read value from RTC reg;
*/
uint8_t rtc_read_value(uint8_t reg);
/*
    Get update in progress flag;
*/
int get_up_prog_flag();
#endif