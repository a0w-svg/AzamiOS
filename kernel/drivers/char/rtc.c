#include <stdbool.h>
#include "./include/rtc.h"
#include "../arch/include/isr.h"
#include "../klibc/include/string.h"
#include "../klibc/include/port.h"
#include "../klibc/include/stdio.h"
#include "../mem/include/mmp.h"

#define CURRENT_YEAR 2000
#define CMOS_ADDRESS_REG 0x70
#define CMOS_DATA_REG 0x71

static time_t rtc_time_cache;
static int century_reg = 0x00;

static uint8_t reg_in(uint8_t reg)
{
    outb(CMOS_ADDRESS_REG, reg);
    return inb(CMOS_DATA_REG);
}

void rtc_send_value(uint8_t reg, uint8_t value)
{
    outb(CMOS_ADDRESS_REG, reg);
    outb(CMOS_DATA_REG, value);
}

uint8_t rtc_read_value(uint8_t reg)
{
    return reg_in(reg);
}

int get_up_prog_flag()
{
    outb(CMOS_ADDRESS_REG, 0x0A);
    return (inb(CMOS_DATA_REG) & 0x80);
}

static void rtc_read_datetime()
{
    uint8_t century = 0;
    uint8_t last_second, last_minute, last_hour, last_day, last_month, last_year, last_century;
    uint8_t reg_b;

    while(get_up_prog_flag());
    rtc_time_cache.second = reg_in(0x00);
    rtc_time_cache.minute = reg_in(0x02);
    rtc_time_cache.hour   = reg_in(0x04);
    rtc_time_cache.day    = reg_in(0x07);
    rtc_time_cache.month  = reg_in(0x08);
    rtc_time_cache.year   = reg_in(0x09);
    if(century_reg != 0) century = reg_in(century_reg);

    do {
        last_second  = rtc_time_cache.second;
        last_minute  = rtc_time_cache.minute;
        last_hour    = rtc_time_cache.hour;
        last_day     = rtc_time_cache.day;
        last_month   = rtc_time_cache.month;
        last_year    = rtc_time_cache.year;
        last_century = century;

        while(get_up_prog_flag());
        rtc_time_cache.second = reg_in(0x00);
        rtc_time_cache.minute = reg_in(0x02);
        rtc_time_cache.hour   = reg_in(0x04);
        rtc_time_cache.day    = reg_in(0x07);
        rtc_time_cache.month  = reg_in(0x08);
        rtc_time_cache.year   = reg_in(0x09);
        if(century_reg != 0) century = reg_in(century_reg);
    } while((last_second  != rtc_time_cache.second) || 
            (last_minute  != rtc_time_cache.minute) ||
            (last_hour    != rtc_time_cache.hour)   ||
            (last_day     != rtc_time_cache.day)    ||
            (last_month   != rtc_time_cache.month)  ||
            (last_year    != rtc_time_cache.year)   ||
            (last_century != century));

    reg_b = reg_in(0x0B);

    if(!(reg_b & 0x4))
    {
        rtc_time_cache.second = (rtc_time_cache.second & 0x0F) + ((rtc_time_cache.second >> 4) * 10);
        rtc_time_cache.minute = (rtc_time_cache.minute & 0x0F) + ((rtc_time_cache.minute >> 4) * 10);
        rtc_time_cache.hour   = ((rtc_time_cache.hour & 0x0F) + (((rtc_time_cache.hour & 0x70) >> 4) * 10)) | (rtc_time_cache.hour & 0x80);
        rtc_time_cache.day    = (rtc_time_cache.day & 0x0F) + ((rtc_time_cache.day >> 4) * 10);
        rtc_time_cache.month  = (rtc_time_cache.month & 0x0F) + ((rtc_time_cache.month >> 4) * 10);
        rtc_time_cache.year   = (rtc_time_cache.year & 0x0F) + ((rtc_time_cache.year >> 4) * 10);
        if(century_reg != 0) {
            century = (century & 0x0F) + ((century >> 4) * 10);
        }
    }

    if(!(reg_b & 0x2) && (rtc_time_cache.hour & 0x80)) {
        rtc_time_cache.hour = ((rtc_time_cache.hour & 0x7F) + 12) % 24;
    }

    if(century_reg != 0) {
        rtc_time_cache.year += century * 100;
    } else {
        rtc_time_cache.year += (CURRENT_YEAR / 100) * 100;
        if(rtc_time_cache.year < CURRENT_YEAR) rtc_time_cache.year += 100;
    }
}

void rtc_get_time(time_t* time)
{ 
    rtc_read_datetime();
    if(time) {
        memcpy(time, &rtc_time_cache, sizeof(time_t));
    }
}

static void rtc_handler(registers_t* r)
{
    UNUSED(r);
    rtc_read_datetime();
    outb(0x70, 0x0C);
    inb(0x71);
}

void rtc_init()
{
    rtc_read_datetime();
    register_interrupt_handler(IRQ8, rtc_handler);
    asm volatile ("cli");
    outb(0x70, 0x8B);
    char prev = inb(0x71);
    outb(0x70, 0x8B);
    outb(0x71, prev | 0x40);
    asm volatile("sti");
}