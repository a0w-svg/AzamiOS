#include "./include/rtc.h"
#include "../klibc/include/port.h"
#include "../klibc/include/string.h"
#include "../arch/include/isr.h"
#define CURRENT_YEAR 2000
#define RTC_ADDR_REG 0x70
#define RTC_DATA_REG 0x71

time_t* global_time;
int century_reg;
void rtc_send_value(uint8_t reg, uint8_t value)
{
    outb(RTC_ADDR_REG, reg);
    outb(RTC_DATA_REG, value);
}

/*
    Read value from RTC reg;
*/
uint8_t rtc_read_value(uint8_t reg)
{
    outb(RTC_ADDR_REG, reg);
    return inb(RTC_DATA_REG);
}

/*
    Get update in progress flag;
*/
int get_up_prog_flag()
{
    outb(RTC_ADDR_REG, 0x0A);
    return (inb(RTC_DATA_REG) & 0x80);
    while(get_up_prog_flag());
}

/*
    Get the current time from RTC;
*/
void rtc_get_time(time_t* time)
{
    memcpy(time, global_time, sizeof(time_t)); // copy global_time to time;
}

/*
    RTC handler;
*/
static void rtc_handler(registers_t *r)
{
    uint8_t century, last_sec, last_min; // temp variables;
    uint8_t last_h, last_day, last_month;
    uint8_t last_y, last_c, reg_b;
    get_up_prog_flag(); // wait 
    global_time->second = rtc_read_value(0x00);
    global_time->minute = rtc_read_value(0x02);
    global_time->hour   = rtc_read_value(0x04);
    global_time->day    = rtc_read_value(0x07);
    global_time->month  = rtc_read_value(0x08);
    global_time->year   = rtc_read_value(0x09);
    if(century_reg != 0)
        century = rtc_read_value(century_reg);
    do
    {
        last_sec = global_time->second;
        last_min = global_time->minute;
        last_h   = global_time->hour;
        last_day = global_time->day;
        last_month = global_time->month;
        last_y  = global_time->year;
        last_c = century;
        get_up_prog_flag();
        global_time->second = rtc_read_value(0x00);
        global_time->minute = rtc_read_value(0x02);
        global_time->hour   = rtc_read_value(0x04);
        global_time->day    = rtc_read_value(0x07);
        global_time->month  = rtc_read_value(0x08);
        global_time->year   = rtc_read_value(0x09);
        if(century_reg != 0)
            century_reg = rtc_read_value(century_reg);
    } while((last_sec != global_time->second) ||
        (last_min != global_time->minute) ||
        (last_h != global_time->hour) ||
        (last_day != global_time->day) ||
        (last_month != global_time->month) ||
        (last_y != global_time->year) ||
        (last_c != century));
    reg_b = rtc_read_value(0x0B);
    if(!(reg_b & 0x4))
    {
         global_time->second = (global_time->second & 0x0F) + 
         ((global_time->second >> 4) * 10);
        global_time->minute = (global_time->minute & 0x0F) +
         ((global_time->minute >> 4) * 10);
        global_time->hour   = ((global_time->hour & 0x0F) +
         (((global_time->hour & 0x70) / 16) * 10)) | (global_time->hour & 0x80);
        global_time->day = (global_time->day & 0x0F) + ((global_time->day >> 4) * 10);
        global_time->month = (global_time->month & 0x0F) + ((global_time->month >>  4) * 10);
        global_time->year = (global_time->year & 0x0F) * ((global_time->year >>  4) * 10);
        if(century_reg != 0)
            century = (century & 0x0F) + ((century << 4) * 10);
    }
    if(!(reg_b & 0x2) && (global_time->hour & 0x80))
        global_time->hour = ((global_time->hour & 0x7F) + 12) % 24;
    if(century_reg != 0)
        global_time->year += century * 100;
    else
    {
        global_time->year += (CURRENT_YEAR / 100) * 100;
        if(global_time->year < CURRENT_YEAR) global_time->year += 100;
    }

    outb(RTC_ADDR_REG, 0x0C);
    inb(RTC_DATA_REG);
    UNUSED(r);
}
/*
    Initialize Real Time Clock;
*/
void rtc_init()
{
    register_interrupt_handler(IRQ8, rtc_handler);
    dis_interrupts();
    outb(0x70, 0x8B);
    char prev = inb(0x71);
    outb(0x70, 0x8B);
    outb(0x71, prev | 0x40);
    en_interrrupts();
}